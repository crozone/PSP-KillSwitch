// PSP-KillSwitch v1.1
// .prx plugin that stops the power switch from putting the unit to sleep, unless a button combo is held down.
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

#include <stdbool.h>

#define str(s) #s // For stringizing defines
#define xstr(s) str(s)

#ifdef DEBUG
#define DEBUG_PRINT(...) pspDebugScreenKprintf( __VA_ARGS__ )
#else
#define DEBUG_PRINT(...) do{ } while ( 0 )
#endif

// Allow the switch to work when this button combo is pressed
// Hold HOME + Power Switch to sleep.
// See https://pspdev.github.io/pspsdk/group__Ctrl.html#gac080131ea3904c97efb6c31b1c4deb10 for button constants
#define BUTTON_COMBO_MASK PSP_CTRL_HOME
#define MAX_CONSECUTIVE_SLEEPS 10

#define MODULE_NAME "KillSwitch"
#define MAJOR_VER 1
#define MINOR_VER 2

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

static int killswitchSysEventHandler(int ev_id, char *ev_name, void *param, int *result);
static int power_callback_handler(int unknown, int pwrflags, void *common);

bool allow_sleep = true;
int consecutive_sleep_blocks = 0;
int callback_thid = -1;

// Our PspSysEventHandler to receive the power switch event
PspSysEventHandler sys_event = {
    .size = sizeof(PspSysEventHandler),
    .name = "sce" MODULE_NAME, // Arbitrary string, doesn't appear to be used for anything
    .type_mask = SCE_SUSPEND_EVENTS,
    .handler = killswitchSysEventHandler,
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

int killswitchSysEventHandler(int ev_id, char *ev_name, void *param, int *result)
{
    //DEBUG_PRINT("Got SysEvent 0x%08x - %s\n", ev_id, ev_name);

    // Trap SCE_SYSTEM_SUSPEND_EVENT_QUERY
    // Basically the ScePowerMain thread is asking us "is it okay to sleep?"
    if(ev_id == SCE_SYSTEM_SUSPEND_EVENT_QUERY && !allow_sleep) {
        // There are edgecases where we can still get stuck in an infinite sleep request loop,
        // eg if the user triggers a standby while holding the power switch up.
        // Limit the maximum number of attempts that can be made during a single sleep disallow duration before
        // the request is allowed through as a failsafe.
        if(consecutive_sleep_blocks < MAX_CONSECUTIVE_SLEEPS) {
            consecutive_sleep_blocks++;
            DEBUG_PRINT("Blocked suspend query 0x%08x - %s (%i)\n", ev_id, ev_name, consecutive_sleep_blocks);
            return SCE_ERROR_BUSY;
        }
        else {
            DEBUG_PRINT("Max consecutive suspend queries reached (%i), allowing sleep.\n", consecutive_sleep_blocks);

            // We won't receive the power switch released callback since we'll be asleep, so reset allow_sleep here.
            allow_sleep = true;
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
int power_callback_handler(int unknown, int pwrflags, void *common)
{
    if (pwrflags & PSP_POWER_CB_POWER_SWITCH) {
        // This is called immediately as the switch is pressed.
        // The SysEventHandler is called when the power switch is released, or held down for a second.
        // This gives us a chance to get in before it and decide whether to allow the sleep.

        DEBUG_PRINT("Power switch pressed\n");

        // Check if the user is pressing the override key combination
        //
        SceCtrlData pad_state;
        if(sceCtrlPeekBufferPositive(&pad_state, 1) >= 0) {
            if((pad_state.Buttons & BUTTON_COMBO_MASK) == BUTTON_COMBO_MASK) {
                DEBUG_PRINT("Override key pressed, allowing sleep\n");
                allow_sleep = true;
            }
            else {
                DEBUG_PRINT("Disallowing sleep\n");
                allow_sleep = false;
            }
        }
        else {
            // There was an error reading button state. Allow sleep in this case.
            DEBUG_PRINT("Failed to read button state! Allowing sleep\n");
            allow_sleep = true;
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
        if(!allow_sleep) {
            DEBUG_PRINT("Allowing sleep\n");
        }

        allow_sleep = true;
    }

    if(allow_sleep) {
        consecutive_sleep_blocks = 0;
    }

    return 0;
}

// Set up and process callbacks
int callback_thread(SceSize args, void *argp)
{
    int cbid;
    int reg_callback_ret;
    int slot;

    DEBUG_PRINT("Creating power callback\n");
    cbid = sceKernelCreateCallback(MODULE_NAME " Power Callback", power_callback_handler, NULL);
    if(cbid < 0) {
        DEBUG_PRINT("Failed to create power callback: ret 0x%08x\n", cbid);
        return cbid;
    }

    // -1 for slot autoassignment doesn't appear to work, so search backwards for an available slot manually
    for(slot = 15; slot >= 0; slot--) {
        DEBUG_PRINT("Registering power callback in slot %i\n", slot);
        reg_callback_ret = scePowerRegisterCallback(slot, cbid);
        if(reg_callback_ret >= 0) {
            break;
        }
        else {
            DEBUG_PRINT("Failed to register power callback in slot %i: ret 0x%08x\n", slot, reg_callback_ret);
        }
    }

    if(reg_callback_ret >= 0 && slot >= 0) {
        DEBUG_PRINT("Power callback successfully registered in slot %i\n", slot);
        DEBUG_PRINT("Now processing callbacks\n");

        // Sleep and processing callbacks until we get woken up
        sceKernelSleepThreadCB();

        // Cleanup
        reg_callback_ret = scePowerUnregisterCallback(slot);
        if(reg_callback_ret < 0) {
            // We can't really do anything about an error here except log it, although we don't expect this to error
            DEBUG_PRINT("Failed to unregister power callback from slot %i: ret 0x%08x\n", slot, reg_callback_ret);
        }
    }
    else {
        DEBUG_PRINT("Failed to register power callback in any slot!\n");
    }

    // Cleanup
    DEBUG_PRINT("Deleting power callback\n");
    int delete_ret = sceKernelDeleteCallback(cbid);
    if(delete_ret < 0) {
        // We can't really do anything about an error here except log it, although we don't expect this to error
        DEBUG_PRINT("Failed to delete power callback: ret 0x%08x\n", delete_ret);
    }

    return 0;
}

int register_suspend_handler(void)
{
    DEBUG_PRINT("Registering sysevent handler\n");
    int register_sysevent_ret = sceKernelRegisterSysEventHandler(&sys_event);
    if(register_sysevent_ret < 0) {
        DEBUG_PRINT("Failed to register sysevent handler: ret 0x%08x\n", register_sysevent_ret);
    }

    return register_sysevent_ret;
}

int unregister_suspend_handler(void)
{
    DEBUG_PRINT("Unregistering sysevent handler\n");
    int unregister_sysevent_ret = sceKernelUnregisterSysEventHandler(&sys_event);
    if(unregister_sysevent_ret < 0) {
        DEBUG_PRINT("Failed to unregister sysevent handler: ret 0x%08x\n", unregister_sysevent_ret);
    }

    return unregister_sysevent_ret;
}

// Starts callback thread
int start_callbacks(void)
{
    int result;
    // name, entry, initPriority, stackSize, PspThreadAttributes, SceKernelThreadOptParam
    result = sceKernelCreateThread(MODULE_NAME "TaskCallbacks", callback_thread, 0x11, 0x800, 0, 0);
    if (result >= 0) {
        callback_thid = result;
        DEBUG_PRINT("Starting callback thread\n");
        result = sceKernelStartThread(result, 0, 0);
        if(result < 0) {
            DEBUG_PRINT("Failed to start callback thread: ret 0x%08x\n", result);
        }
    }
    else {
        DEBUG_PRINT("Failed to create callback thread: ret 0x%08x\n", result);
    }

    return result;
}

int stop_callbacks(void)
{
    int result = 0;
    int thid = callback_thid;
    if(thid >= 0) {
        // Unblock sceKernelSleepThreadCB() and have thread begin cleanup
        result = sceKernelWakeupThread(thid);
        if(result < 0) {
            DEBUG_PRINT("Failed to wakeup callback thread: ret 0x%08x\n", result);
        }

        // Wait for the callback thread to clean up and exit
        DEBUG_PRINT("Waiting for callback thread exit ...\n");
        result = sceKernelWaitThreadEnd(thid, NULL);
        if(result < 0) {
            // Thread did not stop, force terminate and delete it
            DEBUG_PRINT("Failed to wait for callback thread exit: ret 0x%08x\n", result);
            DEBUG_PRINT("Terminating and deleting thread\n", result);
            result = sceKernelTerminateDeleteThread(thid);
            if(result >= 0) {
                callback_thid = -1;
            }
            else {
                DEBUG_PRINT("Failed to terminate delete callback thread: ret 0x%08x\n", result);
            }
        }
        else {
            DEBUG_PRINT("Deleting callback thread ...\n");
            // Thead stopped cleanly, delete it
            result = sceKernelDeleteThread(thid);
            if(result >= 0) {
                DEBUG_PRINT("Callback cleanup complete.\n");
                callback_thid = -1;
            }
            else {
                DEBUG_PRINT("Failed to delete callback thread: ret 0x%08x\n", result);
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

    result = start_callbacks();
    if(result < 0) {
        return MODULE_ERROR;
    }

    result = register_suspend_handler();
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

    result = unregister_suspend_handler();
    if(result < 0) {
        return MODULE_ERROR;
    }

    result = stop_callbacks();
    if(result < 0) {
        return MODULE_ERROR;
    }

    DEBUG_PRINT(MODULE_NAME " v" xstr(MAJOR_VER) "." xstr(MINOR_VER) " Module Stop\n");

    return MODULE_OK;
}
