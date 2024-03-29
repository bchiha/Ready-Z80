# Cellular Automata for the Z80

These programs generated Elementary Cellular Automation using a 128x64 Graphics LCD screen and Amstrad CPC range of Z80 computers.

* __CellularAutomata_TEC1G.z80__ is the code based on the Cellular Automation video.  It is designed for the TEC-1G using Mon3 to interface with the Graphical 128x64 LCD Screen.
* __CellularAutomata_Generic.z80__ is designed for any Z80 computer that uses a Graphical 128x64 LCD Screen.  The ```lcd_128x64_glib.z80``` file is included which contains the graphical routines.  This will need to be configure to suit the way the GLCD is connected to your computer.
* __CellularAutomata_AmstradCPC.z80__ is designed for an Amstrad CPC computer.  The code is the same except for its hardware interfacing.  Also, only some rules are shown.  Have a look if you are interested in only choosing a limited amount of rules to display.
  