# PATA drive to TEC-1G expansion board

Here you can find the schematics, PCB and files for the TEC-1G PATA drive project.

``pata_fat32.z80`` is designed to work on the TEC-1G.  It can be compiled using [asm80.com](https://asm80.com).  It uses TEC-1G API's but can be modified to work on other z80 systems.

The expansion board is designed to fit on top of the TEC-1G into the TEC-DECK slots.  Gerber files and a Bill of Materials (BOM) file has been provided.

The [files](files) directory contain Binaries and Intel HEX files that can be copied to the PATA drive and used on the TEC-1G.  Two Readme files have been created that gives an overview of the files.  `readLCD.hex` uses the 20x4 LCD screen and `readGLCD.hex` uses the GLCD screen if installed.

