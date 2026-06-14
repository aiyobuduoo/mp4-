# USB Extend Screen

This folder contains the standalone USB Vendor JPEG extend-screen module adapted
from Espressif's `usb_extend_screen` example.

It uses the project's existing LCD, hardware JPEG decoder, and PPA rotation.
The Windows image is 800x480 and is rotated into the LCD controller's native
480x800 frame orientation. The example's UAC audio, HID touch, BSP, and LCD
initialization are intentionally not included.

## Control API

Include `usb_extend_screen.h`, then call:

```c
usb_extend_screen_start();
usb_extend_screen_stop();
```

`usb_extend_screen_start()` pauses LVGL rendering and connects the USB display
device. `usb_extend_screen_stop()` disconnects USB, clears pending USB frames,
and restores the LVGL interface.

The module remains disabled until `usb_extend_screen_start()` is called.

On the time screen, swipe down to open the enter confirmation dialog. While the
USB extended screen is active, touch input is reported to Windows as a five-point
USB HID touchscreen. Swipe down again to open the exit confirmation dialog.

The HID touchscreen makes the USB device composite, so it uses PID `0x2986`.
After flashing this version, Windows enumerates it as a new device. Install or
rebind the Espressif IDD driver for `USB\VID_303A&PID_2986`. If touch controls
the wrong monitor, use Windows Tablet PC Settings to select the extended screen.
