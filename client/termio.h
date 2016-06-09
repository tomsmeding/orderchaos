#pragma once

#include <stdbool.h>

typedef struct Size{
	int w,h;
} Size;

typedef struct Style{
	int fg,bg;
	bool bold,ul;
} Style;


void initkeyboard(void);
void endkeyboard(void);
void initscreen(void);
void endscreen(void);

Size gettermsize(void);
void setstyle(const Style *style);
void setfg(int fg);
void setbg(int bg);
void setbold(bool bold);
void setul(bool ul);
void tputc(char c);
void tprint(const char *format,...) __printflike(1,2);
void redraw(void);

// If in [0,254], actually that character; else one of the KEY_ constants
int getkey(void);

#define KEY_BACKSPACE 8
#define KEY_LF 10
#define KEY_CR 13

#define KEY_RIGHT 1001
#define KEY_UP 1002
#define KEY_LEFT 1003
#define KEY_DOWN 1004
