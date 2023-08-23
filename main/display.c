#include "ssd1306.h"

#define	MSG_LINE	(7)

static SSD1306_t dev;

void init_display(){
	ssd1306_init(&dev, 128, 64);
	ssd1306_clear_screen(&dev, false);
	ssd1306_contrast(&dev, 0xff);
}

void text_display(int line, const char* text, int len){
	ssd1306_clear_line(dev, line, false);
	ssd1306_display_text(&dev, line, text, len, false);
}

void msg_display(const char* text, int len){
	ssd1306_clear_line(dev, MSG_LINE, false);
	ssd1306_display_text(&dev, MSG_LINE, text, len, false);
}

void img_display(const char* img){
	ssd1306_clear_screen(&dev, false);
}
