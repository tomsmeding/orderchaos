#pragma once

#include "printflike.h"

struct Logwidget;
typedef struct Logwidget Logwidget;

Logwidget* lgw_make(int x,int y,int w,int h);
void lgw_destroy(Logwidget *lgw);
void lgw_redraw(Logwidget *lgw); //called automatically; should only be needed if something else overwrote the widget
void lgw_add(Logwidget *lgw,const char *line);
void lgw_addf(Logwidget *lgw,const char *format,...) __printflike(2,3);
void lgw_clear(Logwidget *lgw);


typedef struct Menuitem{
	char *text;
	int hotkey;
	void (*func)(void); //will be returned by menu_select()
} Menuitem;

typedef struct Menudata{
	int nitems;
	Menuitem *items;
} Menudata;

struct Menuwidget;
typedef struct Menuwidget Menuwidget;

typedef enum Menukey{
	MENUKEY_HANDLED,
	MENUKEY_IGNORED,
	MENUKEY_QUIT,
	MENUKEY_CALLED
} Menukey;

Menuwidget* menu_make(int basex,int basey,const Menudata *data); //keeps a reference to the data!
void menu_destroy(Menuwidget *mw);
void menu_redraw(Menuwidget *mw); //should only be needed if overwritten
Menukey menu_handlekey(Menuwidget *mw,int key);
