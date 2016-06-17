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
#ifdef HAVE_SYS_WAIT_H
# include <sys/wait.h>
#endif
#ifdef HAVE_ERRNO_H
# include <errno.h>
#endif

#include "conf.h"
#include "debug.h"
#include "rrd.h"

struct target *targets = NULL;

struct config default_config = {
	.timestamp_format = "%b %d %H:%M:%S",
	.pid_file = "/var/run/apinger.pid",
	.mailer = "/usr/lib/sendmail -t",
	.alarm_defaults = {
		.mailsubject = "%r: %T(%t) *** %a ***",
		.mailfrom = "nobody",
		.name = "default",
		.type = AL_NONE,
	},
	.target_defaults = {
		.avg_loss_delay_samples = 5,
		.avg_delay_samples = 20,
		.avg_loss_samples = 50,
		.description = "",
		.name = "default",
		.interval = 1000,
		.srcip = "",
	},
};

int foreground = 1;
char *config_file = CONFIG;

uint16_t ident;

struct timeval next_probe = { 0, 0 };

/* Interrupt handler */
typedef void (*sighandler_t)(int);
volatile int reload_request = 0;
volatile int status_request = 0;
volatile int interrupted_by = 0;
volatile int sigpipe_received = 0;

void
signal_handler(int signum)
{
	if (signum == SIGPIPE) {
		signal(SIGPIPE, SIG_IGN);
		sigpipe_received = 1;
	} else if (signum == SIGHUP) {
		signal(SIGHUP, SIG_IGN);
		reload_request = 1;
	} else if (signum == SIGUSR1) {
		signal(SIGUSR1, SIG_IGN);
		status_request = 1;
	} else {
		interrupted_by = signum;
	}
}

#ifdef FORKED_RECEIVER
void sigchld_handler (int signum) {
int pid, status, serrno;

	serrno = errno;
	while (1) {
		   pid = waitpid (WAIT_ANY, &status, WNOHANG);
		   if (pid <= 0) break;
	}
	errno = serrno;
}
#endif

static void
usage(void)
{
	fprintf(stderr,"Alarm Pinger " PACKAGE_VERSION " (c) 2002 Jacek Konieczny <jajcus@jajcus.net>\n");
	fprintf(stderr,"Usage:\n");
	fprintf(stderr,"\tapinger [-c <file>] [-f] [-d]\n");
	fprintf(stderr,"\tapinger [-c <file>] -g <dir> [-l <location>]\n");
	fprintf(stderr,"\tapinger -h\n");
	fprintf(stderr,"\n");
	fprintf(stderr,"\t-c <file>\talternate config file path.\n");
	fprintf(stderr,"\t-t\ttest config and exit.\n");
	fprintf(stderr,"\t-f\trun in foreground.\n");
	fprintf(stderr,"\t-d\tdebug on.\n");
	fprintf(stderr,"\t-g <dir>\tgenerate simple rrd-cgi script.\n");
	fprintf(stderr,		"\t\t<dir> is a directory where generated graph will be stored.\n");
	fprintf(stderr,"\t-l <location>\tHTTP location of generated graphs.\n");
	fprintf(stderr,"\t-h\tthis help message.\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	char *graph_location = "/apinger/";
	int stay_foreground = 0;
	char *graph_dir = NULL;
	int config_test = 0;
	int do_debug = 0;
	FILE *pidfile;
	pid_t pid;
	int i;
	int c;

	while ((c = getopt(argc,argv,"c:dfg:hl:t")) != -1) {
		switch (c) {
		case 'c':
			config_file = optarg;
			break;
		case 'd':
			do_debug = 1;
			break;
		case 'f':
			stay_foreground = 1;
			break;
		case 'g':
			graph_dir = optarg;
			break;
		case 'l':
			graph_location = optarg;
			break;
		case 't':
			config_test = 1;
			break;
		case 'h':
		default:
			usage();
			/* NOTREACHED */
		}
	}

	if (load_config(config_file)) {
		logit("Couldn't read config (\"%s\").", config_file);
		return (1);
	}

	if (config_test) {
		logit("Config is fine.");
		return (0);
	}

	config->debug = do_debug;

	if (graph_dir!=NULL)
		return rrd_print_cgi(graph_dir,graph_location);

	if (!stay_foreground){
		pidfile=fopen(config->pid_file,"r");
		if (pidfile){
			int n=fscanf(pidfile,"%d",&pid);
			if (n>0 && pid>0 && kill(pid,0)==0){
				fprintf(stderr,"pinger already running\n");
				return 1;
			}
			fclose(pidfile);
		}
	}

	if (!stay_foreground){
		pid=fork();
		if (pid<0){
			perror("fork");
			exit(1);
		}
		if (pid>0){ /* parent */
			pidfile=fopen(config->pid_file,"w");
			if (!pidfile){
				fprintf(stderr,"Couldn't open pid file for writing. ");
				perror(config->pid_file);
				return 1;
			}
			fprintf(pidfile,"%i\n",pid);
			fclose(pidfile);
			free_config();
			exit(0);
		}
		foreground=0;
		for (i = 0; i < 255; i++) {
			close(i);
		}
		setsid();
	}

	ident=getpid() & 0xFFFF;
	signal(SIGTERM,signal_handler);
	signal(SIGINT,signal_handler);
	signal(SIGHUP,signal_handler);
	signal(SIGUSR1,signal_handler);
	signal(SIGPIPE,signal_handler);
#ifdef FORKED_RECEIVER
	signal(SIGCHLD,sigchld_handler);
#endif
	logit("Starting Alarm Pinger, apinger(%i)", ident);
#ifndef HAVE_CLOCK_GETTIME
	logit("Warning: Falling back to gettimeofday() usage. "
	    "Measurements may skew with e.g. NTP enabled.");
#endif

	main_loop();

	logit("Exiting on signal %i.",interrupted_by);

	if (!foreground) {
		/* clear the pid file */
		pidfile = fopen(config->pid_file, "w");
		if (pidfile) {
			fclose(pidfile);
		}
		/* try to remove it. Most probably this will fail */
		unlink(config->pid_file);
	}

	free_config();

	return 0;
}
