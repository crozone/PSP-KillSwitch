// PSP-KillSwitch v1.0
// .prx plugin that stops the power switch from putting the unit to sleep, unless a button combo is held down.
//
// Ryan Crosby 2025

#include <stdbool.h>

#include <pspdebug.h>
#include <pspdisplay.h>

#include <pspsdk.h>
#include <pspkernel.h>
#include <psppower.h>
#include <pspsysevent.h>
#include <pspctrl.h>
#include <psptypes.h>

#define ONE_MSEC 1000
#define ONE_SEC (1000 * ONE_MSEC)

#ifdef DEBUG
#define DEBUG_PRINT(...) fprintf( stderr, __VA_ARGS__ )
#else
#define DEBUG_PRINT(...) do{ } while ( 0 )
#endif

// Allow the switch to work when this button combo is pressed
// Hold HOME + Power Switch to sleep.
// See https://pspdev.github.io/pspsdk/group__Ctrl.html#gac080131ea3904c97efb6c31b1c4deb10 for button constants
#define BUTTON_COMBO_MASK PSP_CTRL_HOME

#define MODULE_NAME "KillSwitch"
#define MAJOR_VER 1
#define MINOR_VER 0

// https://github.com/uofw/uofw/blob/7ca6ba13966a38667fa7c5c30a428ccd248186cf/include/common/errors.h
#define SCE_ERROR_OK                                0x0
#define SCE_ERROR_BUSY                              0x80000021

// https://github.com/uofw/uofw/blob/7ca6ba13966a38667fa7c5c30a428ccd248186cf/include/sysmem_sysevent.h#L7-L83
#define SCE_SUSPEND_EVENTS                          0x0000FF00
#define SCE_SYSTEM_SUSPEND_EVENT_QUERY              0x00000100

// We are building a kernel mode prx plugin
PSP_MODULE_INFO(MODULE_NAME, PSP_MODULE_KERNEL, MAJOR_VER, MINOR_VER);
PSP_MAIN_THREAD_ATTR(0);
PSP_HEAP_SIZE_KB(-1);
//PSP_MAIN_THREAD_NAME(MODULE_NAME);

// We don't need a main thread since we only do basic setup during module start and won't stall module loading.
// This will make us be called from the module loader thread directly, instead of a secondary kernel thread.
PSP_NO_CREATE_MAIN_THREAD();

static int killswitchSysEventHandler(int ev_id, char *ev_name, void *param, int *result);

bool power_switch_pressed = false;
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
    SceCtrlData pad_state;

    DEBUG_PRINT("Got SysEvent %#010x - %s\n", ev_id, ev_name);

    // Trap SCE_SYSTEM_SUSPEND_EVENT_QUERY
    // Basically the ScePowerMain thread is asking us "is it okay to sleep?"
    //
    // If the physical power switch isn't currently pressed, this is a suspend or standby command
    // coming from elsewhere (eg a PSP Cradle, or debugger poweroff command).
    //
    // We need to always allow suspend or standby from these other places, because if we don't,
    // the event handler is called with SCE_SYSTEM_SUSPEND_EVENT_QUERY in a loop forever
    // until we eventually return SCE_ERROR_OK (or we spin until the system watchdog takes down the whole system).
    //
    if(power_switch_pressed && ev_id == SCE_SYSTEM_SUSPEND_EVENT_QUERY) {
        // Peek (not read) the buffer so that we don't cause other callers using Read to block
        if(sceCtrlPeekBufferPositive(&pad_state, 1) >= 0) {
            // Check if the user is holding down the combo
            if((pad_state.Buttons & BUTTON_COMBO_MASK) == BUTTON_COMBO_MASK) {
                // Allow sleep
                power_switch_pressed = false;
                return SCE_ERROR_OK;
            }
            else {
                // User is not holding combo, so disallow sleep.
                return SCE_ERROR_BUSY;
            }
        }
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
        // This is called immediately as the switch is pressed, before the SysEventHandler is called,
        // so we have a chance to get in before it and set power_switch_pressed.

	    DEBUG_PRINT("Power switch pressed, disabling sleep\n");
        power_switch_pressed = true;
    }
    else {
        // This is called as the switch is released, but after before the SysEventHandler is called,
        // so it won't reset power_switch_pressed too early.
        power_switch_pressed = false;
    }

	return 0;
}

// Sets up and process callbacks
int callback_thread(SceSize args, void *argp)
{
    int cbid;
    int slot;

    cbid = sceKernelCreateCallback(MODULE_NAME " Power Callback", power_callback_handler, NULL);
    if(cbid < 0) return cbid;

    slot = scePowerRegisterCallback(0, cbid); // -1 for slot autoassignment didn't work, so use slot 0

    if(slot >= 0) {
        DEBUG_PRINT("Registered power callback in slot %i\n", slot);

        // Sleep and processing callbacks until we get woken up
        sceKernelSleepThreadCB();

        // Cleanup
        scePowerUnregisterCallback(slot);
    }
    else {
        DEBUG_PRINT("Failed to registered power callback: ret %i\n", slot);
    }

    // Cleanup
    sceKernelDeleteCallback(cbid);

    if(slot < 0) {
        return slot;
    }

	return 0;
}

// Starts callback thread
int start_callbacks(void)
{
    int thid = 0;
    thid = sceKernelCreateThread(MODULE_NAME "TaskCallbacks", callback_thread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) {
        callback_thid = thid;
	    sceKernelStartThread(thid, 0, 0);
    }
    return thid;
}

int stop_callbacks(void)
{
    // Unblock sceKernelSleepThreadCB()
    sceKernelWakeupThread(callback_thid);
    // Wait for the callback thread to exit
    sceKernelWaitThreadEnd(callback_thid, NULL);
    // Cleanup
    sceKernelDeleteThread(callback_thid);
    callback_thid = -1;
}

// Called during module init
int module_start(SceSize args, void *argp)
{
    int result;

    #ifdef DEBUG
    pspDebugScreenInit();
    #endif

    DEBUG_PRINT("KillSwitch Module Start\n");

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
    DEBUG_PRINT("KillSwitch Module Stop\n");

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
