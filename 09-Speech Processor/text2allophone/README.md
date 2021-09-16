# Text 2 Allophone converter - text2allophone.c
Compile and run, then type some text.  Type **PP** to insert a 200ms pause and **EOF** to insert `0xFF` (End of file for TEC running program).  Control-D to exit.
**Note:** Puctuation must be seperated with a space or don't use it at all.

Output is given as Allophones and the SP0256a-AL2 hex equivilant.  If a word can't be found "**??**" is outputed and a "**.**" represents a word gap.  A pause of 100ms is automatically placed between words.

To be used with the TEC Speech Module attached to **Port 7**.

**Options**
|  |  |
|--|--|
|**-b**| Output Binary File which includes main program code |
| **-w** | Don't include main program in bianary file (used with -b) |
| **-w** | Don't include main program in bianary file (used with -b) |
| **-p** xx | Change the default output port where xx is the port number (used with -b> |
| **-d** | Output hex in z80 assmebler format using the DB directive |

For binary output to directly load into the TEC. use -b as a command line option.  Binary files are created with the code to activate the Speech Module and the speech data.  If you just want the allophones to be outputted use -w as well.  A new file is created after each Carriage Return.  Binary file are to be loaded at address `0x900` on the TEC.  If outputing a file the default port is 07H, this can be changed with the -p option.  If compiling the code locally, use the -d option that will translate the hex data into DB directive statements.

## Files that are requried are:

| | |
|-|-|
|text2allophone.c|This Program|
|cmudict-0.7b|CMU dictionary file that associates words to the ARPAbet phoneme set.  See http://www.speech.cs.cmu.edu/cgi-bin/cmudict|
|cmu2sp0.symbols|Convert CMU to SPO256a Allophones|

## Sample output from the program

    > Welcome to Talking Electronics PP I hope you have a great day ! EOF
    TXT> WW EH LL KK2 AX MM . TT2 UW2 . TT2 AO KK2 IH NG . IH LL EH KK2 TT2 RR1 AA
         NN1 IH KK2 SS . AY . HH2 OW PP . YY2 UW2 . HH2 AE VV . AX . GG3 RR1 EY TT2 .
         DD2 EY . EH KK2 SS KK2 LL AX MM EY SH AX NN1 PP OY NN1 TT2 .

    000> 2E 07 2D 02 29 0F 10 03
    008> 02 0D 1F 03 02 0D 17 02
    010> 29 0C 2C 03 0C 2D 07 02
    018> 29 02 0D 0E 18 0B 0C 02
    020> 29 37 03 04 06 03 39 35
    028> 02 09 03 19 1F 03 39 1A
    030> 23 03 0F 03 01 22 0E 14
    038> 02 0D 03 01 21 14 03 07
    040> 02 29 37 02 29 2D 0F 10
    048> 14 25 0F 0B 02 09 05 0B
    050> 02 0D 03 FF

    DB 2EH,07H,2DH,02H,29H,0FH,10H,03H
    DB 02H,0DH,1FH,03H,02H,0DH,17H,02H
    DB 29H,0CH,2CH,03H,0CH,2DH,07H,02H
    DB 29H,02H,0DH,0EH,18H,0BH,0CH,02H
    DB 29H,37H,03H,04H,06H,03H,39H,35H
    DB 02H,09H,03H,19H,1FH,03H,39H,1AH
    DB 23H,03H,0FH,03H,01H,22H,0EH,14H
    DB 02H,0DH,03H,01H,21H,14H,03H,07H
    DB 02H,29H,37H,02H,29H,2DH,0FH,10H
    DB 14H,25H,0FH,0BH,02H,09H,05H,0BH
    DB 02H,0DH,03H,0FFH

For reference, here is the Test Program that is used to run the Speech Module taking in data for the SP0256a-AL2 chip.  It is included in the binary output by default

    0900    21 0B 09    LD HL,090C      ;Location of allophone data
    0903    01 pp ss    LD BC,SIZE + PORT ;Load BC with length of allophone data and output port
    0906    ED B3       OTIR            ;Output Contents of HL, to port C, B times
    0908    76          HALT            ;Wait for key input as EOF has reached
    0909    18 F5       JR 0900         ;Jump back to start
    090B    <start of Allophone data>

    Note: SIZE (ss) and PORT (pp) are calculated at runtime based on inputs
