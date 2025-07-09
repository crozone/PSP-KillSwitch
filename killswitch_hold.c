// PSP-KillSwitchHold v1.0
// .prx plugin that stops the power switch from putting the unit to sleep immediately after hold is deactivated.
// This prevents the issue of accidentally sleeping the PSP when deactivating hold and overshooting the detent.
//
// Ryan Crosby 2025

#include <pspsdk.h>
#include <pspkernel.h>
#include <psppower.h>
#include <pspsysevent.h>
#include <pspctrl.h>
#include <stdbool.h>

#define ONE_MSEC 1000
#define ONE_SEC (1000 * ONE_MSEC)

// Disable sleep for 1 second after hold is deactivated
#define DISABLE_DURATION ONE_SEC

#define MODULE_NAME "KillSwitchHold"
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

bool run = false;
bool sleep_allowed = true;

//
// The overall strategy is to keep sleep disabled whenever the hold switch is activated,
// and then after hold is deactivated, hold it disabled for a small duration more.
//

int suspend_event_handler(int ev_id, char *ev_name, void *param, int *result)
{
    // Sleep event
    if(ev_id == 0x100) {
        return sleep_allowed ? 0 : -1;
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

// Main loop that polls for hold state
int main_thread(SceSize args, void *argp)
{
    SceCtrlData pad_state;
    while(run){
        if(sceCtrlPeekBufferPositive(&pad_state, 1) >= 0) {
            // Check if the hold switch is activated
            if((pad_state.Buttons & PSP_CTRL_HOLD) == PSP_CTRL_HOLD) {
                // Disable sleep
                sleep_allowed = false;
            }
            else {
                // Hold deactivated
                if(!sleep_allowed) {
                    // Hold activated -> deactivated
                    // Wait the duration before re-enabling sleep
                    sceKernelDelayThread(DISABLE_DURATION);
                    sleep_allowed = true;
                }
            }
        }
        else {
            // Could not read the gamepad.
            // Failsafe and re-enable sleep.
            sleep_allowed = true;
        }

        // Sleep 50ms before next poll
        sceKernelDelayThread(50 * ONE_MSEC);
    }

    return 0;
}

// Called during module init
int module_start(SceSize args, void *argp)
{
    int result = register_suspend_handler();
    if(result >= 0) {
        int thid = sceKernelCreateThread(MODULE_NAME, main_thread, 32, 0x800, 0, NULL);
        if(thid >= 0) {
            run = true;
            sceKernelStartThread(thid, args, argp);
        }
    }

    return result;
}

// Called during module deinit
int module_stop(SceSize args, void *argp)
{
    int result = unregister_suspend_handler();
    run = false; // Stop main thread
    return result;
}
