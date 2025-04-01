// See ReadMe tab for instructions to add a "user setup" configuration file to your sketch

/*TO INTERFACE THE TFT_ESPI to ILI9844 ON PICO
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

4.  This step is already done in this sketch, but to adapt you own sketches, open the sketch and add a
    new tab, "tft_setup.h" in the main sketch, put all the tft library setup information in that header.
    The tab header name must be tft_setup.h

  IMPORTANT:  You will need to remember to add the platform.local.txt file again if you upgrade the IDE or
  the board package version. Note that the file must be added to each processor board package you are using.
*/


#include <SPI.h>
#include <TFT_eSPI.h>       // Hardware-specific library
#include <EEPROM.h>
#include "zbitx.h"
#include "free_font.h"

TFT_eSPI tft = TFT_eSPI();  // Invoke custom library
//static uint8_t waterfall[240*200]; //Very Arbitrary!

uint16_t font_width2[256];
uint16_t font_width4[256];

void screen_draw_mono(const char *text, int count, int x_at, int y_at);

/* Graphics primitives */

static void screen_read_calibration(uint16_t *calibration_data){
  
  byte *p = (byte *)calibration_data;
  for (int i = 0; i < 12; i++){
    byte b = EEPROM.read(i);
    *p++ = b;  
  }
}

void screen_init(){
  uint16_t calibration_data[10];
  
  EEPROM.begin(512);
  tft.init();
  tft.fillScreen(SCREEN_BACKGROUND_COLOR);
  tft.setRotation(3);
  tft.setSwapBytes(true);


  ///calibrate the screen or retreive the calibration from EEPROM
  uint16_t x, y;
  if (screen_read(&x, &y)){
    Serial.println("Calibrating the screen");
     while(screen_read(&x, &y))
      delay(100);
    delay(200);
    tft.calibrateTouch(calibration_data, TFT_WHITE, TFT_RED, 15);
    byte *p = (byte *)calibration_data;
    for (int i = 0; i < 12; i++){
      byte b = *p++;
      EEPROM.write(i, b);
    }
    EEPROM.commit(); 
  }
  else 
    screen_read_calibration(calibration_data);

  screen_text_extents(2, font_width2);
  screen_text_extents(4, font_width4);
  tft.setTouch(calibration_data);
}

void screen_fill_rect(int x, int y, int w, int h, int color){
  tft.fillRect(x,y,w,h,color);
}

void screen_draw_rect(int x, int y, int w, int h, int color){
  tft.drawRect(x, y, w, h, color);
}

void screen_fill_round_rect(int x, int y, int w, int h, int color){
  tft.fillRoundRect(x,y,w,h,4, color);
}

void screen_draw_round_rect(int x, int y, int w, int h, int color){
  tft.drawRoundRect(x, y, w, h, 4, color);
}

void   screen_draw_line(int x1, int y1, int x2, int y2, int color){
	tft.drawLine(x1, y1, x2, y2, color);
}

void screen_draw_mono(const char *text, int count, int x_at, int y_at, uint16_t color){
  uint16_t pixel_line[16];
  //repeat for each pixel line

  for (int i = 0; i < count; i++){
    for (int col = 0; col < 5; col++){
      char bits = pgm_read_byte(&font[0] + ((text[i]*5) + col));
      uint8_t mask = 0x01;
      uint16_t *p = pixel_line;
      for (int row = 0; row < 7; row++){
        if(mask & bits){
          *p++ = color;
          *p++ = color;
        }
        else{
          *p++ = 0;
          *p++ = 0;
        }
        mask <<= 1;
      }
      *p++ = 0; //space between the characters          

      //dump the bitmap
//      tft.pushRect(x_at++, y_at, 1, 16, pixel_line);
      tft.pushRect(x_at++, y_at, 1, 16, pixel_line);
    }
    //space between characters
    memset(pixel_line, 0, sizeof(pixel_line));
    tft.pushRect(x_at++, y_at, 1, 16, pixel_line);
    tft.pushRect(x_at++, y_at, 1, 16, pixel_line);
//    tft.pushRect(x_at++, y_at, 1, 16, pixel_line);
    //x_at += 1;
  }
}

void screen_draw_text(const char *text, int length, int x, int y, int color, int font){
  //if (font == 3)
  //  tft.setFreeFont(&FreeMonoBold18pt7b);
  tft.setTextFont(font);
  tft.setTextColor(color); 
  tft.drawString(text, x, y);
}

void screen_text_extents(int font, uint16_t *extents){
  char *widthtable = (char *)pgm_read_dword( &(fontdata[font].widthtbl )); //subtract the 32 outside the loop
  for (int i = 32; i < 127; i++)
    extents[i] = (uint16_t) pgm_read_byte(widthtable + i - 32);
}

void screen_bitblt(int x, int y, int w, int h, uint16_t *bmp){
   tft.pushRect(x, y, w, h, bmp);
}

int16_t screen_text_width(char *text, uint8_t font){
  return tft.textWidth(text, font);
}

int16_t screen_text_height(uint8_t font){
  return tft.fontHeight(font);
}

void screen_pixel(int x, int y, uint16_t color){
  tft.drawPixel(x,y,color);
}

bool screen_read(uint16_t *x, uint16_t *y){
  return tft.getTouch(x, y);  
}

