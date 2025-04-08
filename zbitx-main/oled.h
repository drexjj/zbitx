
#define OLED_ADDR    0x3C  // Slave address
#define OLED_PAGES   4
#define OLED_COMMAND 0x00
#define OLED_DATA    0x40
//#define CONDENSED 1
//#define OLED_SSD1306   1   // OLED display (SSD1306 128x32 or 128x64)
#define OLED_SH1106    1   // OLED display (SH1106 1.3" inch display)

//esp8266-oled-sh1106
static const uint8_t oled_init_sequence []  = {  
  0xAE,       // Display OFF (sleep mode)
  0xA6,         // Set display mode: Normal
  0xD5, 0x80,   // 0x01--set display clock divide ratio/oscillator frequency   OK? (0x80 (or >=0x10) needed when multiplex ration set to 0x3F)
  0xA8, 0x3F,   // Set multiplex ratio(1 to 64)   128x64
  0xD3, 0x00,   // Set display offset. 00 = no offset
  0x40,         // Set display start line address
  0x8D, 0x14,   // Set charge pump, internal VCC
  0xA1,         // Set Segment Re-map. A0=column 0 mapped to SEG0; A1=column 127 mapped to SEG0. Flip Horizontally
  0xC8,         // Set COM Output Scan Direction.  Flip Veritically.
  0xDA, 0x12,   // Set com pins hardware configuration  128x64
  0x81, 0xCF,   // Set contrast control register
  0xD9, 0xF1,   // 0xF1=brighter //0x22 Set pre-charge period
  0xDB, 0x40,   // Set vcomh 0x20 = 0.77xVcc
  0xA4,     // Output RAM to Display  (display all on resume)   0xA4=Output follows RAM content; 0xA5,Output ignores RAM content
  0xA6,         // Set display mode: Normal
//	0x2E,			//not documented
  0xAF,         // Display ON
  0x20, 0x0,   // Set Memory Addressing; 0=Horizontal Mode; 1=Vertical Mode; 2=Page Mode
	
};

/*
static const uint8_t oled_init_sequence []  = {  // Initialization Sequence  https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf
  0xAE,       // Display OFF (sleep mode)
  0xD5, 0x80,   // 0x01--set display clock divide ratio/oscillator frequency   OK? (0x80 (or >=0x10) needed when multiplex ration set to 0x3F)
  0xA8, 0x3F,   // Set multiplex ratio(1 to 64)   128x64
  0xD3, 0x00,   // Set display offset. 00 = no offset
  0x40,         // Set display start line address
  0xAD, 0x8B,   // SH1106 Set pump mode: pump ON
  0xA1,         // Set Segment Re-map. A0=column 0 mapped to SEG0; A1=column 127 mapped to SEG0. Flip Horizontally
  0xC8,         // Set COM Output Scan Direction.  Flip Veritically.
  0xDA, 0x12,   // Set com pins hardware configuration  128x64
  0x81, 0xFF,   // Set contrast control register
  0xD9, 0xF1,   // 0xF1=brighter //0x22 Set pre-charge period
  0xDB, 0x40,   // Set vcomh 0x20 = 0.77xVcc
  0x30 | 0x2,   // SH1106 Pump voltage 8.0V
  0xA6,         // Set display mode: Normal
  0x20, 0x10,   // Set Memory Addressing; 0=Horizontal Mode; 1=Vertical Mode; 2=Page Mode
  0xA4,     // Output RAM to Display  (display all on resume)   0xA4=Output follows RAM content; 0xA5,Output ignores RAM content
};
*/

/* usdx init 

static const uint8_t oled_init_sequence []  = {  // Initialization Sequence  https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf
  //0xAE,       // Display OFF (sleep mode)
  0xD5, 0x80,   // 0x01--set display clock divide ratio/oscillator frequency   OK? (0x80 (or >=0x10) needed when multiplex ration set to 0x3F)
#ifdef CONDENSED
  0xA8, 0x3F,   // Set multiplex ratio(1 to 64)   128x64
#else
  0xA8, 0x1F,   // Set multiplex ratio(1 to 64)   128x32
#endif
  0xD3, 0x00,   // Set display offset. 00 = no offset
#ifndef OLED_SH1106  // for SSD1306 only:
  0x40,         // Set display start line address
  0x8D, 0x14,   // Set charge pump, internal VCC
  0x20, 0x02,   // Set Memory Addressing; 0=Horizontal Mode; 1=Vertical Mode; 2=Page Mode
  0xA4,     // Output RAM to Display  (display all on resume)   0xA4=Output follows RAM content; 0xA5,Output ignores RAM content
#endif //!OLED_SH1106
  0xA1,         // Set Segment Re-map. A0=column 0 mapped to SEG0; A1=column 127 mapped to SEG0. Flip Horizontally
  0xC8,         // Set COM Output Scan Direction.  Flip Veritically.
#ifdef CONDENSED
  0xDA, 0x12,   // Set com pins hardware configuration  128x64
#else
  0xDA, 0x02,   // Set com pins hardware configuration  128x32
#endif
  0x81, 0x80,   // Set contrast control register
  0xDB, 0x40,   // Set vcomh 0x20 = 0.77xVcc
  0xD9, 0xF1,   // 0xF1=brighter //0x22 Set pre-charge period
  0xB0 | 0x0,   // Set page address, 0-7
#ifdef OLED_SH1106
  0xAD, 0x8B,   // SH1106 Set pump mode: pump ON
  0x30 | 0x2,   // SH1106 Pump voltage 8.0V
#endif //OLED_SH1106
#ifdef INVERSE
  0xA7,         // Set display mode: Inverse
#else
  0xA6,         // Set display mode: Normal
#endif
  //0x00,       // Set low nibble of column address
  //0x10,       // Set high nibble of column address
  0xAF,         // Display ON
};

static const uint8_t oled_init_sequence []  = {  // Initialization Sequence  https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf
  0xAE,       // Display OFF (sleep mode)
  0xD5, 0x80,   // 0x01--set display clock divide ratio/oscillator frequency   OK? (0x80 (or >=0x10) needed when multiplex ration set to 0x3F)
#ifdef CONDENSED
  0xA8, 0x3F,   // Set multiplex ratio(1 to 64)   128x64
#else
  0xA8, 0x1F,   // Set multiplex ratio(1 to 64)   128x32
#endif
  0xD3, 0x00,   // Set display offset. 00 = no offset
#ifndef OLED_SH1106  // for SSD1306 only:
  0x40,         // Set display start line address
  0x8D, 0x14,   // Set charge pump, internal VCC
  0x20, 0x02,   // Set Memory Addressing; 0=Horizontal Mode; 1=Vertical Mode; 2=Page Mode
  0xA4,     // Output RAM to Display  (display all on resume)   0xA4=Output follows RAM content; 0xA5,Output ignores RAM content
#endif //!OLED_SH1106
  0xA1,         // Set Segment Re-map. A0=column 0 mapped to SEG0; A1=column 127 mapped to SEG0. Flip Horizontally
  0xC8,         // Set COM Output Scan Direction.  Flip Veritically.
#ifdef CONDENSED
  0xDA, 0x12,   // Set com pins hardware configuration  128x64
#else
  0xDA, 0x02,   // Set com pins hardware configuration  128x32
#endif
  0x81, 0x80,   // Set contrast control register
  0xDB, 0x40,   // Set vcomh 0x20 = 0.77xVcc
  0xD9, 0xF1,   // 0xF1=brighter //0x22 Set pre-charge period
  0xB0 | 0x0,   // Set page address, 0-7
#ifdef OLED_SH1106
  0xAD, 0x8B,   // SH1106 Set pump mode: pump ON
  0x30 | 0x2,   // SH1106 Pump voltage 8.0V
#endif //OLED_SH1106
#ifdef INVERSE
  0xA7,         // Set display mode: Inverse
#else
  0xA6,         // Set display mode: Normal
#endif
  //0x00,       // Set low nibble of column address
  //0x10,       // Set high nibble of column address
  0xAF,         // Display ON
};


static const uint8_t oled_init_sequence []  = {
  // Initialization Sequence  https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf
  //0xAE,       // Display OFF (sleep mode)
  0xD5, 0x80,   // 0x01--set display clock divide ratio/oscillator frequency   OK? (0x80 (or >=0x10) needed when multiplex ration set to 0x3F)
  0xA8, 0x3F,   // Set multiplex ratio(1 to 64)   128x64
  0xD3, 0x00,   // Set display offset. 00 = no offset
//#ifndef OLED_SH1106  // for SSD1306 only:
  0x40,         // Set display start line address
  0x8D, 0x14,   // Set charge pump, internal VCC
  0x20, 0x02,   // Set Memory Addressing; 0=Horizontal Mode; 1=Vertical Mode; 2=Page Mode
  0xA4,     // Output RAM to Display  (display all on resume)   0xA4=Output follows RAM content; 0xA5,Output ignores RAM content
//#endif //!OLED_SH1106
  0xA1,         // Set Segment Re-map. A0=column 0 mapped to SEG0; A1=column 127 mapped to SEG0. Flip Horizontally
  0xC8,         // Set COM Output Scan Direction.  Flip Veritically.
  0xDA, 0x12,   // Set com pins hardware configuration  128x64
  0x81, 0x80,   // Set contrast control register
  0xDB, 0x40,   // Set vcomh 0x20 = 0.77xVcc
  0xD9, 0xF1,   // 0xF1=brighter //0x22 Set pre-charge period
  0xB0 | 0x0,   // Set page address, 0-7
#ifdef OLED_SH1106
  0xAD, 0x8B,   // SH1106 Set pump mode: pump ON
  0x30 | 0x2,   // SH1106 Pump voltage 8.0V
#endif //OLED_SH1106
#ifdef INVERSE
  0xA7,         // Set display mode: Inverse
#else
  0xA6,         // Set display mode: Normal
#endif
  //0x00,       // Set low nibble of column address
  //0x10,       // Set high nibble of column address
  0xAF,         // Display ON
};
*/


int oled_init();
void oled_refresh();
const char *oled_write(uint8_t col, uint8_t row, const char *string);
void oled_clear();
void oled_console(int style, char *string);
