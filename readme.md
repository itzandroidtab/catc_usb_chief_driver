# CATC USB Chief
This is a reverse engineering effort to make the driver work on a 64-bit machine (it should also work on 32-bit). The driver was reverse engineerd using IDA Pro and Ghidra.

## Pictures
Some pictures of it working on a 64-bit Windows 11 machine


### Open issues
* The original driver didnt free memory on almost any of the error cases.
* If the application locks up you need a full reboot before the driver works again
* Driver has code that looks very janky (looking at you reused event lock value for byte count)

## Original software
The original software can be found at [Teledynelecroy](https://www.teledynelecroy.com/support/softwaredownload/psg_swarchive.aspx?standardid=4). Search for in the archived downloads `chief`

### Extra note
This also fixes the bug that was patched using a lower driver. If someone from Teledynelecroy wants to sign and release the driver that would be great :+1:

