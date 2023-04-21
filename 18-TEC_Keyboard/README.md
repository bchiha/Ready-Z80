# Talking Electronic Computer (TEC) Keyboard

Here are the keyboard files used.  

* __full_keyboard.z80__ Has the ```KEYSCAN``` routine to read two key presses on the keyboard.  It has routines to process the key into ASCII and a Joystick routine.
* __keyboard_test.z80__ The routine that I used to test the keyboard routine in the above file.
* __text_adventure.z80__ Program used in conjuction with the Ruby Adventure Kernel System the sends key presses to the Ruby game via the TEC Serial port and display information on the 20x4 LCD
