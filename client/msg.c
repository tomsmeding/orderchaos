#define _GNU_SOURCE  // asprintf
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/select.h>
#include <assert.h>

#include "msg.h"

typedef void (*Value)(ws_conn *conn,const Message*);

typedef struct Chunk{
	int key;
	Value value;
	struct Chunk *next;
} Chunk;

#define HASHMAPSZ (11) //enlarge for larger applications
Chunk* cbhashmap[HASHMAPSZ]={NULL}; //hash map of linked lists

int max_id=1;

static int genid(void){
	return max_id++;
}

static int hashvalue(int key){
	//stub implementation, but should actually work fine for low-frequency messaging
	return key%HASHMAPSZ;
}

static void hash_insert(int key,Value value){
	int hv=hashvalue(key);
	Chunk *ch=malloc(sizeof(Chunk));
	assert(ch);
	ch->key=key;
	ch->value=value;
	ch->next=cbhashmap[hv];
	cbhashmap[hv]=ch;
}

//NULL if nonexistent
static Chunk* hash_find(int key){
	Chunk *ch=cbhashmap[hashvalue(key)];
	while(ch!=NULL&&ch->key!=key){
		ch=ch->next;
	}
	return ch;
}

//returns whether item existed
static bool hash_delete(int key){
	int hv=hashvalue(key);
	Chunk *ch=cbhashmap[hv];
	Chunk *prev=NULL;
	while(ch!=NULL&&ch->key!=key){
		prev=ch;
		ch=ch->next;
	}
	if(ch==NULL)return false;
	if(prev==NULL)cbhashmap[hv]=ch->next;
	else prev->next=ch->next;
	free(ch);
	return true;
}

static char* json_escape(const char *s){
	int len=0;
	for(int i=0;s[i];i++){
		len++;
		if(s[i]=='\\')len++;
		else if(s[i]=='"')len++;
	}
	char *buf=malloc(len+1);
	if(!buf)return NULL;
	for(int i=0,j=0;s[i];i++,j++){
		if(s[i]=='\\'||s[i]=='"')buf[j++]='\\';
		buf[j]=s[i];
	}
	buf[len]='\0';
	return buf;
}

static void free_message(Message *msg){
	if(msg){
		if(msg->typ)free(msg->typ);
		if(msg->args){
			for(int i=0;i<msg->nargs;i++)if(msg->args[i])free(msg->args[i]);
			free(msg->args);
		}
		free(msg);
	}
}


static Message* parse_message(const char *line){
	if(line[0]!='{')return NULL;
	Message *msg=(Message*)malloc(sizeof(Message));
	msg->id=-1;
	msg->typ=NULL;
	msg->args=NULL;
	msg->nargs=0;

#define FAIL_RETURN do {free_message(msg); return NULL;} while(0)

	line++; //move past the '{'

	while(true){
		if(*line!='"')FAIL_RETURN;
		line++;
		const char *keyname=line;
		const char *p=strchr(line,'"');
		if(p==NULL||p[1]!=':'||p[2]=='\0')FAIL_RETURN;
		const char *value=p+2;

		line=value;
		if(p-keyname==2&&memcmp(keyname,"id",2)==0){
			int i,id=0;
			for(i=0;line[i];i++){
				if(line[i]<'0'||line[i]>'9')break;
				id=10*id+line[i]-'0';
			}
			if(i==0)FAIL_RETURN;
			msg->id=id;
			line+=i;
		} else if(p-keyname==4&&memcmp(keyname,"type",4)==0){
			if(*line!='"')FAIL_RETURN;
			line++;
			p=strchr(line,'"');
			if(p==NULL)FAIL_RETURN;
			int typlen=p-line;
			msg->typ=(char*)malloc(typlen+1);
			if(!msg->typ)FAIL_RETURN;
			memcpy(msg->typ,line,typlen);
			msg->typ[typlen]='\0';
			line=p+1;
		} else if(p-keyname==4&&memcmp(keyname,"args",4)==0){
			if(*line!='[')FAIL_RETURN;
			line++;
			int nargs=0,i=0;
			while(true){
				if(line[i]=='\0')FAIL_RETURN;
				if(line[i]==']')break; //no arguments
				if(line[i]!='"')FAIL_RETURN;
				i++;
				while(line[i]!='\0'&&line[i]!='"'){
					if(line[i]=='\\'){
						i++;
						if(line[i]=='\0')FAIL_RETURN;
					}
					i++;
				}
				if(line[i]=='\0')FAIL_RETURN;
				i++;
				nargs++;
				if(line[i]==']')break;
				if(line[i]!=',')FAIL_RETURN;
			}

			//nargs has been calculated, now collect actual args
			//line points at quote of first argument; line+i is final ']'
			msg->nargs=nargs;
			msg->args=calloc(nargs,sizeof(char*)); //such that args are NULL still
			if(!msg->args)FAIL_RETURN;
			for(i=0;i<nargs;i++){
				if(*line!='"')FAIL_RETURN;
				line++;
				for(p=line;*p!='\0'&&*p!='"';p++){
					if(*p=='\\'){
						p++;
						if(*p=='\0')FAIL_RETURN;
					}
				}
				if(*p=='\0')FAIL_RETURN;
				int arglen=p-line;
				msg->args[i]=malloc(arglen+1);
				if(!msg->args[i])FAIL_RETURN;
				memcpy(msg->args[i],line,arglen);
				msg->args[i][arglen]='\0';
				line=p+1;
				if(*line==']'){
					if(i==nargs-1)break;
					else { //shouldn't happen; we counted arguments?
						fprintf(stderr,"Counting arguments doesn't seem to work...\n");
						FAIL_RETURN;
					}
				}
				if(*line!=',')FAIL_RETURN;
				line++;
			}
			line++;
		} else FAIL_RETURN;

		if(*line=='}')break;
		if(*line!=',')FAIL_RETURN;
		line++;
	}
	if(line[1]!='\0')FAIL_RETURN;

#undef FAIL_RETURN

	return msg;
}


bool msg_send(ws_conn *conn,const char *typ,void (*cb)(ws_conn *conn,const Message*),int nargs,...){
	assert(typ);
	assert(nargs>=0);

	char *esc=json_escape(typ);

	char **args=NULL; //already json_escape'd
	int argslen=2; //brackets
	if(nargs>0){
		va_list ap;
		va_start(ap,nargs);
		args=(char**)malloc(nargs*sizeof(char*));
		if(args==NULL){
			va_end(ap);
			free(esc);
			return false;
		}
		for(int i=0;i<nargs;i++){
			char *given=va_arg(ap,char*);
			args[i]=json_escape(given);
			if(args[i]==NULL){
				while(i-->0)free(args[i]);
				free(args);
				free(esc);
				return false;
			}
			argslen+=3+strlen(args[i]); //comma, quotes, string
		}
		va_end(ap);
		argslen--; //remove extra comma
	}

	char *argsstr=(char*)malloc(argslen+1);
	if(!argsstr){
		for(int i=0;i<nargs;i++)free(args[i]);
		free(args);
		free(esc);
		return false;
	}
	argsstr[0]='[';
	int cursor=1;
	for(int i=0;i<nargs;i++){
		if(i!=0)argsstr[cursor++]=',';
		argsstr[cursor++]='"';
		strcpy(argsstr+cursor,args[i]);
		cursor+=strlen(args[i]);
		argsstr[cursor++]='"';
	}
	argsstr[cursor++]=']';
	argsstr[cursor]='\0';

	int id=genid();
	char *res;
	asprintf(&res,"{\"id\":%d,\"type\":\"%s\",\"args\":%s}\n",id,esc,argsstr);
	free(argsstr);
	for(int i=0;i<nargs;i++)free(args[i]);
	free(args);
	free(esc);
	if(!res)return false;
	// fprintf(stderr,"Sending: '%s'\n",res);

	ws_writestr(conn,res);
	free(res);

	if(cb){
		hash_insert(id,cb);
	}
	return true;
}

void msg_runloop(
		ws_conn *conn,
		struct timeval* (*populate_fdsets)(fd_set*,fd_set*),
		void (*msghandler)(ws_conn *conn,const Message*),
		void (*fdhandler)(ws_conn *conn,const fd_set*,const fd_set*),
		void (*timeouthandler)(ws_conn *conn)){

	assert(populate_fdsets);
	assert(msghandler);

	int connsock=ws_selectsock(conn);
	while(true){
		if(!ws_isopen(conn))break;
		fd_set rdset,wrset;
		FD_ZERO(&rdset);
		FD_ZERO(&wrset);
		struct timeval *timeout=populate_fdsets(&rdset,&wrset);
		FD_SET(connsock,&rdset);
		int ret=select(FD_SETSIZE,&rdset,&wrset,NULL,timeout); //TODO: improve nfds estimate
		if(timeout)free(timeout);
		if(ret==-1){
			perror("select");
			exit(1);
		}
		if(ret==0){
			if(timeouthandler)timeouthandler(conn);
			else fprintf(stderr,"No timeout handler while timeout was given!\n");
			continue;
		}

		if(FD_ISSET(connsock,&rdset)){
			char *line=ws_readline(conn);
			if(line==NULL){ //error or closed
				return;
			}
			Message *msg=parse_message(line);
			if(msg){
				int id=msg->id;
				Chunk *ch=hash_find(id);
				if(ch==NULL)msghandler(conn,msg);
				else {
					ch->value(conn,msg);
					hash_delete(id);
				}
				free(msg);
			} else {
				fprintf(stderr,"Could not parse message: '%s'\n",line);
			}
		}

		if(ret>1||(ret==1&&!FD_ISSET(connsock,&rdset))){
			if(fdhandler)fdhandler(conn,&rdset,&wrset);
			else fprintf(stderr,"No fd handler while extra fd's were given!\n");
		}
	}
}