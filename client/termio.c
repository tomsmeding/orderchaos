#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <termios.h>
#include <signal.h>
#include <unistd.h>
#include <assert.h>
#include <sys/select.h>
#include <sys/ioctl.h>

#include "termio.h"

#define prflush(...) fprflush(stdout,__VA_ARGS__)
#define fprflush(f,...) do {fprintf(f,__VA_ARGS__); fflush(f);} while(0)
#define atxy(buf,x,y) ((buf)[termsize.w*(y)+(x)])
#define atpos(buf,pos) atxy(buf,pos.x,pos.y)


typedef struct Position{
	int x,y;
} Position;

typedef struct Screencell{
	char c;
	Style style;
} Screencell;


static bool screenlive=false,keyboardinited=false,sighandlerinstalled=false;
static bool needresize=false;
static bool handlerefresh=false;

static Screencell *screenbuf=NULL,*drawbuf=NULL;
static Size termsize={0,0};

static Position cursor={0,0};
static Style curstyle={9,9,false,false};


static void sighandler(int sig){
	if(!screenlive)return;
	switch(sig){
		case SIGINT:
		case SIGABRT:
			endkeyboard();
			endscreen();
			exit(130); // ^C

		case SIGWINCH:
			needresize=true;
			break;
	}
}

static Size querytermsize(void){
	struct winsize w;
	ioctl(1,TIOCGWINSZ,&w);
	Size sz={w.ws_col,w.ws_row};
	return sz;
}

Size gettermsize(void){
	return termsize;
}

static bool have_tios_bak=false;
static struct termios tios_bak;

void initkeyboard(void){
	struct termios tios;
	tcgetattr(0,&tios_bak);
	have_tios_bak=true;
	tios=tios_bak;
	tios.c_lflag&=~(
		ECHO|ECHOE // no echo of normal characters, erasing
#ifdef ECHOKE
		|ECHOKE // ...and killing
#endif
#ifdef ECHOCTL
		|ECHOCTL // don't visibly echo control characters (^V etc.)
#endif
		|ECHONL // don't even echo a newline
		|ICANON // disable canonical mode
#ifdef NOKERNINFO
		|NOKERNINFO // don't print a status line on ^T
#endif
		|IEXTEN // don't handle things like ^V specially
		//|ISIG // disable ^C ^\ and ^Z
		);
	tios.c_cc[VMIN]=1; // read one char at a time
	tios.c_cc[VTIME]=0; // no timeout on reading, make it a blocking read
	tcsetattr(0,TCSAFLUSH,&tios);

	keyboardinited=true;
}

void endkeyboard(void){
	if(!keyboardinited)return;
	if(have_tios_bak)tcsetattr(0,TCSAFLUSH,&tios_bak);
	keyboardinited=false;
}

void initscreen(void){
	printf("\x1B[?1049h\x1B[2J\x1B[H"); fflush(stdout);

	termsize=querytermsize();

	screenbuf=malloc(termsize.w*termsize.h*sizeof(Screencell));
	assert(screenbuf);
	drawbuf=malloc(termsize.w*termsize.h*sizeof(Screencell));
	assert(drawbuf);

	for(int i=0;i<termsize.w*termsize.h;i++){
		screenbuf[i].c=' ';
		screenbuf[i].style.fg=screenbuf[i].style.bg=9;
		screenbuf[i].style.bold=screenbuf[i].style.ul=false;
	}
	memcpy(drawbuf,screenbuf,termsize.w*termsize.h*sizeof(Screencell));

	if(!sighandlerinstalled){
		signal(SIGWINCH,sighandler);
		signal(SIGINT,sighandler);
		signal(SIGABRT,sighandler);
	}
	sighandlerinstalled=true;
	screenlive=true;
}

void endscreen(void){
	if(!screenlive)return;

	printf("\x1B[?1049l"); fflush(stdout);

	if(screenbuf)free(screenbuf);
	if(drawbuf)free(drawbuf);

	screenlive=false;
}

void installrefreshhandler(bool install){
	handlerefresh=install;
}


void clearscreen(void){
	for(int i=0;i<termsize.w*termsize.h;i++){
		drawbuf[i].c=' ';
		drawbuf[i].style.fg=drawbuf[i].style.bg=9;
		drawbuf[i].style.bold=drawbuf[i].style.ul=false;
	}
}


void setstyle(const Style *style){
	assert((style->fg>0&&style->fg<8)||style->fg==9);
	assert((style->bg>0&&style->bg<8)||style->bg==9);
	curstyle=*style;
}

void setfg(int fg){
	assert((fg>0&&fg<8)||fg==9);
	curstyle.fg=fg;
}
void setbg(int bg){
	assert((bg>0&&bg<8)||bg==9);
	curstyle.bg=bg;
}
void setbold(bool bold){
	curstyle.bold=bold;
}
void setul(bool ul){
	curstyle.ul=ul;
}

// Modifies accstyle to match style
// Pass NULL as accstyle to unconditionally set style
static void outputstyle(Style *accstyle,const Style *style){
	char *parts[6];
	int nparts=0;
	if(!accstyle||accstyle->bold!=style->bold){
		if(style->bold){
			parts[nparts++]="1";
		} else {
			parts[nparts++]="0"; // no way to reset bold
			if(accstyle)accstyle->fg=accstyle->bg=9;
			if(accstyle)accstyle->bold=accstyle->ul=false;
		}
	}
	if(!accstyle||accstyle->ul!=style->ul){
		if(style->ul)parts[nparts++]="4";
		else parts[nparts++]="24";
	}
	if(!accstyle||style->fg!=accstyle->fg)asprintf(&parts[nparts++],"3%d",style->fg);
	if(!accstyle||style->bg!=accstyle->bg)asprintf(&parts[nparts++],"4%d",style->bg);

	if(accstyle)*accstyle=*style;

	if(nparts==0)return;
	printf("\x1B[%s",parts[0]);
	for(int i=1;i<nparts;i++)printf(";%s",parts[i]);
	putchar('m');
}

static void copyintersect(Screencell *dest,Size destsz,const Screencell *src,Size srcsz){
	for(int y=0;y<destsz.h;y++){
		for(int x=0;x<destsz.w;x++){
			if(x<srcsz.w&&y<srcsz.h){
				memcpy(dest+(destsz.w*y+x),src+(srcsz.w*y+x),sizeof(Screencell));
			} else {
				dest[destsz.w*y+x].c=' ';
				dest[destsz.w*y+x].style.fg=dest[destsz.w*y+x].style.bg=9;
				dest[destsz.w*y+x].style.bold=dest[destsz.w*y+x].style.ul=false;
			}
		}
	}
}

static void resizeterm(void){
	needresize=false;

	Size oldsize=termsize;

	termsize=querytermsize();

	Screencell *newscreen=malloc(termsize.w*termsize.h*sizeof(Screencell));
	assert(newscreen);
	Screencell *newdraw=malloc(termsize.w*termsize.h*sizeof(Screencell));
	assert(newdraw);

	copyintersect(newscreen,termsize,screenbuf,oldsize);
	copyintersect(newdraw,termsize,drawbuf,oldsize);

	free(screenbuf);
	free(drawbuf);
	screenbuf=newscreen;
	drawbuf=newdraw;
}

static void tputcstartx(char c,int *startx){
	switch(c){
		case '\r':
			*startx=0;
		case '\n':
			cursor.x=*startx;
			if(cursor.y<termsize.h-1)cursor.y++;
			break;

		default:
			assert(c>=32&&c<127);
			atpos(drawbuf,cursor).style=curstyle;
			atpos(drawbuf,cursor).c=c;
			cursor.x++;
			if(cursor.x==termsize.w){
				cursor.x=*startx;
				if(cursor.y<termsize.h-1){
					cursor.y++;
				}
			}
			break;
	}
}

void tputc(char c){
	int startx=0;
	tputcstartx(c,&startx);
}

__printflike(1,2) void tprintf(const char *format,...){
	if(needresize)resizeterm();
	char *buf;
	va_list ap;
	va_start(ap,format);
	int len=vasprintf(&buf,format,ap);
	va_end(ap);

	assert(buf&&len>=0);

	int startx=cursor.x;

	char *bufit=buf;
	while(len-->0){
		tputcstartx(*bufit++,&startx);
	}
}

static void redrawfullx(bool full){
	if(needresize){
		resizeterm();
		full=true;
	}
	int x,y;
	Style st;
	bool first=true;
	printf("\x1B[?25l"); //civis
	for(y=0;y<termsize.h;y++){
		bool shouldmove=true;
		for(x=0;x<termsize.w;x++){
			if(!full&&memcmp(&atxy(drawbuf,x,y),&atxy(screenbuf,x,y),sizeof(Screencell))==0){
				shouldmove=true;
				continue;
			}
			if(shouldmove){
				printf("\x1B[%d;%dH",y+1,x+1);
				shouldmove=false;
			}
			if(first){
				outputstyle(NULL,&atxy(drawbuf,x,y).style);
				first=false;
				st=atxy(drawbuf,x,y).style;
			} else outputstyle(&st,&atxy(drawbuf,x,y).style);
			putchar(atxy(drawbuf,x,y).c);
			atxy(screenbuf,x,y).style=atxy(drawbuf,x,y).style;
			atxy(screenbuf,x,y).c=atxy(drawbuf,x,y).c;
		}
	}
	printf("\x1B[%d;%dH",cursor.y+1,cursor.x+1);
	printf("\x1B[?12;25h"); //cvvis
	fflush(stdout);
}

void redraw(void){
	redrawfullx(false);
}

void redrawfull(void){
	redrawfullx(true);
}


void moveto(int x,int y){
	assert(x>=0&&x<termsize.w&&y>=0&&y<termsize.h);
	cursor.x=x;
	cursor.y=y;
	//printf("\x1B[%d;%dH",y+1,x+1);
}

typedef struct Llitem{
	Position pos;
	struct Llitem *next;
} Llitem;
static Llitem *cursorstack=NULL;

void pushcursor(void){
	Llitem *item=malloc(sizeof(Llitem));
	assert(item);
	item->pos=cursor;
	item->next=cursorstack;
	cursorstack=item;
}

void popcursor(void){
	assert(cursorstack);
	Llitem *item=cursorstack;
	cursorstack=item->next;
	cursor=item->pos;
	free(item);
}


void bel(void){
	putchar('\007');
	fflush(stdout);
}


int getkey(void){
	static int bufchar=-1; //-1 is nothing, 0<=x<=255 is char
	unsigned char c;
	if(bufchar!=-1){
		c=bufchar;
		bufchar=-1;
	} else if(read(0,&c,1)==0)return -1; //EOF
	if(c==12&&handlerefresh){
		redrawfull();
		return getkey();
	}
	if(c!=27)return c;

	//escape key was pressed, now listen for escape sequence
	fd_set inset;
	FD_ZERO(&inset);
	FD_SET(0,&inset);
	struct timeval tv;
	tv.tv_sec=0;
	tv.tv_usec=100000; //100ms escape timeout
	int ret=select(1,&inset,NULL,NULL,&tv);

	if(ret==0)return 27; //just escape key
	// if(ret==-1)do_something(); //in case of select error, we just continue reading
	if(read(0,&c,1)==0)return -1; //EOF
	if(c!='['){
		bufchar=c;
		return 27;
	}
	if(read(0,&c,1)==0)return -1; //EOF
	switch(c){
		case 'A': return KEY_UP;
		case 'B': return KEY_DOWN;
		case 'C': return KEY_RIGHT;
		case 'D': return KEY_LEFT;
		default:
			return getkey(); // unrecognised escape sequence; really need to handle this better
	}
}
