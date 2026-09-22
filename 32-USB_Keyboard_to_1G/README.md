# USB Keyboard Adaptor for the TEC-1G

This folder contains code and PCB design for the USB Keyboard to TEC Adaptor Card.  It Uses a Raspberry PI PICO to do the USB to TEC data transfer.  

The PICO code is now **Fixed!**.  Thanks to Scott Gregory who got the program to work correct and improve it.  It now handles all keys normally, including Shift, Ctrl and Function (F1).  New features include Ctrl-Alt-Del will reset the PICO and the TEC-1G.  Pressing Caps Lock will now illuminate a light on the keyboard.

The main issue I had was that I wasn't that familiar with how the PICO works including its two cores with one of the cores delaying the output.

~~At the time of the release of this video, the code for the PICO didn't work fully.  Single key presses worked 'okay' but two key presses (IE: Shift-F) were unreliable.~~

~~I'm hoping someone else out there will get the code working, which I'll update this folder with when working.~~

A new PCB has also been developed to handle the new features and fix some small errors.  Please find this file in the Gerber directory.

I used VSCode using the PlatformIO extension.  I've include the ``main.cpp`` file and the ``platform.ini`` file to get you started.

