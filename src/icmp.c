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

#ifdef HAVE_STDLIB_H
# include <stdlib.h>
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#ifdef HAVE_STRING_H
# include <string.h>
#endif
#ifdef HAVE_NETINET_IN_SYSTM_H
# include <netinet/in_systm.h>
#endif
#ifdef HAVE_NETINET_IP_H
# include <netinet/ip.h>
#endif
#ifdef HAVE_NETINET_IP_ICMP_H
# include <netinet/ip_icmp.h>
#endif
#ifdef HAVE_ARPA_INET_H
# include <arpa/inet.h>
#endif
#ifdef HAVE_ERRNO_H
# include <errno.h>
#endif
#ifdef HAVE_SCHED_H
# include <sched.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
# include <sys/ioctl.h>
#endif
#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif
#ifdef HAVE_SYS_UIO_H
# include <sys/uio.h>
#endif
#include "debug.h"

/* function borrowed from iputils */
u_short in_cksum(const u_short *addr, register int len, u_short csum){

	register int nleft = len;
        const u_short *w = addr;
        register u_short answer;
        register int sum = csum;

        /*
         *  Our algorithm is simple, using a 32 bit accumulator (sum),
         *  we add sequential 16 bit words to it, and at the end, fold
         *  back all the carry bits from the top 16 bits into the lower
         *  16 bits.
         */
        while (nleft > 1)  {
                sum += *w++;
                nleft -= 2;
        }

        /* mop up an odd byte, if necessary */
        if (nleft == 1)
                sum += htons(*(u_char *)w << 8);

        /*
         * add back carry outs from top 16 bits to low 16 bits
         */
        sum = (sum >> 16) + (sum & 0xffff);     /* add hi 16 to low 16 */
        sum += (sum >> 16);                     /* add carry */
        answer = ~sum;                          /* truncate to 16 bits */
        return (answer);
}

void send_icmp_probe(struct target *t,int seq){
static char buf[1024];
struct icmp *p=(struct icmp *)buf;
struct trace_info ti;
struct timeval cur_time;
int size;
int ret;


	p->icmp_type=ICMP_ECHO;
	p->icmp_code=0;
	p->icmp_cksum=0;
	p->icmp_seq=seq%65536;
	p->icmp_id=ident;

#ifdef HAVE_SCHED_YIELD
	/* Give away our time now, or we may be stopped between apinger_gettime() and sendto() */
	sched_yield();
#endif
	apinger_gettime(&cur_time);
	ti.timestamp=cur_time;
	ti.target_id=t;
	ti.seq=seq;
	memcpy(p+1,&ti,sizeof(ti));
	size=sizeof(*p)+sizeof(ti);

	p->icmp_cksum = in_cksum((u_short *)p,size,0);
	ret=sendto(t->socket,p,size,MSG_DONTWAIT,
			(struct sockaddr *)&t->addr.addr4,sizeof(t->addr.addr4));
	if (ret<0){
		if (config->debug) myperror("sendto");
		switch (errno) {
		case EBADF:
		case ENOTSOCK:
			if (t->socket)
				close(t->socket);
			make_icmp_socket(t);
			break;
		}
	}
}

void recv_icmp(struct target *t, struct timeval *time_recv, int timedelta){
int len,hlen,icmplen,datalen;
char buf[1024];
struct sockaddr_in from;
struct icmp *icmp;
struct ip *ip;
socklen_t sl;
reloophack:

	sl=sizeof(from);
	len=recvfrom(t->socket,buf,1024,MSG_DONTWAIT,(struct sockaddr *)&from,&sl);
	if (len<0){
		if (errno==EAGAIN) return;
		myperror("recvfrom");
		return;
	}
	if (len==0) return;
	ip=(struct ip *)buf;
	hlen=ip->ip_hl*4;
	if (len<hlen+8 || ip->ip_hl<5) {
		debug("Too short packet reveiced");
		return;
	}
	icmplen=len-hlen;
	icmp=(struct icmp *)(buf+hlen);
	if (icmp->icmp_type != ICMP_ECHOREPLY){
		debug("Other (%i) icmp type received",icmp->icmp_type);
		return;
	}
	if (icmp->icmp_id != ident){
		debug("Alien echo-reply received from %s. Expected %i, received %i",inet_ntoa(from.sin_addr), ident, icmp->icmp_id);
		goto reloophack;
		return;
	}

	debug("Ping reply from %s",inet_ntoa(from.sin_addr));

	datalen=icmplen-sizeof(*icmp);
	if (datalen!=sizeof(struct trace_info)){
		debug("Packet data truncated.");
		return;
	}
	analyze_reply(time_recv,icmp->icmp_seq,(struct trace_info*)(icmp+1), timedelta);
}

int
make_icmp_socket(struct target *t)
{
	t->socket = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (t->socket < 0) {
		logit("Could not create socket on address (%s) "
		    "for monitoring address %s (%s)",
		    t->config->srcip, t->name, t->description);
		myperror("socket()");
	} else if (bind(t->socket, (struct sockaddr *)&t->ifaddr.addr4,
	    sizeof(t->ifaddr.addr4)) < 0) {
		logit("Could not bind socket on address (%s) "
		    "for monitoring address %s (%s)",
		    t->config->srcip, t->name, t->description);
		myperror("bind()");
	}

	return t->socket;
}
