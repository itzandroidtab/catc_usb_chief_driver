# CATC USB Chief
This is a reverse engineering effort to make the driver work on a 64-bit machine (it should also work on 32-bit). The driver was reverse engineerd using IDA Pro and Ghidra.

## Pictures
Some pictures of it working on a 64-bit Windows 11 machine

![IMG_20251224_013859](https://github.com/user-attachments/assets/d76e70c2-f00e-44bf-9aa9-ffe41f5a4ac0)

<img width="1567" height="1078" alt="image" src="https://github.com/user-attachments/assets/c615542e-367b-43bf-a8a0-113d2001534e" />

### Open issues
* The original driver didnt free memory on almost any of the error cases.
* If the application locks up you need a full reboot before the driver works again
* Driver has code that looks very janky
* The software doesnt show anything when a High-Speed USB device is connected (banged my head against that)
* On some hardware the USB Chief doesnt return anything after the first device descriptor, configuration descriptor requests Windows does (happened Windows server 2019)

## How to build
1. Install cmake, Visual studio and windows DDK (I installed `7600.16385.1`)
2. Open VScode (with cmake tools installed)
3. Change the `SDK_ROOT` SDK path in `CMakeLists.txt` to your installed location
4. Change `Kit` to "Visual studio xxxxxx amd64"
5. Press build (or keyboard shortcut `F7`)
6. Change `$WinDDKRoot` in `sign_driver.ps1` to the same path as `SDK_ROOT`
7. Run powershell as admin
8. Run `sign_driver.ps1`
9. Install driver using `pnputil`, right clicking `usbchief.inf` or using the device manager

## Original software
The original software can be found at [Teledynelecroy](https://www.teledynelecroy.com/support/softwaredownload/psg_swarchive.aspx?standardid=4). Search for in the archived downloads `chief`

### Extra note
This also fixes the bug that was patched using a lower driver. If someone from Teledynelecroy wants to sign and release the driver that would be great :+1:
