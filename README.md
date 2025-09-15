# PSP-KillSwitch
Plugins (.prx) for Playstation Portable running Custom Firmware (PSP CFW), to disable the PSP power switch and prevent accidental sleep mode or shutdown.

Tested with ARK-4 CFW on PSP-3000 FW 6.61.

## Preface

It's no secret that the PSP power switch is poorly designed, and Sony never bothered to fix it on any version of the PSP (and yet they did bother to move the WLAN switch!)

There are two major issues:

* The position of the power switch makes it very easy to accidentally trigger in-game, leading to sleeping or shutdown during gameplay.
* Disabling the hold switch can accidentally overshoot and hit the power button, sleeping the PSP.

These plugins aim to fix these issues.

## Plugins

### KillSwitch

Disables the power switch completely, unless the HOME button is held down while the power switch is pushed.

This is designed to prevent accidental sleep mode or shutdown during gameplay.

When the power switch is pressed and sleep is blocked, the power LED blinks rapidly to alert the user to let go of the switch.
This helps to prevent forced shutdown if the switch is accidentally held for more than 10 seconds.

Although the plugin can be loaded at any time, the typical setup is to only activate KillSwitch in-game, by configuring the CFW plugin loading to "game".
For example, with ARK-4 CFW, add the following line to `SEPLUGINS/PLUGINS.TXT`:

`game, ms0:/SEPLUGINS/KillSwitch.prx, on`

### KillSwitchHold

Disables the power switch for 300ms after hold is deactivated.

This is designed to prevent accidental sleep mode when disabling hold and overshooting the detent.

The typical setup is to activate this for the VSH (the XMB menu), or always, depending on whether it is combined with KillSwitch.
For example, with ARK-4 CFW, add the following line to `SEPLUGINS/PLUGINS.TXT`:

`vsh, ms0:/SEPLUGINS/KillSwitchHold.prx, on`

## Installation

* You will need a custom firmware installed on your PSP. See the [ARK-4 project](github.com/PSP-Archive/ARK-4) for details on how to install it.
* Copy KillSwitch.prx and/or KillSwitchHold.prx into /SEPLUGINS/ on the root of your Memory Stick
* Edit `SEPLUGINS/PLUGINS.TXT`
  * For running both KillSwitch and KillSwitchHold, add the lines
    ```
    vsh, ms0:/SEPLUGINS/KillSwitchHold.prx, on
    game, ms0:/SEPLUGINS/KillSwitch.prx, on
    ```
  * For KillSwitchHold only, add the line
    ```
    all, ms0:/SEPLUGINS/KillSwitchHold.prx, on
    ```
  * See [ARK-4 Plugins](https://github.com/PSP-Archive/ARK-4/wiki/Plugins) for more details. 
* Restart the PSP.

## Caveats

The plugins do not, and cannot, override the force shutdown proceedure. Holding the power switch for 10-15 seconds will always forcefully power off the PSP. It is a built-in hardware feature that cannot be disabled.

## Technical details

There are multiple sources of sleep requests on the PSP:
* Power switch pressed and then released
* A deferred sleep request re-raised when a `scePowerLock()` is released by `scePowerUnlock()`
* The PSP HP Remote control or docking cradle remote sleep button is pressed
* VSH sleeps after the auto sleep timeout
* Anything else that calls `scePowerRequestSuspend()` or `scePowerRequestStandby()`

When a sleep request occurs, a `SCE_SYSTEM_SUSPEND_EVENT_QUERY` system event is raised. This system request notifies any subscribers that
the a sleep request has occured and provides an opportunity for the subscriber to inhibit the sleep request.
Each subscriber can return `SCE_ERROR_OK` to allow the sleep, or `SCE_ERROR_BUSY` to disallow the sleep.
If the sleep is allowed, a `SCE_SYSTEM_SUSPEND_EVENT_START` event is received, followed by many more events for each sleep stage, before the PSP goes to sleep.
If the sleep is disallowed, `SCE_SYSTEM_SUSPEND_EVENT_CANCELLATION` is received and the sleep does not continue.

KillSwitch calls `sceKernelRegisterSysEventHandler()` to subscribe to system events, in order to inhibit the `SCE_SYSTEM_SUSPEND_EVENT_QUERY` by returning `SCE_ERROR_BUSY`. However, it cannot simply block all sleep queries.
This is because certain sleep sources, such as the headphone remote, docking cradle remote sleep button, and even the VSH auto sleep, will raise the sleep query in an infinite loop until sleep is allowed.
If every sleep request is inhibited permanently with `SCE_ERROR_BUSY`, the PSP will be stuck in an infinite loop and crash, eventually powering off.

In order to overcome this issue, two complementary strategies are used:

* Limit the total number of consecutive sleep queries that can be inhibited in a row.
  * The current count of available sleep inhibits remaining is stored in `g_sleep_inhibits_count`. When sleep is disabled, it is set to `MAX_CONSECUTIVE_SLEEP_ATTEMPTS`.
  * Each time a sleep query is raised and inhibited, `g_sleep_inhibits_count` is decremented. If it is zero, sleep is allowed.
  * Any sleep source that raises infinite sleep requests will rapidly exhaust the small count of `g_sleep_inhibits_count` remaining, and the PSP will sleep normally.
  * When held, the power switch can only ever trigger ~5 sleep requests before force shutdown is activated after 10 seconds. Therefore, any `MAX_CONSECUTIVE_SLEEP_ATTEMPTS` value above 5 is suitable. We use 10 as a reasonable safe number.

* Sleep is only blocked at the moment the power switch is pressed.
  * To do this, we create a callback with `sceKernelCreateCallback()` and register it with `scePowerRegisterCallback()`. The main kernel thread calls and blocks on `sceKernelSleepThreadCB()` to process these callbacks as they are raised.
  * When the power switch is pressed, the power event callback is raised with the `PSP_POWER_CB_POWER_SWITCH` flag set.
  * Since the sleep query is only triggered once the power switch is *released*, we have an opportunity to disable sleep before the sleep query is raised.
  * `g_sleep_inhibits_count` is set to `MAX_CONSECUTIVE_SLEEP_ATTEMPTS` as soon as the switch is pressed, which inhibits sleep.
  * If a magic key combination is held down (HOME) while the power switch is pressed, `g_sleep_inhibits_count` is set to zero, sleep is not inhibited, and the PSP sleeps normally when the power switch is released.

When the power switch is released, we could instantly re-enable sleep by setting `g_sleep_inhibits_count` to zero. However, there is one remaining edgecase.
When an application or game takes a power lock using `scePowerLock()` (which is common when reading/writing save data), if the power switch is pressed during this lock region, the PSP may sleep as soon as the lock is released.
Even though we will have blocked the initial sleep request, the power lock will remember that a sleep request occured, and re-raise the sleep request when the power lock is released.
It is likely that the user will have already released the power switch before the power lock is released. In this case, the sleep query will be re-raised, and sleep will be allowed, which is undesirable.

To overcome this issue, when the power switch is released, we simply leave `g_sleep_inhibits_count` alone with a small count left over. If a sleep query is raised afterwards by a power lock being released, it will be blocked by the remaining `g_sleep_inhibits_count`.


KillSwitchHold uses almost the exact same logic as KillSwitch.
The only difference is that the power callback also checks for the `PSP_POWER_CB_HOLD_SWITCH` flag, to detect when the hold switch is released.
When hold is released, `sceKernelLibcClock()` is used to save a hold released timestamp.
If the power switch is pressed within 300ms of the hold release timestamp, sleep is inhibited. Otherwise, sleep is allowed.

## Building

* Follow the PSPDEV toolchain [installation steps](https://pspdev.github.io/installation.html)

### For release

```bash
mkdir -p -- build/release
cd build/release
psp-cmake -DCMAKE_BUILD_TYPE=Release ../..
make
```

### For debug (with debug logs printed to the PSP display)

```bash
mkdir -p -- build/debug
cd build/debug
psp-cmake -DCMAKE_BUILD_TYPE=Debug ../..
make
```

## Disclaimer

As always, the software is provided as-is without warranties of any kind, or claims of fitness for a particular purpose.
I take no responsiblity for any damage caused to your hardware. Be safe and always check your work.
