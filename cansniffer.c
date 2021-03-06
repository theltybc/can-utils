/*
 * cansniffer.c
 *
 * Copyright (c) 2002-2007 Volkswagen Group Electronic Research
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of Volkswagen nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * Alternatively, provided that this notice is retained in full, this
 * software may be distributed under the terms of the GNU General
 * Public License ("GPL") version 2, in which case the provisions of the
 * GPL apply INSTEAD OF those given above.
 *
 * The provided data structures and external interfaces from this code
 * are not restricted to be used by modules with a GPL compatible license.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * Send feedback to <linux-can@vger.kernel.org>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <libgen.h>
#include <time.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <net/if.h>

#include <linux/can.h>
#include <linux/can/raw.h>

#include "terminal.h"

#define U64_DATA(p) (*(unsigned long long*)(p)->data)

#define SETFNAME "sniffset."
#define ANYDEV   "any"
#define SNIFTABLENEXP 11 /* max. 16 due to u16 indices */
#define SNIFTABLEN (1 << SNIFTABLENEXP) /* max. 2^16 due to u16 indices */
#define EFFIDXTABLENEXP 10
#define EFFIDXTABLEN (1 << EFFIDXTABLENEXP) /* idx pointers into sniftab for EFF */
#define SFFIDXTABLEN (1 << CAN_SFF_ID_BITS)
#define MAX_IFACE 4
#define MAXCOL 6
#define COLSTRSZ 20

/* flags */

#define ENABLE  1 /* by filter or user */
#define DISPLAY 2 /* is on the screen */
#define UPDATE  4 /* needs to be printed on the screen */
#define CLRSCR  8 /* clear screen in next loop */

/* flags testing & setting */

#define is_set(id, flag) (sniftab[id].flags & flag)
#define is_clr(id, flag) (!(sniftab[id].flags & flag))

#define do_set(id, flag) (sniftab[id].flags |= flag)
#define do_clr(id, flag) (sniftab[id].flags &= ~flag)

/* time defaults */

#define TIMEOUT 500 /* in 10ms */
#define HOLD    100 /* in 10ms */
#define LOOP     20 /* in 10ms */

#define ATTCOLOR ATTBOLD FGRED

#define BOLD    ATTBOLD
#define RED     ATTBOLD FGRED
#define GREEN   ATTBOLD FGGREEN
#define YELLOW  ATTBOLD FGYELLOW
#define BLUE    ATTBOLD FGBLUE
#define MAGENTA ATTBOLD FGMAGENTA
#define CYAN    ATTBOLD FGCYAN

const char col_on [MAXCOL][COLSTRSZ] = {BLUE, MAGENTA, RED, BOLD, GREEN, CYAN};

static struct snif {
	int flags;
	int ifnum;
	int next_by_id;
	int next_by_if;
	long hold;
	long timeout;
	struct timeval laststamp;
	struct timeval currstamp;
	struct canfd_frame last;
	struct canfd_frame current;
	struct canfd_frame marker;
	struct canfd_frame notch;
} sniftab[SNIFTABLEN];

static struct iface {
	int ifindex;
	char ifname[IFNAMSIZ];
	char colorstr[COLSTRSZ];
	uint16_t firstsffidx;
	uint16_t firsteffidx;
	uint16_t sff2idx[SFFIDXTABLEN];
	uint16_t eff2idxstart[EFFIDXTABLEN];
} ifacetab[MAX_IFACE];


extern int optind, opterr, optopt;

static int running = 1;
static int clearscreen = 1;
static int print_eff;
static int notch;
static int filter_id_only;
static long timeout = TIMEOUT;
static long hold = HOLD;
static long loop = LOOP;
static unsigned char binary;
static unsigned char binary_gap;
static unsigned char color;
static char interface[4*(COLSTRSZ+IFNAMSIZ)];

void print_snifline(canid_t id);
int handle_keyb(int fd);
int handle_raw(int fd, long currcms);
int handle_timeo(int fd, long currcms);
void writesettings(char* name);
void readsettings(char* name, int sockfd);

void print_usage(char *prg)
{
	const char manual [] = {
		"commands that can be entered at runtime:\n"
		"\n"
		"q<ENTER>       - quit\n"
		"b<ENTER>       - toggle binary / HEX-ASCII output\n"
		"B<ENTER>       - toggle binary with gap / HEX-ASCII output (exceeds 80 chars!)\n"
		"c<ENTER>       - toggle color mode\n"
		"#<ENTER>       - notch currently marked/changed bits (can be used repeatedly)\n"
		"*<ENTER>       - clear notched marked\n"
		"rMYNAME<ENTER> - read settings file (filter/notch)\n"
		"wMYNAME<ENTER> - write settings file (filter/notch)\n"
		"+FILTER<ENTER> - add CAN-IDs to sniff\n"
		"-FILTER<ENTER> - remove CAN-IDs to sniff\n"
		"\n"
		"FILTER can be a single CAN-ID or a CAN-ID/Bitmask:\n"
		"+1F5<ENTER>    - add CAN-ID 0x1F5\n"
		"-42E<ENTER>    - remove CAN-ID 0x42E\n"
		"-42E7FF<ENTER> - remove CAN-ID 0x42E (using Bitmask)\n"
		"-500700<ENTER> - remove CAN-IDs 0x500 - 0x5FF\n"
		"+400600<ENTER> - add CAN-IDs 0x400 - 0x5FF\n"
		"+000000<ENTER> - add all CAN-IDs\n"
		"-000000<ENTER> - remove all CAN-IDs\n"
		"\n"
		"if (id & filter) == (sniff-id & filter) the action (+/-) is performed,\n"
		"which is quite easy when the filter is 000\n"
		"\n"
	};

	fprintf(stderr, "\nUsage: %s <can-interface> [<can-interface>*]\n", prg);
	fprintf(stderr, "Options: -m <mask>  (initial FILTER default 0x00000000)\n");
	fprintf(stderr, "         -v <value> (initial FILTER default 0x00000000)\n");
	fprintf(stderr, "         -q         (quiet - all IDs deactivated)\n");
	fprintf(stderr, "         -r <name>  (read %sname from file)\n", SETFNAME);
	fprintf(stderr, "         -e         (set extended frame format output)\n");
	fprintf(stderr, "         -b         (start with binary mode)\n");
	fprintf(stderr, "         -B         (start with binary mode with gap - exceeds 80 chars!)\n");
	fprintf(stderr, "         -c         (color changes)\n");
	fprintf(stderr, "         -f         (filter on CAN-ID only)\n");
	fprintf(stderr, "         -t <time>  (timeout for ID display [x10ms] default: %d, 0 = OFF)\n", TIMEOUT);
	fprintf(stderr, "         -h <time>  (hold marker on changes [x10ms] default: %d)\n", HOLD);
	fprintf(stderr, "         -l <time>  (loop time (display) [x10ms] default: %d)\n", LOOP);
	fprintf(stderr, "Use interface name '%s' to receive from all can-interfaces\n", ANYDEV);
	fprintf(stderr, "\n");
	fprintf(stderr, "%s", manual);
}

void sigterm(int signo)
{
	running = 0;
}

int main(int argc, char **argv)
{
	fd_set rdfs;
	int s;
	int num_ifaces = 0;
	canid_t mask = 0;
	canid_t value = 0;
	long currcms = 0;
	long lastcms = 0;
	unsigned char quiet = 0;
	int opt, ret;
	const int canfd_on = 1;
	struct timeval timeo, start_tv, tv;
	struct sockaddr_can addr;
	int i;


	signal(SIGTERM, sigterm);
	signal(SIGHUP, sigterm);
	signal(SIGINT, sigterm);

	for (i=0; i < 2048 ;i++) /* default: check all CAN-IDs */
		do_set(i, ENABLE);

	while ((opt = getopt(argc, argv, "m:v:r:t:h:l:qebBcf?")) != -1) {
		switch (opt) {
		case 'm':
			sscanf(optarg, "%x", &mask);
			break;

		case 'v':
			sscanf(optarg, "%x", &value);
			break;

		case 'r':
			readsettings(optarg, 0); /* no BCM-setting here */
			break;

		case 't':
			sscanf(optarg, "%ld", &timeout);
			break;

		case 'h':
			sscanf(optarg, "%ld", &hold);
			break;

		case 'l':
			sscanf(optarg, "%ld", &loop);
			break;

		case 'q':
			quiet = 1;
			break;

		case 'e':
			print_eff = 1;
			break;

		case 'b':
			binary = 1;
			binary_gap = 0;
			break;

		case 'B':
			binary = 1;
			binary_gap = 1;
			break;

		case 'c':
			color = 1;
			break;

		case 'f':
			filter_id_only = 1;
			break;

		case '?':
			break;

		default:
			fprintf(stderr, "Unknown option %c\n", opt);
			break;
		}
	}

	num_ifaces = argc - optind;
	if ((optind == argc) || (num_ifaces > MAX_IFACE)) {
		print_usage(basename(argv[0]));
		exit(0);
	}

	/* fill interface table with up to MAX_IFACE interfaces */
	for (i=0; i < num_ifaces; i++) {

		if (strlen(argv[optind+i]) > IFNAMSIZ-1) {
			printf("name of CAN device '%s' is too long!\n", argv[optind+i]);
			return 1;
		}

		strcpy(ifacetab[i].ifname, argv[optind+i]);
		ifacetab[i].ifindex = if_nametoindex(ifacetab[i].ifname);
		if (!ifacetab[i].ifindex) {
			printf("CAN device '%s' is not available!\n", argv[optind+i]);
			return 1;
		}
	}

	if (num_ifaces > 1) {
		for (i=0; i < num_ifaces; i++) {
			snprintf(ifacetab[i].colorstr, COLSTRSZ, "%s", col_on[i]);
			strcat(interface, ifacetab[i].colorstr);
			strcat(interface, ifacetab[i].ifname);
			strcat(interface, " " ATTRESET);
		}
	} else
		sprintf(interface, "%s ", ifacetab[0].ifname);

	s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if (s < 0) {
		perror("socket");
		return 1;
	}

	addr.can_family = AF_CAN;
	addr.can_ifindex = 0; /* any can interface */

	/* try to switch the socket into CAN FD mode */
	setsockopt(s, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &canfd_on, sizeof(canfd_on));

	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("connect");
		return 1;
	}

#if 0
	for (i=0; i < 2048 ;i++) /* initial BCM setup */
		if (is_set(i, ENABLE))
			rx_setup(s, i);
#endif

	gettimeofday(&start_tv, NULL);
	tv.tv_sec = tv.tv_usec = 0;

	printf("%s", CSR_HIDE); /* hide cursor */

	while (running) {

		FD_ZERO(&rdfs);
		FD_SET(0, &rdfs);
		FD_SET(s, &rdfs);

		timeo.tv_sec  = 0;
		timeo.tv_usec = 10000 * loop;

		if ((ret = select(s+1, &rdfs, NULL, NULL, &timeo)) < 0) {
			//perror("select");
			running = 0;
			continue;
		}

		gettimeofday(&tv, NULL);
		currcms = (tv.tv_sec - start_tv.tv_sec) * 100 + (tv.tv_usec / 10000);

		if (FD_ISSET(0, &rdfs))
			running &= handle_keyb(s);

		if (FD_ISSET(s, &rdfs))
			running &= handle_raw(s, currcms);

		if (currcms - lastcms >= loop) {
			running &= handle_timeo(s, currcms);
			lastcms = currcms;
		}
	}

	printf("%s", CSR_SHOW); /* show cursor */

	close(s);
	return 0;
}

int handle_keyb(int fd){

	char cmd [20] = {0};
	int i;
	unsigned int mask;
	unsigned int value;

	if (read(0, cmd, 19) > strlen("+123456\n"))
		return 1; /* ignore */

	if (strlen(cmd) > 0)
		cmd[strlen(cmd)-1] = 0; /* chop off trailing newline */

	switch (cmd[0]) {

	case '+':
	case '-':
		sscanf(&cmd[1], "%x", &value);
		if (strlen(&cmd[1]) > 3) {
			mask = value & 0xFFF;
			value >>= 12;
		}
		else
			mask = 0x7FF;

		if (cmd[0] == '+') {
			for (i=0; i < 2048 ;i++) {
				if (((i & mask) == (value & mask)) && (is_clr(i, ENABLE))) {
					do_set(i, ENABLE);
					//rx_setup(fd, i);
				}
			}
		}
		else { /* '-' */
			for (i=0; i < 2048 ;i++) {
				if (((i & mask) == (value & mask)) && (is_set(i, ENABLE))) {
					do_clr(i, ENABLE);
					//rx_delete(fd, i);
				}
			}
		}
		break;

	case 'w' :
		writesettings(&cmd[1]);
		break;

	case 'r' :
		readsettings(&cmd[1], fd);
		break;

	case 'q' :
		running = 0;
		break;

	case 'B' :
		binary_gap = 1;
		if (binary)
			binary = 0;
		else
			binary = 1;

		break;

	case 'b' :
		binary_gap = 0;
		if (binary)
			binary = 0;
		else
			binary = 1;

		break;

	case 'c' :
		if (color)
			color = 0;
		else
			color = 1;

		break;

	case '#' :
		notch = 1;
		break;

	case '*' :
		for (i=0; i < 2048; i++)
			U64_DATA(&sniftab[i].notch) = (__u64) 0;
		break;

	default:
		break;
	}

	clearscreen = 1;

	return 1; /* ok */
};

int handle_raw(int fd, long currcms){

	int nbytes, id;
	struct canfd_frame cf;

	if ((nbytes = read(fd, &cf, sizeof(cf))) < 0) {
		perror("raw read");
		return 0; /* quit */
	}

	if (!print_eff && (cf.can_id & CAN_EFF_FLAG)) {
		print_eff = 1;
		clearscreen = 1;
	}

	id = cf.can_id & 0x7FF;
	ioctl(fd, SIOCGSTAMP, &sniftab[id].currstamp);

	if ((nbytes != CAN_MTU) && (nbytes != CANFD_MTU)) {
		printf("received strange frame data length %d!\n", nbytes);
		return 0; /* quit */
	}

	sniftab[id].current = cf;
	U64_DATA(&sniftab[id].marker) |= 
		U64_DATA(&sniftab[id].current) ^ U64_DATA(&sniftab[id].last);
	sniftab[id].timeout = (timeout)?(currcms + timeout):0;

	if (is_clr(id, DISPLAY))
		clearscreen = 1; /* new entry -> new drawing */

	do_set(id, DISPLAY);
	do_set(id, UPDATE);
	
	return 1; /* ok */
};

int handle_timeo(int fd, long currcms){

	int i;
	int force_redraw = 0;
	static unsigned int frame_count;

	if (clearscreen) {

		if (print_eff)
			printf("%s%sXX|ms  -- ID --  data ...     < %s# l=%ld h=%ld t=%ld >",
			       CLR_SCREEN, CSR_HOME, interface, loop, hold, timeout);
		else
			printf("%s%sXX|ms  ID   data ...     < %s# l=%ld h=%ld t=%ld >",
			       CLR_SCREEN, CSR_HOME, interface, loop, hold, timeout);

		force_redraw = 1;
		clearscreen = 0;
	}

	if (notch) {
		for (i=0; i < 2048; i++)
			U64_DATA(&sniftab[i].notch) |= U64_DATA(&sniftab[i].marker);
		notch = 0;
	}

	printf("%s", CSR_HOME);
	printf("%02d\n", frame_count++); /* rolling display update counter */
	frame_count %= 100;

	for (i=0; i < 2048; i++) {

		if is_set(i, ENABLE) {

				if is_set(i, DISPLAY) {

						if (is_set(i, UPDATE) || (force_redraw)){
							print_snifline(i);
							sniftab[i].hold = currcms + hold;
							do_clr(i, UPDATE);
						}
						else
							if ((sniftab[i].hold) && (sniftab[i].hold < currcms)) {
								U64_DATA(&sniftab[i].marker) = (__u64) 0;
								print_snifline(i);
								sniftab[i].hold = 0; /* disable update by hold */
							}
							else
								printf("%s", CSR_DOWN); /* skip my line */

						if (sniftab[i].timeout && sniftab[i].timeout < currcms) {
							do_clr(i, DISPLAY);
							do_clr(i, UPDATE);
							clearscreen = 1; /* removed entry -> new drawing next time */
						}
					}
				sniftab[i].last      = sniftab[i].current;
				sniftab[i].laststamp = sniftab[i].currstamp;
			}
	}

	return 1; /* ok */

};

void print_snifline(canid_t id){

	long diffsec  = sniftab[id].currstamp.tv_sec  - sniftab[id].laststamp.tv_sec;
	long diffusec = sniftab[id].currstamp.tv_usec - sniftab[id].laststamp.tv_usec;
	int dlc_diff  = sniftab[id].last.len - sniftab[id].current.len;
	int i,j;

	if (diffusec < 0)
		diffsec--, diffusec += 1000000;

	if (diffsec < 0)
		diffsec = diffusec = 0;

	if (diffsec >= 100)
		diffsec = 99, diffusec = 999999;

	printf("%s", ifacetab[0].colorstr);
	if (id & CAN_EFF_FLAG)
		printf("%02ld%03ld  %08X  ", diffsec, diffusec/1000, id & CAN_EFF_MASK);
	else if (print_eff)
		printf("%02ld%03ld  %08X  ", diffsec, diffusec/1000, sniftab[id].current.can_id & CAN_EFF_MASK);
	else
		printf("%02ld%03ld  %03X  ", diffsec, diffusec/1000, id & CAN_SFF_MASK);
	printf("%s", ATTRESET);

	if (binary) {

		for (i=0; i<sniftab[id].current.len; i++) {
			for (j=7; j>=0; j--) {
				if ((color) && (sniftab[id].marker.data[i] & 1<<j) &&
				    (!(sniftab[id].notch.data[i] & 1<<j)))
					if (sniftab[id].current.data[i] & 1<<j)
						printf("%s1%s", ATTCOLOR, ATTRESET);
					else
						printf("%s0%s", ATTCOLOR, ATTRESET);
				else
					if (sniftab[id].current.data[i] & 1<<j)
						putchar('1');
					else
						putchar('0');
			}
			if (binary_gap)
				putchar(' ');
		}

		/*
		 * when the len decreased (dlc_diff > 0),
		 * we need to blank the former data printout
		 */
		for (i=0; i<dlc_diff; i++) {
			printf("        ");
			if (binary_gap)
				putchar(' ');
		}
	}
	else {

		for (i=0; i<sniftab[id].current.len; i++)
			if ((color) && (sniftab[id].marker.data[i]) && (!(sniftab[id].notch.data[i])))
				printf("%s%02X%s ", ATTCOLOR, sniftab[id].current.data[i], ATTRESET);
			else
				printf("%02X ", sniftab[id].current.data[i]);

		if (sniftab[id].current.len < 8)
			printf("%*s", (8 - sniftab[id].current.len) * 3, "");

		for (i=0; i<sniftab[id].current.len; i++)
			if ((sniftab[id].current.data[i] > 0x1F) && 
			    (sniftab[id].current.data[i] < 0x7F))
				if ((color) && (sniftab[id].marker.data[i]) && (!(sniftab[id].notch.data[i])))
					printf("%s%c%s", ATTCOLOR, sniftab[id].current.data[i], ATTRESET);
				else
					putchar(sniftab[id].current.data[i]);
			else
				putchar('.');

		/*
		 * when the len decreased (dlc_diff > 0),
		 * we need to blank the former data printout
		 */
		for (i=0; i<dlc_diff; i++)
			putchar(' ');
	}

	putchar('\n');

	U64_DATA(&sniftab[id].marker) = (__u64) 0;

};


void writesettings(char* name){

	int fd;
	char fname[30] = SETFNAME;
	int i,j;
	char buf[8]= {0};

	strncat(fname, name, 29 - strlen(fname)); 
	fd = open(fname,  O_WRONLY|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
    
	if (fd > 0) {

		for (i=0; i < 2048 ;i++) {
			sprintf(buf, "<%03X>%c.", i, (is_set(i, ENABLE))?'1':'0');
			write(fd, buf, 7);
			for (j=0; j<8 ; j++){
				sprintf(buf, "%02X", sniftab[i].notch.data[j]);
				write(fd, buf, 2);
			}
			write(fd, "\n", 1);
			/* 7 + 16 + 1 = 24 bytes per entry */ 
		}
		close(fd);
	}
	else
		printf("unable to write setting file '%s'!\n", fname);
};

void readsettings(char* name, int sockfd){

	int fd;
	char fname[30] = SETFNAME;
	char buf[25] = {0};
	int i,j;

	strncat(fname, name, 29 - strlen(fname)); 
	fd = open(fname, O_RDONLY);
    
	if (fd > 0) {
		if (!sockfd)
			printf("reading setting file '%s' ... ", fname);

		for (i=0; i < 2048 ;i++) {
			if (read(fd, &buf, 24) == 24) {
				if (buf[5] & 1) {
					if (is_clr(i, ENABLE)) {
						do_set(i, ENABLE);
						//if (sockfd)
							//rx_setup(sockfd, i);
					}
				}
				else
					if (is_set(i, ENABLE)) {
						do_clr(i, ENABLE);
						//if (sockfd)
							//rx_delete(sockfd, i);
					}
				for (j=7; j>=0 ; j--){
					sniftab[i].notch.data[j] =
						(__u8) strtoul(&buf[2*j+7], (char **)NULL, 16) & 0xFF;
					buf[2*j+7] = 0; /* cut off each time */
				}
			}
			else {
				if (!sockfd)
					printf("was only able to read until index %d from setting file '%s'!\n",
					       i, fname);
			}
		}
    
		if (!sockfd)
			printf("done\n");

		close(fd);
	}
	else
		printf("unable to read setting file '%s'!\n", fname);
};
