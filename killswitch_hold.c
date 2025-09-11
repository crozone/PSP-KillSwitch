// PSP-KillSwitchHold v1.3
// .prx plugin that stops the power switch from putting the unit to sleep immediately after hold is deactivated.
// This prevents the issue of accidentally sleeping the PSP when deactivating hold and overshooting the detent.
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
#include <psputils.h> // time_t

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

// Disable sleep for 0.5 seconds after hold is deactivated
#define DISABLE_DURATION_MS 500
#define DISABLE_DURATION (DISABLE_DURATION_MS * ONE_MSEC)
#define MAX_CONSECUTIVE_SLEEP_ATTEMPTS 10

#define MODULE_NAME "KillSwitchHold"
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

static int sys_event_handler(int ev_id, char *ev_name, void *param, int *result);
static int power_callback_handler(int unknown, int pwrflags, void *p_common);

static int main_thread(SceSize args, void *argp);
static int start_main_thread(void);
static int stop_main_thread(void);
int module_start(SceSize args, void *argp);
int module_stop(SceSize args, void *argp);

// Globals.
//
// A note about thread safety:
//
// g_sleep_inhibits_count is shared between power_callback_handler and sys_event_handler.
// It is atomically set by power_callback_handler.
// sys_event_handler tests and decrements g_sleep_inhibits_count, however sys_event_handler
// is called with interrupts completely disabled so the entire operation is atomic.
//
// g_mainThreadId is only used by module_start and module_stop.

unsigned int g_sleep_inhibits_count = 0;
SceUID g_mainThreadId = -1;

// Our PspSysEventHandler to receive power events
PspSysEventHandler g_sys_event = {
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
        // Instead of using a boolean "allow_sleep" variable, we have a limited count of
        // sleep inhibits we are allowed to use, before we will give up and allow the sleep.
        //
        // This is done to avoid crashing the PSP in edgecases that cause infinite sleep request loops,
        // where the system won't take no for an answer.
        // For example, if the user triggers standby from a dock while holding the power switch up.
        // If this occurs, we will only loop until we run out of sleep inhibits count, and then allow
        // the sleep to occur as a failsafe.
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
int power_callback_handler(int unknown, int pwrflags, void *p_common)
{
    static bool hold_active = false;
    static clock_t hold_release_timestamp = 0;

    clock_t current_timestamp = sceKernelLibcClock();

    if(pwrflags & PSP_POWER_CB_HOLD_SWITCH) {
        if(!hold_active) {
            // User just switched hold ON.
            DEBUG_PRINT("Hold activated.\n");
        }

        hold_active = true;
        hold_release_timestamp = 0;
    }
    else {
        if(hold_active) {
            // User just switched hold OFF.
            DEBUG_PRINT("Hold deactivated.\n");
            hold_release_timestamp = current_timestamp;
        }

        hold_active = false;
    }

    if (pwrflags & PSP_POWER_CB_POWER_SWITCH) {
        // This is called immediately as the switch is pressed.
        // The SysEventHandler is called when the power switch is released, or held down for a second.
        // This gives us a chance to get in before it and decide whether to allow the sleep.

        DEBUG_PRINT("Power switch pressed.\n");

        // Check if the hold switch was recently pressed
        clock_t hold_released_duration = current_timestamp - hold_release_timestamp;
        if((hold_release_timestamp != 0) && (hold_released_duration < DISABLE_DURATION)) {
            DEBUG_PRINT("Hold recently pressed (%ims < " xstr(DISABLE_DURATION_MS) "ms), disallowing sleep.\n", (hold_released_duration / 1000));
            g_sleep_inhibits_count = MAX_CONSECUTIVE_SLEEP_ATTEMPTS;
        }
        else {
            DEBUG_PRINT("Hold not recently pressed, allowing sleep.\n");
            g_sleep_inhibits_count = 0;
        }
    }
    else {
        // If the physical power switch isn't currently pressed, this means any suspend or standby command
        // will be coming from an event that wasn't the user hitting the power switch
        // (eg a PSP HP Remote or Cradle command, or PSPLINK poweroff command).
        //
        // We need to always allow suspend or standby from these other places, because if we don't,
        // sleep is re-attempted and SCE_SYSTEM_SUSPEND_EVENT_QUERY is raised in a loop
        // until we eventually return SCE_ERROR_OK, or we spin until the system watchdog takes us down.
        // Specifically, it appears that anything that calls scePowerRequestStandby() will re-fire the event forever.
        //
        if(g_sleep_inhibits_count) {
            DEBUG_PRINT("Power switch released, allowing sleep.\n");
        }

        g_sleep_inhibits_count = 0;
    }

    return 0;
}

// The main thread.
// * Sets up callbacks
// * Sleeps and processes callbacks
// * Cleans up when awoken.
static
int main_thread(SceSize args, void *argp)
{
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
    DEBUG_PRINT("Creating power callback\n");
    power_cbid = sceKernelCreateCallback(MODULE_NAME " Power Callback", power_callback_handler, NULL);
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

    return 0;
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