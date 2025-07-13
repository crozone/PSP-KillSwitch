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
#define CALLBACK_SLOT 14

#define MODULE_NAME "KillSwitch"
#define MAJOR_VER 1
#define MINOR_VER 1

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
PSP_MAIN_THREAD_ATTR(0);
PSP_HEAP_SIZE_KB(-1);
//PSP_MAIN_THREAD_NAME(MODULE_NAME);

// We don't need a main thread since we only do basic setup during module start and won't stall module loading.
// This will make us be called from the module loader thread directly, instead of a secondary kernel thread.
PSP_NO_CREATE_MAIN_THREAD();

static int killswitchSysEventHandler(int ev_id, char *ev_name, void *param, int *result);

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
    //DEBUG_PRINT("Got SysEvent %#010x - %s\n", ev_id, ev_name);

    // Trap SCE_SYSTEM_SUSPEND_EVENT_QUERY
    // Basically the ScePowerMain thread is asking us "is it okay to sleep?"
    if(ev_id == SCE_SYSTEM_SUSPEND_EVENT_QUERY && !allow_sleep) {
        // There are edgecases where we can still get stuck in an infinite sleep request loop,
        // eg if the user triggers a standby while holding the power switch up.
        // Limit the maximum number of attempts that can be made during a single sleep disallow duration before
        // the request is allowed through as a failsafe.
        if(consecutive_sleep_blocks < MAX_CONSECUTIVE_SLEEPS) {
            consecutive_sleep_blocks++;
            DEBUG_PRINT("Blocked suspend query %#010x - %s (%i)\n", ev_id, ev_name, consecutive_sleep_blocks);
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
        DEBUG_PRINT("Got suspend cancelled event %#010x - %s\n", ev_id, ev_name);

    }
    else if(ev_id == SCE_SYSTEM_SUSPEND_EVENT_START) {
        DEBUG_PRINT("Got suspend start event %#010x - %s\n", ev_id, ev_name);
    }

    return SCE_ERROR_OK;
}

int register_suspend_handler(void)
{
    return sceKernelRegisterSysEventHandler(&sys_event);
}

int unregister_suspend_handler(void)
{
    return sceKernelUnregisterSysEventHandler(&sys_event);
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

    cbid = sceKernelCreateCallback(MODULE_NAME " Power Callback", power_callback_handler, NULL);
    if(cbid < 0) return cbid;

    reg_callback_ret = scePowerRegisterCallback(CALLBACK_SLOT, cbid); // -1 for slot autoassignment doesn't appear to work

    if(reg_callback_ret >= 0) {
        DEBUG_PRINT("Registered power callback in slot " xstr(CALLBACK_SLOT) "\n");

        // Sleep and processing callbacks until we get woken up
        sceKernelSleepThreadCB();

        // Cleanup
        scePowerUnregisterCallback(CALLBACK_SLOT);
    }
    else {
        DEBUG_PRINT("Failed to register power callback: ret %i\n", reg_callback_ret);
    }

    // Cleanup
    sceKernelDeleteCallback(cbid);

	return 0;
}

// Starts callback thread
int start_callbacks(void)
{
    // name, entry, initPriority, stackSize, PspThreadAttributes, SceKernelThreadOptParam
    //thid = sceKernelCreateThread(MODULE_NAME "TaskCallbacks", callback_thread, 0x11, 0xFA0, 0, 0);
    callback_thid = sceKernelCreateThread(MODULE_NAME "TaskCallbacks", callback_thread, 0x11, 0x800, 0, 0);
    if (callback_thid >= 0) {
	    sceKernelStartThread(callback_thid, 0, 0);
    }

    return callback_thid;
}

int stop_callbacks(void)
{
    int result;
    if(callback_thid >= 0) {
        // Unblock sceKernelSleepThreadCB()
        sceKernelWakeupThread(callback_thid);
        // Wait for the callback thread to clean up and exit
        sceKernelWaitThreadEnd(callback_thid, NULL);
        // Delete thread
        result = sceKernelDeleteThread(callback_thid);
        callback_thid = -1;

        return result;
    }

    return 0;
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
        DEBUG_PRINT("Could not start callbacks: ret %#010x\n", result);
        return SCE_KERNEL_ERROR_ERROR ;
    }

    result = register_suspend_handler();
    if(result < 0) {
        DEBUG_PRINT("Could not register suspend handler: ret %#010x\n", result);
        return SCE_KERNEL_ERROR_ERROR;
    }

    return SCE_KERNEL_ERROR_OK;
}

// Called during module deinit
// Module stop doesn't appear to be working correctly
int module_stop(SceSize args, void *argp)
{
    int result;
    DEBUG_PRINT(MODULE_NAME " v" xstr(MAJOR_VER) "." xstr(MINOR_VER) " Module Stop\n");

    result = unregister_suspend_handler();
    if(result < 0) {
        DEBUG_PRINT("Could not unregister suspend handler: ret %#010x\n", result);
    }

    result = stop_callbacks();
    if(result < 0) {
        DEBUG_PRINT("Could not stop callbacks: ret %#010x\n", result);
    }

    if(result < 0) {
        return SCE_KERNEL_ERROR_ERROR;
    }

    return SCE_KERNEL_ERROR_OK;
}
