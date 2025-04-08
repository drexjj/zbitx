#include <Arduino.h>
#include <TFT_eSPI.h>       // Hardware-specific library
#include "zbitx.h"
#include "console.h"

static int start_of_last_line = 0;
static int console_prev_top = -1;
static uint16_t console_next = 0;

#define MAX_LINES 20 
#define MAX_LINE_LENGTH 50 
int current_line = 0; 
int current_column = 0;

struct text_line {
	char buff[MAX_LINE_LENGTH];
};

struct text_line console_buffer[MAX_LINES];

#define CONSOLE_TX 0x0100
#define CONSOLE_RX 0x0200
static uint16_t console_text[MAX_LINES];

void console_init(){
	memset(console_buffer, 0, sizeof(console_buffer));
}

//draw full the first time
static int8_t redraw_full = 1;

void console_update(struct field *f, const char *style, const char *text){
	struct text_line *t = console_buffer+ current_line;
	
	int line_length = screen_text_width(t->buff, 2);	
	while(*text){
		//move to the next line if the char count or the extent exceeds the width
		if (current_column >= (MAX_LINE_LENGTH-1) ||
			(screen_text_width(t->buff, 2) + font_width2[*text]) >= f->w - 4 ||
			*text == '\n'){
			current_line++;
			if (current_line >= MAX_LINES)
				current_line = 0;
			t = console_buffer + current_line;
			t->buff[0] = 0;
			current_column = 0;
			redraw_full = 1;
//			Serial.printf("current line %d\n", current_line);
		}
		int end = strlen(t->buff);
		t->buff[current_column++] = *text;
		t->buff[current_column] = 0;	
		text++;
	}
}

void console_draw(struct field *f){
	int line = current_line;
	int i = f->h/16 - 1;
	if (redraw_full){
	 	line = current_line - (f->h / 16) + 1;
		i = 0;
    screen_fill_rect(f->x+2, f->y+2, f->w-4, f->h-4, SCREEN_BACKGROUND_COLOR);
	}
	if (line < 0)
		line += MAX_LINES;

	struct text_line *t = console_buffer+ line;
	while (i < f->h/16){
		screen_draw_text(t->buff, -1, f->w+2, f->y+(i *16), TFT_GREEN, 2);
		line ++;
		if (line >= MAX_LINES)
			line = 0;
		t = console_buffer + line; 
		i++;
	}
	redraw_full = 0;
}
