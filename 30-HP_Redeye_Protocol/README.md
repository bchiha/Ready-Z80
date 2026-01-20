# HP 27S Scientific Calculator IR Transmission

The HP27S calculator (and many others), has the ability to transmit screen data to a Thermal Printer via an Infrared LED.  It is called by HP, the REDEYE Protocol.

In this video, I explain the protocol, create hardware to receive the light signal and display the transmitted data on a serial terminal.

The ``redeye_printer.z80`` file contains code that uses the Z80 Interrupt line to receive pulses from the calculator and translate the pulses to binary data, and eventually ASCII.  This ASCII is then transmitted to a Serial Terminal.

I've also include other files including the ``HP Journal Oct-87`` which details the IR protocol used, also the ``82240 Technical Guide`` for the Printer.  

Some bonus files are included.  These are the HP-27S calculator manuals and guide.  Also a well written document by Martin Hepperle who uses an Arduino to process the signal.

__NOTE:__ I don't talk about this in the video due to its length, but the Z80 code also includes an Error Correction Routine that can correct up to 2 missed bits.  It also has a Timed Based method to read the IR signal.  This method was mostly okay, but missed too many bytes.  The code is there to peruse. 
