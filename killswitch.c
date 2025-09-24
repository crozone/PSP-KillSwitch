// PSP-KillSwitch v1.3
// .prx plugin that stops the power switch from putting the unit to sleep,
// unless a button combo is held down.
//
// The power LED flashes rapidly to alert the user that the sleep is being blocked.
//
// Ryan Crosby 2025

#ifdef DEBUG
#include <pspdebug.h>
#include <pspdisplay.h>
#endif

#include <pspsdk.h>
#include <psppower.h>
#include <pspsysevent.h>
#include <pspctrl.h>
#include <pspkerror.h>
#include <psptypes.h>
#include <pspkerneltypes.h>
#include <pspthreadman.h>
#include <pspsyscon.h>

#include <stdbool.h>
#include <inttypes.h>

#define str(s) #s // For stringizing defines
#define xstr(s) str(s)

#ifdef DEBUG
#define DEBUG_PRINT(...) pspDebugScreenKprintf( __VA_ARGS__ )
#else
#define DEBUG_PRINT(...) do{ } while ( 0 )
#endif

#define ONE_MSEC (1000)
#define LED_BLINK_PERIOD (60 * ONE_MSEC)

// Maximum sleep attempts that can be blocked in a row, before sleep is allowed as a failsafe.
#define MAX_CONSECUTIVE_SLEEP_ATTEMPTS 10

#define MODULE_NAME "KillSwitch"
#define MAJOR_VER 1
#define MINOR_VER 3

#define MODULE_OK       0
#define MODULE_ERROR    1

// https://github.com/uofw/uofw/blob/7ca6ba13966a38667fa7c5c30a428ccd248186cf/include/common/errors.h
#define SCE_ERROR_OK                                0x0
#define SCE_ERROR_BUSY                              0x80000021

// https://github.com/uofw/uofw/blob/7ca6ba13966a38667fa7c5c30a428ccd248186cf/include/sysmem_sysevent.h#L7-L83
#define SCE_SUSPEND_EVENTS                          0x0000FF00
#define SCE_SYSTEM_SUSPEND_EVENT_QUERY              0x00000100
#define SCE_SYSTEM_SUSPEND_EVENT_CANCELLATION       0x00000101
#define SCE_SYSTEM_SUSPEND_EVENT_START              0x00000102

// The SCE_LED_POWER macro from pspsyscon.h is defined as 1, however that appears to be incorrect
// for the Power LED and is actually for the WLAN LED.
// The Power LED is really 2.
#define POWER_LED 2

//
// KillSwitch (non-hold) specific defines:
//

// Allow the switch to work when this button combo is pressed.
// Hold HOME + Power Switch to sleep.
// See https://pspdev.github.io/pspsdk/group__Ctrl.html#gac080131ea3904c97efb6c31b1c4deb10 for button constants
#define BUTTON_COMBO_MASK PSP_CTRL_HOME

//
// PSP SDK
//
// We are building a kernel mode prx plugin
PSP_MODULE_INFO(MODULE_NAME, PSP_MODULE_KERNEL, MAJOR_VER, MINOR_VER);

// We don't allocate any heap memory, so set this to 0.
PSP_HEAP_SIZE_KB(0);
//PSP_MAIN_THREAD_ATTR(0);
//PSP_MAIN_THREAD_NAME(MODULE_NAME);

// We don't need a main thread since we only do basic setup during module start and won't stall module loading.
// This will make us be called from the module loader thread directly, instead of a secondary kernel thread.
PSP_NO_CREATE_MAIN_THREAD();

// We don't need any of the newlib features since we're not calling into stdio or stdlib etc
PSP_DISABLE_NEWLIB();

//
// Forward declarations
//
static int sys_event_handler(int ev_id, char *ev_name, void *param, int *result);
static int power_callback_handler(int unknown, int pwrflags, void *p_common);
static int timer_callback_handler(int unknown, int vtid, void *p_common);
static SceUInt led_vtimer_handler(SceUID vtid, SceInt64 schedule, SceInt64 actual, void *p_common);

static int main_thread(SceSize args, void *argp);
static SceUID create_led_timer(SceUID* p_callback_id);
static int delete_led_timer(SceUID vtid);
static int start_main_thread(void);
static int stop_main_thread(void);
int module_start(SceSize args, void *argp);
int module_stop(SceSize args, void *argp);

//
// Globals
//
// A note about thread safety:
//
// g_sleep_inhibits_count is shared between power_callback_handler and sys_event_handler.
// It is atomically set by power_callback_handler.
// sys_event_handler tests and decrements g_sleep_inhibits_count, however sys_event_handler
// is called with interrupts completely disabled so the entire operation is atomic.
//
// g_led_timer_active is shared between power_callback_handler and timer_callback_handler.
// Since this is always accessed by callbacks run from the single main thread, it is not declared volatile.
//
// g_timerCallbackId is set once within main_thread and used by timer_callback_handler (which is called from main_thread)
// g_mainThreadId is only used by module_start and module_stop.
//
static unsigned int g_sleep_inhibits_count = 0;
static bool g_led_timer_active = false;
static SceUID g_timerCallbackId = -1;
static SceUID g_mainThreadId = -1;

// int(* PspSysEventHandlerFunc) (int ev_id, char *ev_name, void *param, int *result)
static
int sys_event_handler(int ev_id, char *ev_name, void *param, int *result)
{
    // This sysevent handler is called in kernel mode with interrupts completely disabled.
    // It will not be pre-empted or interrupted.
    // It can therefore assume exclusive control over global variables (g_sleep_inhibits_count)
    // without using any additional synchronisation mechanisms.

    //DEBUG_PRINT("Got SysEvent 0x%08x - %s\n", ev_id, ev_name);

    // Trap SCE_SYSTEM_SUSPEND_EVENT_QUERY
    // This is the ScePowerMain thread asking us "is it okay to sleep?"
    if(ev_id == SCE_SYSTEM_SUSPEND_EVENT_QUERY) {
        // Instead of using a boolean "allow_sleep" variable, we instead have a limited count of
        // sleep inhibits we are allowed to use, before we will give up and allow the sleep anyway.
        //
        // This is done because many external sources of sleep requests (headphone remote / cradle remote / any software that calls
        // scePowerRequestSuspend() or scePowerRequestStandby()) trigger sleep requests in an infinite loop until sleep is allowed,
        // which if blocked indefinitely, crash the PSP and trigger the system watchdog.
        //
        // By using a limited count of sleep inhibits, in the worst case we will only ever block up to MAX_CONSECUTIVE_SLEEP_ATTEMPTS,
        // and external sleep sources will function as expected.
        if(g_sleep_inhibits_count) {
            --g_sleep_inhibits_count;
            DEBUG_PRINT("Blocked suspend query 0x%08x - %s (%i remaining)\n", ev_id, ev_name, g_sleep_inhibits_count);
            return SCE_ERROR_BUSY;
        }
        else {
            DEBUG_PRINT("Accepted suspend query 0x%08x - %s\n", ev_id, ev_name);
            return SCE_ERROR_OK;
        }
    }
    else if(ev_id == SCE_SYSTEM_SUSPEND_EVENT_CANCELLATION) {
        DEBUG_PRINT("Got suspend cancelled event 0x%08x - %s\n", ev_id, ev_name);
    }
    else if(ev_id == SCE_SYSTEM_SUSPEND_EVENT_START) {
        DEBUG_PRINT("Got suspend start event 0x%08x - %s\n", ev_id, ev_name);
    }

    return SCE_ERROR_OK;
}

// Power Callback handler
// int (*SceKernelCallbackFunction)(int arg1, int arg2, void *arg)
static
int power_callback_handler(int unknown, int pwrflags, void *p_common)
{
    static bool power_switch_pressed = false;

    if (pwrflags & PSP_POWER_CB_POWER_SWITCH) {
        if(!power_switch_pressed) {
            // This is called immediately as the switch is pressed.
            // The SysEventHandler is called when the power switch is released, or held down for a second.
            // This gives us a chance to get in before it and decide whether to allow the sleep.
            DEBUG_PRINT("Power switch pressed.\n");

            // Check if the user is pressing the override key combination
            SceCtrlData pad_state;
            if(sceCtrlPeekBufferPositive(&pad_state, 1) >= 0) {
                if((pad_state.Buttons & BUTTON_COMBO_MASK) == BUTTON_COMBO_MASK) {
                    DEBUG_PRINT("Override key pressed, enabling sleep.\n");
                    g_sleep_inhibits_count = 0;
                }
                else {
                    DEBUG_PRINT("Disabling sleep.\n");
                    g_sleep_inhibits_count = MAX_CONSECUTIVE_SLEEP_ATTEMPTS;

                    // Start the LED blink timer
                    // The vtimer id is stored at the common pointer location
                    SceUID* vtid_ptr = (SceUID*)p_common;
                    SceUID vtid = vtid_ptr != NULL ? *vtid_ptr : -1;
                    if(vtid >= 0) {
                        DEBUG_PRINT("Start LED timer\n");
                        g_led_timer_active = true;
                        sceKernelStartVTimer(vtid);
                    }
                }
            }
            else {
                // There was an error reading button state. Allow sleep in this case.
                DEBUG_PRINT("Failed to read button state! Enabling sleep.\n");
                g_sleep_inhibits_count = 0;
            }
        }

        power_switch_pressed = true;
    }
    else {
        if(power_switch_pressed) {
            DEBUG_PRINT("Power switch released.\n");
        }

        power_switch_pressed = false;
        g_led_timer_active = false;

        // Here, we used to set g_sleep_inhibits_count to zero, to re-enable sleep immediately upon the power switch being released.
        // This was done because many external sources of sleep requests (headphone remote / cradle remote / any software that called
        // scePowerRequestSuspend() or scePowerRequestStandby()) would trigger a sleep request in an infinite loop until sleep was allowed,
        // which if blocked indefinitely, would crash the PSP and trigger the system watchdog.
        // In order to avoid this at all costs, g_sleep_inhibits_count was set to zero to make sure these sleeps were allowed immediately
        // and completely unimpeded.
        //
        // However, there was yet another edgecase with this strategy.
        // If an application/game happened to take an scePowerLock() (very common when saving to or loading from the memory card),
        // and then a sleep was requested during this power lock, the power lock would remember the sleep request
        // and re-trigger the sleep as soon as the power lock was released.
        // This would usually occur after the power switch was released when sleep was no longer being blocked by us,
        // and the PSP would go to sleep undesirably.
        //
        // In order to overcome this edgecase, we simply don't zero out g_sleep_inhibits_count here. As long as at least one sleep inhibit is
        // left over in g_sleep_inhibits_count, it will block the sleep request raised by the power lock being released, and the PSP won't go to sleep.
        //
        // If an external sleep source triggers a sleep after this, it will only ever be blocked a maximum of MAX_CONSECUTIVE_SLEEP_ATTEMPTS times,
        // and the PSP will go to sleep as expected.
    }

    return 0;
}

// Timer Callback handler
// int (*SceKernelCallbackFunction)(int arg1, int arg2, void *arg)
static
int timer_callback_handler(int unknown, int vtid, void *p_common)
{
    static bool led_is_on = true;

    bool timer_active = g_led_timer_active;
    int led_ret;

    if(timer_active && led_is_on) {
        // Turn the LED off
        led_is_on = false;

        led_ret = sceSysconCtrlLED(
            POWER_LED,
            LED_OFF
        );

        if(led_ret != 0) {
            DEBUG_PRINT("LED OFF error: %d\n", led_ret);
        }
    }
    else {
        if(!timer_active) {
            if(vtid >= 0) {
                DEBUG_PRINT("Stop LED timer\n");
                sceKernelStopVTimer((SceUID)vtid);
            }
        }

        // Turn the LED on
        led_is_on = true;

        led_ret = sceSysconCtrlLED(
            POWER_LED,
            LED_ON
        );

        if(led_ret != 0) {
            DEBUG_PRINT("LED ON error: %d\n", led_ret);
        }
    }

    return 0;
}

// typedef unsigned int (*SceKernelVTimerHandlerWide)(SceUID vtid, unsigned long long *schedule, unsigned long long *actual, void *p_common);
static
SceUInt led_vtimer_handler(SceUID vtid, SceInt64 schedule, SceInt64 actual, void *p_common) {
    // This handler is run in user mode, so don't call any kernel mode functions from here directly.
    // Instead, notify a callback, which is run on the kernel callback handling thread we have set up.

    // We don't know what *p_common actually is yet (it's definitely not the pointer we provided to the hander registration function).
    // So, for now, pass the callback id to run into this hander using a global variable.
    int cbid = g_timerCallbackId;
    //int* cbid_ptr = (int*)p_common;
    //int cbid = cbid_ptr != NULL ? *cbid_ptr : -1;
    if(cbid >= 0) {
        int callback_ret = sceKernelNotifyCallback(cbid, vtid);

        if(callback_ret < 0) {
            DEBUG_PRINT("Callback ret %d\n", callback_ret);
            return 0;
        }
    }

    // Return time to next invocation
    return LED_BLINK_PERIOD;
}

// The main thread.
// * Sets up callbacks and timers
// * Sleeps and processes callbacks
// * Cleans up when awoken.
static
int main_thread(SceSize args, void *argp)
{
    SceUID timer_cbid = -1;
    SceUID led_vtid = -1;
    SceUID power_cbid = -1;
    int slot;
    int result;

    // Our PspSysEventHandler to receive power events
    PspSysEventHandler sys_event = {
        .size = sizeof(PspSysEventHandler),
        .name = MODULE_NAME " SysEvent Handler", // Arbitrary string, doesn't appear to be used for anything
        .type_mask = SCE_SUSPEND_EVENTS,
        .handler = sys_event_handler,
        .r28 = 0,
        .busy = 0,
        .next = NULL,
        .reserved = {
            [0] = 0,
            [1] = 0,
            [2] = 0,
            [3] = 0,
            [4] = 0,
            [5] = 0,
            [6] = 0,
            [7] = 0,
            [8] = 0,
        }
    };

    //
    // Setup
    //
    DEBUG_PRINT("Creating timer callback\n");
    timer_cbid = sceKernelCreateCallback(MODULE_NAME " Timer Callback", timer_callback_handler, NULL);
    if(timer_cbid >= 0) {
        // Set a global variable and reference the global variable from the handler.
        // Hacky, but works for now.
        g_timerCallbackId = timer_cbid;
        led_vtid = create_led_timer(&g_timerCallbackId);
        //led_vtid = create_led_timer(&timer_cbid);
    }
    else {
        DEBUG_PRINT("Failed to create timer callback: ret 0x%08x\n", timer_cbid);
    }

    DEBUG_PRINT("Creating power callback\n");
    power_cbid = sceKernelCreateCallback(MODULE_NAME " Power Callback", power_callback_handler, &led_vtid);
    if(power_cbid >= 0) {
        // -1 for slot autoassignment doesn't appear to work, so search backwards for an available slot manually
        for(slot = 15; slot >= 0; slot--) {
            DEBUG_PRINT("Registering power callback in slot %i\n", slot);
            result = scePowerRegisterCallback(slot, power_cbid);
            if(result >= 0) {
                DEBUG_PRINT("Power callback successfully registered in slot %i\n", slot);
                break;
            }
            else {
                DEBUG_PRINT("Failed to register power callback in slot %i: ret 0x%08x\n", slot, result);
            }
        }

        if(slot < 0) {
            DEBUG_PRINT("Failed to register power callback in any slot!\n");
        }
    }
    else {
        DEBUG_PRINT("Failed to create power callback: ret 0x%08x\n", power_cbid);
    }

    DEBUG_PRINT("Registering sysevent handler\n");
    result = sceKernelRegisterSysEventHandler(&sys_event);
    if(result < 0) {
        DEBUG_PRINT("Failed to register sysevent handler: ret 0x%08x\n", result);
    }

    //
    // Sleep and process callbacks until we get woken up
    //
    DEBUG_PRINT("Now processing callbacks\n");
    sceKernelSleepThreadCB();

    //
    // Cleanup
    //
    DEBUG_PRINT("Unregistering sysevent handler\n");
    result = sceKernelUnregisterSysEventHandler(&sys_event);
    if(result < 0) {
        DEBUG_PRINT("Failed to unregister sysevent handler: ret 0x%08x\n", result);
    }

    if(slot >= 0) {
        result = scePowerUnregisterCallback(slot);
        if(result < 0) {
            DEBUG_PRINT("Failed to unregister power callback from slot %i: ret 0x%08x\n", slot, result);
        }
    }

    if(power_cbid >= 0) {
        DEBUG_PRINT("Deleting power callback\n");
        result = sceKernelDeleteCallback(power_cbid);
        if(result < 0) {
            DEBUG_PRINT("Failed to delete power callback: ret 0x%08x\n", result);
        }
    }

    if(led_vtid >= 0) {
        delete_led_timer(led_vtid);
    }

    if(timer_cbid >= 0) {
        DEBUG_PRINT("Deleting timer callback\n");
        result = sceKernelDeleteCallback(timer_cbid);
        if(result < 0) {
            DEBUG_PRINT("Failed to delete timer callback: ret 0x%08x\n", result);
        }
    }

    return 0;
}

static
SceUID create_led_timer(SceUID* p_callback_id)
{
    DEBUG_PRINT("Registering LED timer\n");
    SceUID vtid = sceKernelCreateVTimer(MODULE_NAME " LED Timer", NULL);

    if(vtid >= 0) {
        int set_vtimer_handle_ret;

        // Stop the timer
        sceKernelStopVTimer(vtid);

        // Reset the timer time to zero
        sceKernelSetVTimerTimeWide(vtid, sceKernelUSec2SysClockWide(0));

        // Register callback on created vtimer.
        // Use Wide version so we get to use SceInt64 for time instead of *SceKernelSysClock
        set_vtimer_handle_ret = sceKernelSetVTimerHandlerWide(
            vtid,
            sceKernelUSec2SysClockWide(10 * ONE_MSEC),
            led_vtimer_handler,
            p_callback_id // TODO: Doesn't get passed to handler directly, the ptr changes.
            );

        if(set_vtimer_handle_ret < 0) {
            DEBUG_PRINT("Failed to register LED timer handler: ret 0x%08x\n", set_vtimer_handle_ret);
        }
    }
    else {
        DEBUG_PRINT("Failed to create LED timer: ret 0x%08x\n", vtid);
    }

    return vtid;
}

static
int delete_led_timer(SceUID vtid)
{
    DEBUG_PRINT("Deleting LED timer\n");
    int delete_vtimer_ret = sceKernelDeleteVTimer(vtid);
    if(delete_vtimer_ret < 0) {
        DEBUG_PRINT("Failed to delete LED timer handler: ret 0x%08x\n", delete_vtimer_ret);
    }

    return delete_vtimer_ret;
}

static
int start_main_thread(void)
{
    int result;
    SceUID thid;

    // name, entry, initPriority, stackSize, PspThreadAttributes, SceKernelThreadOptParam
    thid = sceKernelCreateThread(MODULE_NAME "MainThread", main_thread, 0x11, 0x800, 0, 0);
    if (thid >= 0) {
        DEBUG_PRINT("Starting main thread\n");
        result = sceKernelStartThread(thid, 0, 0);
        if(result < 0) {
            DEBUG_PRINT("Failed to start main thread: ret 0x%08x\n", result);
        }

        g_mainThreadId = thid;
    }
    else {
        result = thid;
        DEBUG_PRINT("Failed to create main thread: ret 0x%08x\n", result);
    }

    return result;
}

static
int stop_main_thread(void)
{
    int result = 0;
    SceUID thid = g_mainThreadId;

    if(thid >= 0) {
        // Unblock sceKernelSleepThreadCB() and have thread begin cleanup
        result = sceKernelWakeupThread(thid);
        if(result < 0) {
            DEBUG_PRINT("Failed to wakeup main thread: ret 0x%08x\n", result);
        }

        // Wait for the main thread to clean up and exit
        DEBUG_PRINT("Waiting for main thread exit ...\n");
        result = sceKernelWaitThreadEnd(thid, NULL);
        if(result < 0) {
            // Thread did not stop, force terminate and delete it
            DEBUG_PRINT("Failed to wait for main thread exit: ret 0x%08x\n", result);
            DEBUG_PRINT("Terminating and deleting main thread\n", result);
            result = sceKernelTerminateDeleteThread(thid);
            if(result >= 0) {
                g_mainThreadId = -1;
            }
            else {
                DEBUG_PRINT("Failed to terminate delete callback thread: ret 0x%08x\n", result);
            }
        }
        else {
            DEBUG_PRINT("Deleting main thread\n");
            // Thead stopped cleanly, delete it
            result = sceKernelDeleteThread(thid);
            if(result >= 0) {
                DEBUG_PRINT("Main thread cleanup complete.\n");
                g_mainThreadId = -1;
            }
            else {
                DEBUG_PRINT("Failed to delete main thread: ret 0x%08x\n", result);
            }
        }
    }

    return result;
}

// Called during module init
int module_start(SceSize args, void *argp)
{
    int result;

    #ifdef DEBUG
    pspDebugScreenInit();
    #endif

    DEBUG_PRINT(MODULE_NAME " v" xstr(MAJOR_VER) "." xstr(MINOR_VER) " Module Start\n");

    result = start_main_thread();
    if(result < 0) {
        return MODULE_ERROR;
    }

    DEBUG_PRINT("Started.\n");

    return MODULE_OK;
}

// Called during module deinit
int module_stop(SceSize args, void *argp)
{
    int result;

    DEBUG_PRINT("Stopping ...\n");

    result = stop_main_thread();
    if(result < 0) {
        return MODULE_ERROR;
    }

    DEBUG_PRINT(MODULE_NAME " v" xstr(MAJOR_VER) "." xstr(MINOR_VER) " Module Stop\n");

    return MODULE_OK;
}
