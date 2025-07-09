// PSP-KillSwitch v1.0
// .prx plugin that stops the power switch from putting the unit to sleep, unless a button combo is held down.
//
// Ryan Crosby 2025

#include <pspsdk.h>
#include <pspkernel.h>
#include <psppower.h>
#include <pspsysevent.h>
#include <pspctrl.h>

// Allow the switch to work when this button combo is pressed
// Hold HOME + Power Switch to sleep.
// See https://pspdev.github.io/pspsdk/group__Ctrl.html#gac080131ea3904c97efb6c31b1c4deb10 for button constants
#define BUTTON_COMBO_MASK PSP_CTRL_HOME

#define MODULE_NAME "KillSwitch"
#define MAJOR_VER 1
#define MINOR_VER 0

// We are building a kernel mode prx plugin
PSP_MODULE_INFO(MODULE_NAME, PSP_MODULE_KERNEL, MAJOR_VER, MINOR_VER);
PSP_MAIN_THREAD_ATTR(0);
PSP_HEAP_SIZE_KB(-1);
//PSP_MAIN_THREAD_NAME(MODULE_NAME);

// We don't need a main thread since we only do basic setup during module start and won't stall module loading.
// This will make us be called from the module loader thread directly, instead of a secondary kernel thread.
PSP_NO_CREATE_MAIN_THREAD();

PspSysEventHandler events = {0};

int suspend_event_handler(int ev_id, char *ev_name, void *param, int *result)
{
    SceCtrlData pad_state;
    // Sleep event
    if(ev_id == 0x100){
        // Peek the buffer so that we don't "steal" input from other consumers
        if(sceCtrlPeekBufferPositive(&pad_state, 1) >= 0) {
            // Check if the user is holding down the combo
            if((pad_state.Buttons & BUTTON_COMBO_MASK) == BUTTON_COMBO_MASK) {
                return 0; // Allow other callbacks to execute. Allows sleep.
            }
            else {
                return -1; // Do not process other callbacks. Prevents sleep.
            }
        }
        else {
            // Could not read the gamepad. Just allow sleep in this case.
            return 0;
        }
    }

    return 0;
}

int register_suspend_handler()
{
    events.size = 0x40;
    events.name = "MSE_Suspend";
    events.type_mask = 0x0000FF00;
    events.handler = suspend_event_handler;

    return sceKernelRegisterSysEventHandler(&events);
}

int unregister_suspend_handler()
{
    return sceKernelUnregisterSysEventHandler(&events);
}

// Called during module init
int module_start(SceSize args, void *argp)
{
    return register_suspend_handler();
}

// Called during module deinit
int module_stop(SceSize args, void *argp)
{
    return unregister_suspend_handler();
}
