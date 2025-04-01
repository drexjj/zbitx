/*
It is possible to bypass the blockage put in place by the Arduino IDE so that a setup file can be included
by a sketch.

The procedure is as follows:

1. Find the folder containing the boards.txt file for the processor board package you are using. To do this
   click the IDE menu "File->Preferences". At the bottom of that window click the link to the preferences.txt
   file. This will open a folder. Navigate to find the folder containing the "boards.txt" file for the
   processor you are using. Example paths for RP2040 and ESP32 are:

   C:\Users\xxxxx\AppData\Local\Arduino15\packages\rp2040\hardware\rp2040\2.5.2
   C:\Users\xxxxx\AppData\Local\Arduino15\packages\esp32\hardware\esp32\2.0.4

2.  Copy the platform.local.txt file to that folder. A copy of the "platform.local.txt" is in this sketch
    folder, press Ctrl+K to open the folder.

3.  Close the Arduino IDE and open it again so the added file is recognised.
Tage you are using.


To Do:
  1. Add an alert box that shows that the log was added
  2. Improve the text editor
  3. Show the RX bandwidth on the waterfall
  4. Make the tuning less jiggly (probably feedback from the main radio)
*/