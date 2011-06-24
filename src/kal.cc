/*
 * Copyright (c) 2010, Joshua Lackey
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *     *  Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *     *  Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * kal
 *
 *    Two functions:
 *
 * 	1.  Calculates the frequency offset between a local GSM tower and the
 * 	    USRP clock.
 *
 *	2.  Identifies the frequency of all GSM base stations in a given band.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#else
#define PACKAGE_VERSION "custom build"
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef D_HOST_OSX
#include <libgen.h>
#endif /* D_HOST_OSX */
#include <string.h>
#include <sys/time.h>
#include <errno.h>
#include <pthread.h>

#include "usrp_source.h"
#include "fcch_detector.h"
#include "arfcn_freq.h"
#include "offset.h"
#include "c0_detect.h"
#include "version.h"
#include "socket.h"
#include "linuxlist.h"
#include "msgb.h"

static const int MAX_THREADS = 10;
static const double GSM_RATE = 1625000.0 / 6.0;

int g_verbosity = 0;
int g_debug = 0;

void usage(char *prog) {

	printf("kalibrate v%s, Copyright (c) 2010, Joshua Lackey\n", kal_version_string);
	printf("\nUsage:\n");
	printf("\tGSM Base Station Scan:\n");
	printf("\t\t%s <-s band indicator> [options]\n", basename(prog));
	printf("\n");
	printf("\tClock Offset Calculation:\n");
	printf("\t\t%s <-f frequency | -c channel> [options]\n", basename(prog));
	printf("\n");
	printf("Where options are:\n");
	printf("\t-s\tband to scan (GSM850, GSM900, EGSM, DCS, PCS)\n");
	printf("\t-f\tfrequency of nearby GSM base station\n");
	printf("\t-c\tchannel of nearby GSM base station\n");
	printf("\t-b\tband indicator (GSM850, GSM900, EGSM, DCS, PCS)\n");
	printf("\t-j\tJammer IP address\n");
	printf("\t-p\tJammer port\n");
	printf("\t-t\tBTS IP address\n");
	printf("\t-q\tBTS port\n");
	printf("\t-R\tside A (0) or B (1), defaults to B\n");
	printf("\t-A\tantenna TX/RX (0) or RX2 (1), defaults to RX2\n");
	printf("\t-g\tgain as %% of range, defaults to 45%%\n");
	printf("\t-F\tFPGA master clock frequency, defaults to 52MHz\n");
	printf("\t-x\tenable external 10MHz reference input\n");
	printf("\t-v\tverbose\n");
	printf("\t-D\tenable debug messages\n");
	printf("\t-h\thelp\n");
	exit(-1);
}

struct session {
	llist_head list;
	int fd;
	int num;
	bool running;
};

static int msg_handler(struct session *sess, struct msgb *msg)
{
	int rc;
	size_t bytes_wr;
	struct session *pos;

	switch (msg->cmd) {
	case JM_NACK_FREQ:
                fprintf(stdout, "\nReceived JM_NACK_FREQ %u\n", msg->arfcn);

		pos = llist_entry(sess->list.next, struct session, list);
		if (pos->num == 0) {
			fprintf(stdout, "Resource unavailable\n");
			return 0;
		}

		msg->cmd = JM_ADD_FREQ;
		bytes_wr = writen(pos->fd, (char *) msg, sizeof(msgb));
		if (bytes_wr != sizeof(msgb)) {
			sess->running = false;
			return -1;
		}
		break;
	default:
                fprintf(stderr, "Unknown %d command\n", msg->cmd);
                return -1;
	}

	return 0;
}

static void *ctl_loop(void *arg)
{
	struct msgb msg;
	session *sess = (struct session *) arg;
	ssize_t bytes_rd;

	while (sess->running) {
		bytes_rd = readn(sess->fd, (char *) &msg, sizeof(msg));
		if (bytes_rd != sizeof(msg))
			break;

		msg_handler(sess, &msg);
	}

	close(sess->fd);

	fprintf(stdout, "Connection closed\n");
	pthread_exit(NULL);
}

int parse_conns(char *addr_in, char *port_in, char **addr_out, char **port_out)
{
	char *addr_ptr,  *port_ptr;
	int addr_cnt, port_cnt;

	if ((addr_in == NULL) ||(port_in == NULL))
		return -1;

	addr_out[0] = addr_in;
	port_out[0] = port_in;

	addr_ptr = addr_in;
	port_ptr = port_in;

	addr_cnt = 1;
	port_cnt = 1;

	while (addr_ptr = strchr(addr_ptr, ' ')) {
		if (*++addr_ptr != ' ') 
			addr_out[addr_cnt++] = addr_ptr;
	}

	while (port_ptr = strchr(port_ptr, ' ')) {
		if (*++port_ptr != ' ')
			port_out[port_cnt++] = port_ptr++;
	}

	if (addr_cnt != port_cnt)
		return -1;

	return addr_cnt;
}

int main(int argc, char **argv) {

	char *endptr;
	char *addr_jm = NULL;
	char *port_jm = NULL;
	char *addr_bts = NULL;
	char *port_bts = NULL;

	int c, antenna = 1, bi = BI_NOT_DEFINED, chan = -1, bts_scan = 0;
	unsigned int subdev = 1;
	long int fpga_master_clock_freq = 100000000;
	bool external_ref = false;
	float gain = 0.45;
	double freq = -1.0;
	usrp_source *u;

	int bts_fd;
	pthread_t thrd[MAX_THREADS];
	session sess;
	INIT_LLIST_HEAD(&sess.list);

	while((c = getopt(argc, argv, "f:c:s:b:j:p:t:q:R:A:g:F:xvDh?")) != EOF) {
		switch(c) {
			case 'f':
				freq = strtod(optarg, 0);
				break;

			case 'c':
				chan = strtoul(optarg, 0, 0);
				break;

			case 's':
				if((bi = str_to_bi(optarg)) == -1) {
					fprintf(stderr, "error: bad band "
					   "indicator: ``%s''\n", optarg);
					usage(argv[0]);
				}
				bts_scan = 1;
				break;

			case 'b':
				if((bi = str_to_bi(optarg)) == -1) {
					fprintf(stderr, "error: bad band "
					   "indicator: ``%s''\n", optarg);
					usage(argv[0]);
				}
				break;
			case 'j':
				addr_jm = optarg;
				break;
			case 'p':
				port_jm = optarg;
				break;
			case 't':
				addr_bts = optarg;
				break;
			case 'q':
				port_bts = optarg;
				break;
			case 'R':
				errno = 0;
				subdev = strtoul(optarg, &endptr, 0);
				if((!errno) && (endptr != optarg))
					break;
				if(tolower(*optarg) == 'a') {
					subdev = 0;
				} else if(tolower(*optarg) == 'b') {
					subdev = 1;
				} else {
					fprintf(stderr, "error: bad side: "
					   "``%s''\n",
					   optarg);
					usage(argv[0]);
				}
				break;

			case 'A':
				if(!strcmp(optarg, "RX2")) {
					antenna = 1;
				} else if(!strcmp(optarg, "TX/RX")) {
					antenna = 0;
				} else {
					errno = 0;
					antenna = strtoul(optarg, &endptr, 0);
					if(errno || (endptr == optarg)) {
						fprintf(stderr, "error: bad "
						   "antenna: ``%s''\n",
						   optarg);
						usage(argv[0]);
					}
				}
				break;

			case 'g':
				gain = strtod(optarg, 0);
				if((gain > 1.0) && (gain <= 100.0))
					gain /= 100.0;
				if((gain < 0.0) || (1.0 < gain))
					usage(argv[0]);
				break;

			case 'F':
				fpga_master_clock_freq = strtol(optarg, 0, 0);
				if(!fpga_master_clock_freq)
					fpga_master_clock_freq = (long int)strtod(optarg, 0); 

				// was answer in MHz?
				if(fpga_master_clock_freq < 1000) {
					fpga_master_clock_freq *= 1000000;
				}
				break;

			case 'x':
				external_ref = true;
				break;

			case 'v':
				g_verbosity++;
				break;

			case 'D':
				g_debug = 1;
				break;

			case 'h':
			case '?':
			default:
				usage(argv[0]);
				break;
		}

	}

	// sanity check frequency / channel
	if(bts_scan) {
		if(bi == BI_NOT_DEFINED) {
			fprintf(stderr, "error: scaning requires band\n");
			usage(argv[0]);
		}
	} else {
		if(freq < 0.0) {
			if(chan < 0) {
				fprintf(stderr, "error: must enter channel or "
				   "frequency\n");
				usage(argv[0]);
			}
			if((freq = arfcn_to_freq(chan, &bi)) < 869e6)
				usage(argv[0]);
		}
		if((freq < 869e6) || (2e9 < freq)) {
			fprintf(stderr, "error: bad frequency: %lf\n", freq);
			usage(argv[0]);
		}
		chan = freq_to_arfcn(freq, &bi);
	}

	// sanity check clock
	if(fpga_master_clock_freq < 48000000) {
		fprintf(stderr, "error: FPGA master clock too slow: %li\n", fpga_master_clock_freq);
		usage(argv[0]);
	}

	if(g_debug) {
#ifdef D_HOST_OSX
		printf("debug: Mac OS X version\n");
#endif
		printf("debug: FPGA Master Clock Freq:\t%li\n", fpga_master_clock_freq);
		printf("debug: External Reference    :\t%s\n", external_ref? "Yes" : "No");
		printf("debug: RX Subdev Spec        :\t%s\n", subdev? "B" : "A");
		printf("debug: Antenna               :\t%s\n", antenna? "RX2" : "TX/RX");
		printf("debug: Gain                  :\t%f\n", gain);
	}

	char *addr_out[MAX_THREADS];
	char *port_out[MAX_THREADS];

	// parse IP addresses and ports 
	int num_conn = parse_conns(addr_jm, port_jm, addr_out, port_out);
	if (num_conn <= 0) {
		fprintf(stderr, "error: could not parse IP addresses or ports\n");
	}

	// make jammer connections
	for (int i = 0; i < num_conn; i++) {
		session *new_sess = new session();

		new_sess->fd = make_connection(addr_out[i], atoi(port_out[i]));
		if(new_sess->fd < 0) {
			fprintf(stderr, "error: could not establish signal generator connection\n");
			return -1;
		}

		new_sess->num = i;
		new_sess->running = true;

		llist_add(&new_sess->list, &sess.list);
		pthread_create(&thrd[i], NULL, &ctl_loop, (void *) new_sess);

		printf("Connection established to %s port %s\n", addr_out[i], port_out[i]);
	}

	// make bts connection
	if (addr_bts) {
		bts_fd = make_connection(addr_bts, atoi(port_bts));
		if (bts_fd < 0) {
			fprintf(stderr, "error: could not establish BTS connection\n");
			return -1;
		}
	}

	// let the device decide on the decimation
	u = new usrp_source(GSM_RATE, fpga_master_clock_freq, external_ref);
	if(!u) {
		fprintf(stderr, "error: usrp_source\n");
		return -1;
	}
	if(u->open(subdev) == -1) {
		fprintf(stderr, "error: usrp_source::open\n");
		return -1;
	}
	u->set_antenna(antenna);
	if(!u->set_gain(gain)) {
		fprintf(stderr, "error: usrp_source::set_gain\n");
		return -1;
	}

	if(!bts_scan) {
		if(!u->tune(freq)) {
			fprintf(stderr, "error: usrp_source::tune\n");
			return -1;
		}

		fprintf(stderr, "%s: Calculating clock frequency offset.\n",
		   basename(argv[0]));
		fprintf(stderr, "Using %s channel %d (%.1fMHz)\n",
		   bi_to_str(bi), chan, freq / 1e6);

		return offset_detect(u);
	}

	fprintf(stderr, "%s: Scanning for %s base stations.\n",
	   basename(argv[0]), bi_to_str(bi));

	// start loading carriers on the first connection
	struct session *pos;
	pos = llist_entry(sess.list.next, struct session, list);

	return c0_detect(u, bi, pos->fd, bts_fd);
}
