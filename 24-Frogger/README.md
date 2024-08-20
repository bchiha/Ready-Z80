# TEC Frogger for the Z80 TEC-1G

This is my remake of the classic '80s game Frogger.  Z80 code is built for the TEC-1G Z80 computer using Mon3 (Monitor).

Use the Intel Hex Load option from the main menu to load the `TEC_Frogger.hex` file via USB FTDI interface.  The program loads at address `0x4000`.  Press `GO` to run and enjoy!

* __TEC_Frogger.z80__ Is the source code of the game for you to view/modify.  It is designed for the TEC-1G using Mon3 to interface with the Graphical 128x64 LCD Screen.  To compile use [asm80.com](https://www.asm80.com). 
* __TEC_Frogger.hex__ Intel Hex file format ready to load on the TEC-1G.  Game will load at address `0x4000`.
* __frogger_data.z80__ and __glcd.z80__ are include files for the game.
* __frogger_music__ mp3 and sheet music for the intro game sound.
  