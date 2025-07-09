# PSP-KillSwitch
Plugins (.prx) to disable the PSP power switch and prevent accidental sleep mode/shutdown.

Tested with ARK-4 CFW on PSP-3000 FW 6.61.

## Preface

It's no secret that the PSP power switch is poorly designed, and Sony never bothered to fix it on any version of the PSP.

There are two major issues:

* The position of the power switch makes it very easy to accidentally trigger in-game, leading to sleeping or shutdown during gameplay.
* Disabling the hold switch can accidentally overshoot and hit the power button, sleeping the PSP.

These plugins aim to fix these issues.

## Plugins

There are two plugins which each fix one issue. They can be used individually or together.

* KillSwitch.prx
* KillSwitchHold.prx

The plugins do not, and cannot, override the force shutdown proceedure. Holding the power switch for 10-15 seconds will always forcefully power off the PSP. It is a built-in hardware feature.

### KillSwitch

Disables the power switch completely, unless the HOME button is held down while the power switch is pushed.

This is designed to prevent accidental sleep mode or shutdown during gameplay.

Although the plugin can be loaded at any time, the typical setup is to only activate KillSwitch in-game, by configuring the CFW plugin loading to "game".
For example, with ARK-4 CFW, add the following line to `SEPLUGINS/PLUGINS.TXT`:

`game, ms0:/SEPLUGINS/KillSwitch.prx, on`

### KillSwitchHold

Disables the power switch for 1 second after hold is deactivated.

This is designed to prevent accidental sleep mode when disabling hold and overshooting the detent.

The typical setup is to activate this for the VSH (the XMB menu), or always, depending on whether it is combined with KillSwitch.
For example, with ARK-4 CFW, add the following line to `SEPLUGINS/PLUGINS.TXT`:

`vsh, ms0:/SEPLUGINS/KillSwitchHold.prx, on`

## Installation

* You will need a custom firmware installed on your PSP. See the [ARK-4 project](github.com/PSP-Archive/ARK-4) for details on how to install it.

* Copy KillSwitch.prx and/or KillSwitchHold.prx into /SEPLUGINS/ on the root of your Memory Stick
* Edit `SEPLUGINS/PLUGINS.TXT`
  * For KillSwitch and KillSwitchHold, add the lines
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

## Building

* Follow the PSPDEV toolchain [installation steps](https://pspdev.github.io/installation.html)
* mkdir build
* cd build
* psp-cmake ..
* make

## Disclaimer

As always, the software is provided as-is without warranties of any kind, or claims of fitness for a particular purpose.
I take no responsiblity for any damage caused to your hardware. Be safe and always check your work.



