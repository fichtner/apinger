/*
 *  Alarm Pinger (c) 2002 Jacek Konieczny <jajcus@jajcus.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as published
 *  by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 *  USA
 *
 */

#include "config.h"
#include "apinger.h"

#include <stdio.h>
#ifdef HAVE_STDLIB_H
# include <stdlib.h>
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#ifdef HAVE_SIGNAL_H
# include <signal.h>
#endif
#ifdef HAVE_STRING_H
# include <string.h>
#endif
#ifdef HAVE_SYS_WAIT_H
# include <sys/wait.h>
#endif
#ifdef HAVE_SYS_POLL_H
# include <sys/poll.h>
#endif
#ifdef HAVE_ARPA_INET_H
# include <arpa/inet.h>
#endif

#include <netdb.h>

#include "debug.h"
#include "tv_macros.h"
#include "rrd.h"

#ifdef HAVE_ASSERT_H
# include <assert.h>
#else
# define assert(cond)
#endif

#define MIN(a,b) (((a)<(b))?(a):(b))

struct delayed_report {
	int on;
	struct alarm_cfg *a;
	struct target *t;
	struct timeval timestamp;
	struct delayed_report *next;
};

struct delayed_report *delayed_reports=NULL;

struct timeval operation_started;


int is_alarm_on(struct target *t,struct alarm_cfg *a){
struct active_alarm_list *al;

	for(al=t->active_alarms;al;al=al->next)
		if (al->alarm==a)
			return 1;
	return 0;
}

void alarm_on(struct target *t,struct alarm_cfg *a){
struct active_alarm_list *al;
struct timeval cur_time,tv;

	gettimeofday(&cur_time,NULL);
	al=NEW(struct active_alarm_list,1);
	al->next=t->active_alarms;
	al->alarm=a;
	al->num_repeats=0;
	if (a->repeat_interval){
		tv.tv_sec=a->repeat_interval/1000;
		tv.tv_usec=(a->repeat_interval%1000)*1000;
		timeradd(&cur_time,&tv,&al->next_repeat);
	}
	t->active_alarms=al;
}

void alarm_off(struct target *t,struct alarm_cfg *a){
struct active_alarm_list *al,*pa,*na;

	pa=NULL;
	for(al=t->active_alarms;al;al=na){
		na=al->next;
		if (al->alarm==a){
			if (pa!=NULL)
				pa->next=na;
			else
				t->active_alarms=na;
			free(al);
			return;
		}
		else pa=al;
	}
	logit("Alarm '%s' not found in '%s'",a->name,t->name);
	return;
}

static char *macros_buf=NULL;
static int macros_buf_l=0;
const char * subst_macros(const char *string,struct target *t,struct alarm_cfg *a,int on){
char *p;
int nmacros=0;
int i,sl,l,n;
char **values;
char ps[16],pr[16],al[16],ad[16],ts[100];
time_t tim;

	if (string==NULL || string[0]=='\000') return "";
	for(i=0;string[i]!='\000';i++){
		if (string[i]!='%') continue;
		nmacros++;
		i++;
		if (string[i]=='\000') break;
	}
	if (nmacros==0) return string;
	values=NEW(char *,(nmacros+1));
	assert(values!=NULL);
	l=sl=strlen(string);
	n=0;
	for(i=0;i<sl;i++){
		if (string[i]!='%') continue;
		i++;
		switch(string[i]){
		case 't':
			values[n]=t->name;
			break;
		case 'T':
			values[n]=t->description;
			break;
		case 'a':
			if (a)
				values[n]=a->name;
			else
				values[n]="?";
			break;
		case 'A':
			if (a)
				switch(a->type){
				case AL_DOWN:
					values[n]="down";
					break;
				case AL_LOSS:
					values[n]="loss";
					break;
				case AL_DELAY:
					values[n]="delay";
					break;
				default:
					values[n]="unknown";
					break;
				}
			else
				values[n]="?";
			break;
		case 'r':
			switch(on){
			case -1:
				values[n]="alarm canceled (config reload)";
				break;
			case 0:
				values[n]="alarm canceled";
				break;
			default:
				values[n]="ALARM";
				break;
			}
			break;
		case 'p':
			sprintf(ps,"%i",t->last_sent);
			values[n]=ps;
			break;
		case 'P':
			sprintf(pr,"%i",t->received);
			values[n]=pr;
			break;
		case 'l':
			if (AVG_LOSS_KNOWN(t)){
				sprintf(al,"%0.1f%%",AVG_LOSS(t));
				values[n]=al;
			}
			else values[n]="n/a";
			break;
		case 'd':
			if (AVG_DELAY_KNOWN(t)){
				sprintf(ad,"%0.3fms",AVG_DELAY(t));
				values[n]=ad;
			}
			else values[n]="n/a";
			break;
		case 's':
			tim=time(NULL);
			strftime(ts,100,config->timestamp_format,localtime(&tim));
			values[n]=ts;
			break;
		case '%':
			values[n]="%";
			break;
		default:
			values[n]="";
			break;
		}
		l+=strlen(values[n])+1;
		n++;
	}
	values[n]=NULL;
	l+=2;
	if (macros_buf == NULL){
		macros_buf=NEW(char,l);
		macros_buf_l=l;
	}else if (macros_buf_l != l){
		macros_buf=(char *)realloc(macros_buf,l);
		macros_buf_l=l;
	}
	assert(macros_buf!=NULL);
	memset(macros_buf, 0, macros_buf_l);
	p=macros_buf;
	n=0;
	for(i=0;i<sl;i++){
		if (string[i]!='%'){
			*p++=string[i];
			continue;
		}
		strcpy(p,values[n]);
		p+=strlen(values[n]);
		n++;
		i++;
	}
	free(values);
	*p='\000';
	return macros_buf;
}

void write_report(FILE *f,struct target *t,struct alarm_cfg *a,int on){
time_t tm;

	tm=time(NULL);
	fprintf(f,"%s|%s|%i|%i|%u|",t->name, t->description, t->last_sent+1,
		t->received, t->last_received_tv.tv_sec);
	if (AVG_DELAY_KNOWN(t)){
		fprintf(f,"%4.3fms|",AVG_DELAY(t));
	}
	if (AVG_LOSS_KNOWN(t)){
		fprintf(f,"%5.1f%%",AVG_LOSS(t));
	}
	fprintf(f, "\n");
}

void make_reports(struct target *t,struct alarm_cfg *a,int on){
FILE *p;
int ret;
const char *command;

	if (on>0) command=a->pipe_on;
	else command=a->pipe_off;
	if (command){
		command=subst_macros(command,t,a,on);
		debug("Popening: %s",command);
		p=popen(command,"w");
		if (!p){
			logit("Couldn't pipe report through %s",command);
			myperror("popen");
		}
		else {
			write_report(p,t,a,on);
			ret=pclose(p);
			if (!WIFEXITED(ret)){
				logit("Error while piping report.");
				logit("command (%s) terminated abnormally.",command);
			}
			else if (WEXITSTATUS(ret)!=0){
				logit("Error while piping report.");
				logit("command (%s) exited with status: %i",command,WEXITSTATUS(ret));
			}
		}
	}
	if (on>0) command=a->command_on;
	else command=a->command_off;
	if (command){
		command=subst_macros(command,t,a,on);
		debug("Starting: %s",command);
		ret=system(command);
		if (!WIFEXITED(ret)){
			logit("Error while starting command form alarm(%s) on target(%s-%s)", a->name, t->name, t->description);
			logit("command (%s) terminated abnormally.",command);
		}
		else if (WEXITSTATUS(ret)!=0){
			logit("Error while starting command form alarm(%s) on target(%s-%s)", a->name, t->name, t->description);
			logit("command (%s) exited with status: %i",command,WEXITSTATUS(ret));
		}
	}
}

void make_delayed_reports(void){
	struct delayed_report *wdr;

	if (delayed_reports==NULL) return;

	wdr = delayed_reports;

	make_reports(wdr->t, wdr->a, wdr->on);

	delayed_reports = wdr->next;
	free(wdr);
}

void toggle_alarm(struct target *t,struct alarm_cfg *a,int on){
struct delayed_report *dr,*tdr;

	if (on>0){
		logit("ALARM: %s(%s)  *** %s ***",t->description,t->name,a->name);
		alarm_on(t,a);
	}
	else{
		alarm_off(t,a);
		if (on==0)
			logit("alarm canceled: %s(%s)  *** %s ***",t->description,t->name,a->name);
		else
			logit("alarm canceled (config reload): %s(%s)  *** %s ***",t->description,t->name,a->name);
	}

	if ((on < 0) || (t->config->force_down == 1)) {
		return;
	}

	if (a->combine_interval>0){
		for(tdr=delayed_reports;tdr!=NULL && tdr->next!=NULL;tdr=tdr->next){
			if (strcmp(tdr->t->name,t->name)==0 && tdr->a==a && tdr->on==on) return;
		}
		if (tdr!=NULL && strcmp(tdr->t->name,t->name)==0 && tdr->a==a && tdr->on==on) return;
		dr=NEW(struct delayed_report,1);
		assert(dr!=NULL);
		dr->t=t;
		dr->a=a;
		dr->on=on;
		gettimeofday(&dr->timestamp,NULL);
		dr->next=NULL;
		if (tdr==NULL)
			delayed_reports=dr;
		else
			tdr->next=dr;
	}
	else {
		make_reports(t,a,on);
	}
}

/* if a time came for the next event schedule next one in given interval and return 1 */
int scheduled_event(struct timeval *next_event,struct timeval *cur_time,int interval){
struct timeval ct,tv;
int ret;

	if (cur_time==NULL){
		gettimeofday(&ct,NULL);
		cur_time=&ct;
	}
	if (!timerisset(next_event) || timercmp(next_event,cur_time,<)){
		if (!timerisset(next_event)){
			*next_event=*cur_time;
		}
		tv.tv_sec=interval/1000;
		tv.tv_usec=(interval%1000)*1000;
		timeradd(cur_time,&tv,next_event);
		ret=1;
	}
	else {
		ret=0;
	}
	if (!timerisset(&next_probe) || timercmp(next_event,&next_probe,<))
		next_probe=*next_event;
	return ret;
}

void send_probe(struct target *t){
int i,i1;
char buf[100];
int seq;

	seq=++t->last_sent;
	debug("Sending ping #%i to %s (%s)",seq,t->description,t->name);
#if 0
	strftime(buf,100,"%b %d %H:%M:%S",localtime(&t->next_probe.tv_sec));
	debug("Next one scheduled for %s",buf);
#endif
	if (t->addr.addr.sa_family==AF_INET) send_icmp_probe(t,seq);
#ifdef HAVE_IPV6
	else if (t->addr.addr.sa_family==AF_INET6) send_icmp6_probe(t,seq);
#endif

	i=t->last_sent%(t->config->avg_loss_delay_samples+t->config->avg_loss_samples);
	if (t->last_sent>t->config->avg_loss_delay_samples+t->config->avg_loss_samples){
		if (!t->queue[i]) t->recently_lost--;
	}
	t->queue[i]=0;

	if (t->last_sent>t->config->avg_loss_delay_samples){
		i1=(t->last_sent-t->config->avg_loss_delay_samples)
			%(t->config->avg_loss_delay_samples+t->config->avg_loss_samples);
		if (!t->queue[i1]) t->recently_lost++;
			debug("Recently lost packets: %i",t->recently_lost);
	}

	if (t->recently_lost < 0)
		t->recently_lost = 0;

	t->upsent++;
}


void analyze_reply(struct timeval *time_recv,int icmp_seq,struct trace_info *ti, int timedelta){
struct target *t;
struct timeval tv;
double delay,avg_delay,avg_loss;
double tmp;
int i;
int previous_received;
struct alarm_list *al;
struct active_alarm_list *aal,*paa,*naa;
struct alarm_cfg *a;

	if (icmp_seq!=(ti->seq%65536)){
		debug("Sequence number mismatch.");
		return;
	}

	for(t=targets;t!=NULL;t=t->next){
		if (t==ti->target_id) break;
	}
	if (t==NULL){
		logit("Couldn't match any target to the echo reply.\n");
		return;
	}
	previous_received=t->last_received;
	if (ti->seq>t->last_received) t->last_received=ti->seq;
	t->last_received_tv=*time_recv;
	timersub(time_recv,&ti->timestamp,&tv);
	delay=tv.tv_sec*1000.0+((double)tv.tv_usec)/1000.0 - timedelta;
	//if (delay < 0) delay = 0;
	tmp=t->rbuf[t->received%t->config->avg_delay_samples];
	t->rbuf[t->received%t->config->avg_delay_samples]=delay;
	t->delay_sum+=delay-tmp;
	debug("#%i from %s(%s) delay: %4.3fms/%4.3fms/%4.3fms received = %d ",ti->seq,t->description,t->name,delay,tmp,t->delay_sum, t->received);
	if (t->delay_sum < 0) t->delay_sum = 0;
	t->received++;

	avg_delay=AVG_DELAY(t);
	debug("(avg: %4.3fms)",avg_delay);

	i=ti->seq%(t->config->avg_loss_delay_samples+t->config->avg_loss_samples);
#if 0
	if (!t->queue[i] && ti->seq<=t->last_sent-t->config->avg_loss_delay_samples)
		t->recently_lost--;
#endif
	t->queue[i]=1;

	if (AVG_LOSS_KNOWN(t)){
		avg_loss=AVG_LOSS(t);
	}else
		avg_loss=0;

	debug("(avg. loss: %5.1f%%)",avg_loss);

	paa=NULL;
	for(aal=t->active_alarms;aal;aal=naa){
		naa=aal->next;
		a=aal->alarm;
		if (a->type==AL_DOWN){
			t->received = 1;
			t->recently_lost = 0;
			t->upsent=0;
			avg_loss=0;
		}
		if ((a->type==AL_DOWN)
		   || (a->type==AL_DELAY && avg_delay<a->p.lh.low)
		   || (a->type==AL_LOSS && avg_loss<a->p.lh.low) ){
			if (a->type == AL_DELAY) {
				t->delay_sum = delay-tmp;
				if (t->delay_sum < 0)
					t->delay_sum = 0;
			}
			toggle_alarm(t,a,0);
		}
	}

	for(al=t->config->alarms;al;al=al->next){
		a=al->alarm;
		if (is_alarm_on(t,a)) continue;
		switch(a->type){
		case AL_DELAY:
			if (AVG_DELAY_KNOWN(t) && avg_delay>a->p.lh.high )
				toggle_alarm(t,a,1);
			break;
		case AL_LOSS:
			if ( avg_loss > a->p.lh.high )
				toggle_alarm(t,a,1);
			break;
		default:
			break;
		}
	}
}

void configure_targets(struct config *cfg, int reload){
struct target *t,*pt,*nt;
struct target_cfg *tc;
struct delayed_report *dr, *pdr, *ndr;
struct active_alarm_list *aal,*naal;
struct alarm_cfg *a, *na;
union addr addr, srcaddr;
#ifdef HAVE_IPV6
struct addrinfo hints, *res;
#endif
int r;
int l;

	/* delete all not configured targets */
	pt=NULL;
	for(t=targets;t;t=nt){
		for(tc=cfg->targets;tc;tc=tc->next) {
			if (strlen(tc->srcip) != strlen(t->config->srcip) || strcmp(tc->srcip, t->config->srcip))
				continue;
			if (strlen(tc->name) == strlen(t->name) && strcmp(tc->name,t->name)==0)
				break;
		}
		nt=t->next;
		if (tc==NULL){
			if (pt==NULL)
				targets=nt;
			else
				pt->next=nt;
			for(aal=t->active_alarms;aal;aal=naal){
				naal=aal->next;
				toggle_alarm(t,aal->alarm,-1);
			}
			if (t->socket)
				close(t->socket);

			if (delayed_reports != NULL) {
				pdr = NULL;
				for (dr = delayed_reports; dr != NULL; dr = ndr) {
					ndr = dr->next;
					if (dr->t == t) {
						if (pdr == NULL)
							delayed_reports = ndr;
						else
							pdr->next = ndr;
						free(dr);
					} else
						pdr = dr;
				}
			}

			free(t->queue);
			free(t->rbuf);
			debug("Releasing target %s(%s)", t->name, t->description);
			free(t->name);
			free(t->description);
			free(t);
		}
		else {
			pt=t;

			for(aal=t->active_alarms;aal;aal=naal){
				naal = aal->next;
				for (a=cfg->alarms;a;a=na){
					na = a->next;
					if (aal->alarm->type == a->type && !strcmp(aal->alarm->name, a->name)) {
						debug("Sticking to alrm %s since its still active", a->name);
						aal->alarm = a;
						break;
					}
				}
			}
			if (delayed_reports != NULL) {
				for (dr = delayed_reports; dr != NULL; dr = dr->next) {
					if (dr->t == t) {
						for (a=cfg->alarms;a;a=a->next) {
							if (dr->a->type == a->type && !strcmp(dr->a->name, a->name)) {
								debug("Updating delayed report for target(%s) and alarm(%s)", t->name, a->name);
								dr->a = a;
							}
						}
					}
				}
			}
		}
	}

	/* Update target configuration */
	for(tc=cfg->targets;tc;tc=tc->next){
		for(t=targets;t;t=t->next) {
			if (strlen(tc->srcip) != strlen(t->config->srcip) || strcmp(tc->srcip, t->config->srcip))
				continue;
			if (strlen(t->name) == strlen(tc->name) && !strcmp(t->name,tc->name))
				break;
		}
		if (t==NULL) { /* new target */
			memset(&addr,0,sizeof(addr));
			r=inet_pton(AF_INET,tc->name,&addr.addr4.sin_addr);
			if (r){
				addr.addr.sa_family=AF_INET;
			}else{
#ifdef HAVE_IPV6
				memset(&hints, 0, sizeof(hints));
				hints.ai_family = AF_INET6;
				hints.ai_flags = AI_NUMERICHOST;
				r = getaddrinfo(tc->name, NULL, &hints, &res);
				if (r != 0) {
					r=inet_pton(AF_INET6,tc->name,&addr.addr6.sin6_addr);
					if (r==0){
#endif
						logit("Bad host address: %s\n",tc->name);
						logit("Ignoring target %s\n",tc->name);
						continue;
#ifdef HAVE_IPV6
					}
				} else {
					memcpy(&addr.addr6, res->ai_addr, res->ai_addrlen);
					freeaddrinfo(res);
				}
				if (icmp6_sock<0){
					logit("Sorry, IPv6 is not available\n");
					logit("Ignoring target %s\n",tc->name);
					continue;
				}
				addr.addr.sa_family=AF_INET6;
#endif
			}
			memset(&srcaddr,0,sizeof(srcaddr));
			debug("Converting srcip %s", tc->srcip);
			r=inet_pton(AF_INET,tc->srcip,&srcaddr.addr4.sin_addr);
			if (r){
				srcaddr.addr.sa_family=AF_INET;
			}else{
#ifdef HAVE_IPV6
				memset(&hints, 0, sizeof(hints));
				hints.ai_family = AF_INET6;
				hints.ai_flags = AI_NUMERICHOST;
				r = getaddrinfo(tc->srcip, NULL, &hints, &res);
				if (r != 0) {
					r=inet_pton(AF_INET6,tc->srcip,&srcaddr.addr6.sin6_addr);
					if (r==0){
#endif
						logit("Bad srcip address %s for target %s\n", tc->srcip, tc->name);
						logit("Ignoring target %s\n",tc->name);
						continue;
#ifdef HAVE_IPV6
					}
				} else {
					memcpy(&srcaddr.addr6, res->ai_addr, res->ai_addrlen);
					freeaddrinfo(res);
				}
				if (icmp6_sock<0){
					logit("Sorry, IPv6 is not available\n");
					logit("Ignoring target %s\n",tc->name);
					continue;
				}
				srcaddr.addr.sa_family=AF_INET6;
#endif
			}
			t=NEW(struct target,1);
			t->name=strdup(tc->name);
			t->description=strdup(tc->description);
			debug("Creating new target %s(%s)", t->name, t->description);
			t->addr=addr;
			t->ifaddr=srcaddr;
			t->next=targets;
			t->config=tc;
			targets=t;
			if(t->addr.addr.sa_family==AF_INET) make_icmp_socket(t);
			if(t->addr.addr.sa_family==AF_INET6) make_icmp6_socket(t);
		}
		l=tc->avg_loss_delay_samples+tc->avg_loss_samples;
		if (t->queue) {
			if (l > (t->config->avg_loss_delay_samples+t->config->avg_loss_samples)) {
				t->queue = realloc(t->queue, l);
				assert(t->queue != NULL);
				memset(t->queue+(t->config->avg_loss_delay_samples+t->config->avg_loss_samples), 0, l - (t->config->avg_loss_delay_samples+t->config->avg_loss_samples));
			} else if (l < (t->config->avg_loss_delay_samples+t->config->avg_loss_samples)) {
				t->queue = realloc(t->queue, l);
				assert(t->queue != NULL);
			}
		} else {
			t->queue=NEW(char,l);
			assert(t->queue!=NULL);
		}

		/* t->recently_lost=tc->avg_loss_samples; */
		l=tc->avg_delay_samples;
		if (t->rbuf) {
			if (l > t->config->avg_delay_samples) {
				t->rbuf= realloc(t->rbuf, sizeof(double) * l);
				assert(t->rbuf!= NULL);
				memset(t->queue+t->config->avg_delay_samples, 0, l - t->config->avg_delay_samples);
			} else if (l < t->config->avg_delay_samples) {
				int tmp;
				for (tmp = l; tmp < t->config->avg_delay_samples;tmp++)
					t->delay_sum -= t->rbuf[tmp];
				t->rbuf= realloc(t->rbuf, sizeof(double) * l);
				assert(t->rbuf!= NULL);
			}
		} else {
			t->rbuf=NEW(double,l);
			assert(t->rbuf!=NULL);
		}
		t->config=tc;
	}

	if (targets==NULL){
		logit("No usable targets found, exiting");
		exit(1);
	}
	gettimeofday(&operation_started,NULL);
	if (cfg->rrd_interval)
		rrd_create();
}

void free_targets(void){
struct target *t,*nt;
struct active_alarm_list *al,*nal;

	/* delete all not configured targets */
	for(t=targets;t;t=nt){
		nt=t->next;
		for(al=t->active_alarms;al;al=nal){
			nal=al->next;
			free(al);
		}
		if (t->socket)
			close(t->socket);
		free(t->queue);
		free(t->rbuf);
		free(t->name);
		free(t->description);
		free(t);
	}
}


void reload_config(void){
	if (load_config(config_file))
                logit("Couldn't read config (\"%s\").",config_file);
}

void write_status(void){
FILE *f;
struct target *t;
struct active_alarm_list *al;
struct alarm_cfg *a;
time_t tm;
#if 0
int i,qp,really_lost;
char *buf1,*buf2;
#endif

	if (config->status_file==NULL) return;

	f=fopen(config->status_file,"w");
	if (f==NULL){
		logit("Couldn't open status file");
		myperror(config->status_file);
		return;
	}
	tm=time(NULL);
	for(t=targets;t;t=t->next){
		fprintf(f,"%s|%s|%s|%i|%i|%u|",t->name, t->config->srcip, t->description, t->last_sent+1,
			t->received, t->last_received_tv.tv_sec);
		fprintf(f,"%0.3fms|", AVG_DELAY(t));
		if (AVG_LOSS_KNOWN(t)){
			fprintf(f,"%0.1f%%",AVG_LOSS(t));
		}
		fprintf(f, "|");
		if (t->config->force_down == 1)
			fprintf(f,"force_down");
		else if (t->active_alarms){
			for(al=t->active_alarms;al;al=al->next){
				a=al->alarm;
				fprintf(f,"%s",a->name);
			}
		}
		else fprintf(f,"none");

#if 0
		buf1=NEW(char,t->config->avg_loss_delay_samples+1);
		buf2=NEW(char,t->config->avg_loss_samples+1);
		i=t->last_sent%(t->config->avg_loss_delay_samples+t->config->avg_loss_samples);
		for(i=0;i<t->config->avg_loss_delay_samples;i++){
			if (i>=t->last_sent){
				buf1[t->config->avg_loss_delay_samples-i-1]='-';
				continue;
			}
			qp=(t->last_sent-i)%(t->config->avg_loss_delay_samples+
							t->config->avg_loss_samples);
			if (t->queue[qp])
				buf1[t->config->avg_loss_delay_samples-i-1]='#';
			else
				buf1[t->config->avg_loss_delay_samples-i-1]='.';
		}
		buf1[i]=0;
		really_lost=0;
		for(i=0;i<t->config->avg_loss_samples;i++){
			if (i>=t->last_sent-t->config->avg_loss_delay_samples){
				buf2[t->config->avg_loss_samples-i-1]='-';
				continue;
			}
			qp=(t->last_sent-i-t->config->avg_loss_delay_samples)
				%(t->config->avg_loss_delay_samples+t->config->avg_loss_samples);
			if (t->queue[qp])
				buf2[t->config->avg_loss_samples-i-1]='#';
			else{
				buf2[t->config->avg_loss_samples-i-1]='.';
				really_lost++;
			}
		}
		buf2[i]=0;
		if (t->recently_lost!=really_lost){
			logit("Target \"%s\": Lost packet count mismatch (%i(recently_lost) != %i(really_lost))!",t->name,t->recently_lost,really_lost);
			logit("Target \"%s\": Received packets buffer: %s %s\n",t->name,buf2,buf1);
			//t->recently_lost = really_lost = 0;
		}
		free(buf1);
		free(buf2);
#endif

		fprintf(f,"\n");
	}
	fclose(f);
}

void main_loop(void){
struct target *t;
struct timeval cur_time,next_status={0,0},tv,next_report={0,0},next_rrd_update={0,0}, event_time;
struct pollfd pfd[1024];
int timeout, timedelta;
int npfd=0;
int i;
char buf[100];
int downtime;
struct alarm_list *al,*nal;
struct active_alarm_list *aal;
struct alarm_cfg *a;

	configure_targets(config, 0);
	memset(&pfd, '\0', sizeof pfd);

	if (config->status_interval){
		gettimeofday(&cur_time,NULL);
		tv.tv_sec=config->status_interval/1000;
		tv.tv_usec=(config->status_interval%1000)*1000;
		timeradd(&cur_time,&tv,&next_status);
	}
	while(!interrupted_by){
		npfd = 0;
		gettimeofday(&cur_time,NULL);
		if ( !timercmp(&next_probe,&cur_time,>) )
			timerclear(&next_probe);
		for(t=targets;t;t=t->next){
			if (t->socket){
				pfd[npfd].events=POLLIN|POLLERR|POLLHUP|POLLNVAL;
				pfd[npfd].revents=0;
				pfd[npfd++].fd=t->socket;
			}
			for(al=t->config->alarms;al;al=nal){
				a=al->alarm;
				nal=al->next;
				if (a->type!=AL_DOWN || is_alarm_on(t,a)) continue;
				if (timerisset(&t->last_received_tv)){
					timersub(&cur_time,&t->last_received_tv,&tv);
				}
				else {
					timersub(&cur_time,&operation_started,&tv);
				}
				downtime=tv.tv_sec*1000+tv.tv_usec/1000;
				if (timedelta > 0)
					downtime -= timedelta;
				if ( downtime > a->p.val)
					toggle_alarm(t,a,1);
			}
			if (scheduled_event(&(t->next_probe),&cur_time,t->config->interval)){
				send_probe(t);
			}
		}
		gettimeofday(&event_time,NULL);
		if (reload_request){
			reload_request=0;
			logit("SIGHUP received, reloading configuration.");
			reload_config();
			signal(SIGHUP, signal_handler);
		}
		for(t=targets;t;t=t->next){
			for(aal=t->active_alarms;aal;aal=aal->next){
				a=aal->alarm;
				if (a->repeat_interval<=0)
					continue;
				if (!scheduled_event(&aal->next_repeat,&cur_time,a->repeat_interval))
					continue;
				if (a->repeat_max && aal->num_repeats>=a->repeat_max)
					continue;
				aal->num_repeats++;
				debug("Repeating reports...");
				make_reports(t, a, 1);
			}
		}
		if (config->status_interval){
			if (scheduled_event(&next_status,&cur_time,config->status_interval)){
				if (config->status_file) write_status();
				status_request=0;
			}
		}
		if (status_request){
			status_request=0;
			if (config->status_file){
				debug("SIGUSR1 received, writing status.");
				write_status();
			}
			signal(SIGUSR1, signal_handler);
		}
		if (config->rrd_interval){
			if (scheduled_event(&next_rrd_update,&cur_time,config->rrd_interval)){
				rrd_update();
			}
		}
		if (delayed_reports){
			if (timerisset(&next_report) && timercmp(&next_report,&cur_time,<)){
				make_delayed_reports();
				timerclear(&next_report);
			}
		}
		if (delayed_reports){
			if (!timerisset(&next_report)){
				tv.tv_sec=delayed_reports->a->combine_interval/1000;
				tv.tv_usec=(delayed_reports->a->combine_interval%1000)*1000;
				timeradd(&delayed_reports->timestamp,&tv,&next_report);
			}
			if (!timerisset(&next_probe) || timercmp(&next_report,&next_probe,<))
				next_probe=next_report;
		}

#if 0
		strftime(buf,100,"%b %d %H:%M:%S",localtime(&next_probe.tv_sec));
		debug("Next event scheduled for %s",buf);
#endif

		gettimeofday(&cur_time,NULL);
		if (timercmp(&cur_time,&event_time,<)){
			timedelta=0;
		}
		else{
			timersub(&cur_time,&event_time,&tv);
			timedelta=tv.tv_usec/1000+tv.tv_sec*1000;
		}
		if (timercmp(&next_probe,&cur_time,<)){
			timeout=0;
		}
		else{
			timersub(&next_probe,&cur_time,&tv);
			timeout=tv.tv_usec/1000+tv.tv_sec*1000;
		}
		debug("Polling, timeout: %5.3fs",((double)timeout)/1000);
		if (poll(pfd,npfd,timeout) < 0)
			continue;
		gettimeofday(&cur_time,NULL);
		for(i=0;i<npfd;i++){
			if (!pfd[i].revents&POLLIN) continue;
			for(t=targets;t;t=t->next){
				if (t->addr.addr.sa_family==AF_INET) {
					if (t->socket == pfd[i].fd) {
						recv_icmp(t, &cur_time, timedelta);
						break;
					}
				}
				else if (t->addr.addr.sa_family==AF_INET6) {
					if (t->socket == pfd[i].fd) {
						recv_icmp6(t, &cur_time, timedelta);
						break;
					}
				}
			}
			pfd[i].revents=0;
		}
	}
	while(delayed_reports!=NULL) make_delayed_reports();
	free_targets();
	if (macros_buf!=NULL) free(macros_buf);
}
