/*
 *	wmediumd, wireless medium simulator for mac80211_hwsim kernel module
 *	Copyright (c) 2011 cozybit Inc.
 *
 *	Author:	Javier Lopez	<jlopex@cozybit.com>
 *		Javier Cardona	<javier@cozybit.com>
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version 2
 *	of the License, or (at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 *	02110-1301, USA.
 */

#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/family.h>
#include <stdint.h>
#include <getopt.h>
#include <signal.h>
#include <event.h>
#include <stdbool.h>
#include <sys/timerfd.h>

#include "wmediumd.h"
#include "probability.h"
#include "mac_address.h"
#include "ieee80211.h"
#include "config.h"
#include "list.h"

struct nl_msg *msg;
struct nl_cb *cb;
struct nl_cache *cache;
struct genl_family *family;

int running = 0;
struct jammer_cfg jam_cfg;
int size;

static int received = 0;
static int sent = 0;
static int dropped = 0;
static int acked = 0;

#define TIME_FMT "%lld.%06lld"
#define TIME_ARGS(a) ((unsigned long long)(a)->tv_sec), ((unsigned long long)(a)->tv_nsec/1000)

#define MAC_FMT "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC_ARGS(a) a[0],a[1],a[2],a[3],a[4],a[5]

static int index_to_rate[] = {
	60, 90, 120, 180, 240, 360, 480, 540
};

struct wmediumd
{
	int timerfd;

	struct nl_sock *sock;
	struct list_head stations;
};

struct wqueue
{
	struct list_head frames;
	int cw_min;
	int cw_max;
};

struct station
{
	char addr[ETH_ALEN];
	struct wqueue data_queue;
	struct wqueue mgmt_queue;
	struct list_head list;
};

struct frame
{
	struct list_head list;		/* frame queue list */
	struct timespec expires;	/* frame delivery (absolute) */
	bool acked;
	u64 cookie;
	int flags;
	int tx_rates_count;
	struct station *sender;
	struct hwsim_tx_rate tx_rates[IEEE80211_TX_MAX_RATES];
	size_t data_len;
	u8 data[0];			/* frame contents */
};

static inline int div_round(int a, int b)
{
	return (a + b-1) / b;
}

static inline int pkt_duration(int len, int rate)
{
	/* preamble + signal + t_sym * n_sym, rate in 100 kbps */
	return 16 + 4 + 4 * div_round((16 + 8 * len + 6) * 10, 4 * rate);
}

void wqueue_init(struct wqueue *wqueue, int cw_min, int cw_max)
{
	INIT_LIST_HEAD(&wqueue->frames);
	wqueue->cw_min = cw_min;
	wqueue->cw_max = cw_max;
}

bool timespec_before(struct timespec *t1, struct timespec *t2)
{
	return t1->tv_sec < t2->tv_sec ||
	       (t1->tv_sec == t2->tv_sec && t1->tv_nsec < t2->tv_nsec);
}

void timespec_add_usec(struct timespec *t, int usec)
{
	t->tv_nsec += usec * 1000;
	if (t->tv_nsec >= 1000000000) {
		t->tv_sec++;
		t->tv_nsec -= 1000000000;
	}
}

void rearm_timer(struct wmediumd *ctx)
{
	struct timespec min_expires;
	struct itimerspec expires = {};
	struct station *station;
	struct frame *frame;

	bool set_min_expires = false;

	/*
	 * Iterate over all the interfaces to find the next frame that
	 * will be delivered, and set the timerfd accordingly.
	 */
	list_for_each_entry(station, &ctx->stations, list) {
		frame = list_first_entry(&station->mgmt_queue.frames,
					 struct frame, list);

		if (frame && (!set_min_expires || timespec_before(&frame->expires, &min_expires))) {
			set_min_expires = true;
			min_expires = frame->expires;
		}

		frame = list_first_entry(&station->data_queue.frames,
					 struct frame, list);

		if (frame && (!set_min_expires || timespec_before(&frame->expires, &min_expires))) {
			set_min_expires = true;
			min_expires = frame->expires;
		}
	}
	expires.it_value = min_expires;
	timerfd_settime(ctx->timerfd, TFD_TIMER_ABSTIME, &expires, NULL);
}

bool frame_is_mgmt(struct frame *frame)
{
	struct ieee80211_hdr *hdr = (void *) frame->data;
	return (hdr->frame_control[0] & 0x0c) == 0;
}

bool is_multicast_ether_addr(const u8 *addr)
{
	return 0x01 & addr[0];
}

void queue_frame(struct wmediumd *ctx, struct station *station,
		 struct frame *frame)
{
	struct ieee80211_hdr *hdr = (void *) frame->data;
	u8 *dest = hdr->addr1;
	struct timespec now, target;
	struct wqueue *queue;
	int send_time;
	int cw;
	double error_prob;
	bool is_acked = false;
	bool noack = false;
	int i, j;
	int rate_idx;

	/* TODO configure phy parameters */
	int slot_time = 9;
	int sifs = 16;
	int difs = 2 * slot_time + sifs;

	int retries = 0;

	/* TODO lookup from somewhere */
	double snr = 15;

	clock_gettime(CLOCK_MONOTONIC, &now);

	int ack_time_usec = pkt_duration(14, index_to_rate[0]) + sifs;

	/*
	 * To determine a frame's expiration time, we compute the
	 * number of retries we might have to make due to radio conditions
	 * or contention, and add backoff time accordingly.  To that, we
	 * add the expiration time of the previous frame in the queue.
	 */
	queue = frame_is_mgmt(frame) ? &station->mgmt_queue : &station->data_queue;
	list_add_tail(&frame->list, &queue->frames);

	/* try to "send" this frame at each of the rates in the rateset */
	send_time = 0;
	cw = queue->cw_min;

	noack = frame_is_mgmt(frame) || is_multicast_ether_addr(dest);
	double choice = -3.14;

	for (i=0; i < IEEE80211_TX_MAX_RATES && !is_acked; i++) {

		rate_idx = frame->tx_rates[i].idx;

		/* no more rates in MRR */
		if (rate_idx < 0)
			break;

		error_prob = get_error_prob(snr, rate_idx, frame->data_len);
		for (j=0; j < frame->tx_rates[i].count; j++) {
			int rate = index_to_rate[rate_idx];
			send_time += difs + pkt_duration(frame->data_len, rate);

			retries++;

			/* skip ack/backoff/retries for noack frames */
			if (noack) {
				is_acked = true;
				break;
			}

			/* TODO TXOPs */

			/* backoff */
			if (j > 0) {
				send_time += (cw * slot_time) / 2;
				cw = (cw << 1) + 1;
				if (cw > queue->cw_max)
					cw = queue->cw_max;
			}
			choice = drand48();
			if (choice > error_prob) {
				is_acked = true;
				break;
			}
			send_time += ack_time_usec;
		}
	}

	if (is_acked) {
		frame->tx_rates[i].count = j + 1;
		for (++i; i < IEEE80211_TX_MAX_RATES; i++) {
			frame->tx_rates[i].idx = -1;
			frame->tx_rates[i].count = -1;
		}
		frame->flags |= HWSIM_TX_STAT_ACK;
	}

	target = now;
	timespec_add_usec(&target, send_time);

	printf("[" TIME_FMT "] queued for " TIME_FMT " len: %d retries: %d ack: %d rate: %d (%d) send_time usec %d %f %f\n", TIME_ARGS(&now), TIME_ARGS(&target), frame->data_len, retries, is_acked, index_to_rate[rate_idx], rate_idx, send_time, error_prob, choice);

	frame->expires = target;
	rearm_timer(ctx);
}

void deliver_frame(struct wmediumd *ctx, struct frame *frame)
{
	struct ieee80211_hdr *hdr = (void *) frame->data;
	struct station *station;
	u8 *dest = hdr->addr1;
	u8 *src = frame->sender->addr;

	int signal = 35;

	if (frame->flags & HWSIM_TX_STAT_ACK) {
		/* rx the frame on the dest interface */
		list_for_each_entry(station, &ctx->stations, list) {
			if (memcmp(src, station->addr, ETH_ALEN) == 0)
				continue;

			if (is_multicast_ether_addr(dest) ||
			    memcmp(dest, station->addr, ETH_ALEN) == 0) {
				send_cloned_frame_msg(ctx->sock, station->addr,
						      frame->data,
						      frame->data_len,
						      1, signal);
			}
		}
	}

	send_tx_info_frame_nl(ctx->sock, frame->sender, frame->flags, signal,
			      frame->tx_rates, frame->cookie);

	free(frame);
}

void deliver_expired_frames_queue(struct wmediumd *ctx,
				  struct list_head *queue,
				  struct timespec *now)
{
	struct frame *frame, *tmp;

	list_for_each_entry_safe(frame, tmp, queue, list) {
		if (timespec_before(&frame->expires, now)) {
			list_del(&frame->list);
			deliver_frame(ctx, frame);
		} else {
			break;
		}
	}
}

void deliver_expired_frames(struct wmediumd *ctx)
{
	struct timespec now;
	struct station *station;
	struct list_head *l;

	clock_gettime(CLOCK_MONOTONIC, &now);
	list_for_each_entry(station, &ctx->stations, list) {
		int data_count=0, mgmt_count = 0;
		list_for_each(l, &station->mgmt_queue.frames) {
			mgmt_count++;
		}
		list_for_each(l, &station->data_queue.frames) {
			data_count++;
		}
		printf("[" TIME_FMT "] Station " MAC_FMT " mgmt %d data %d\n",
		       TIME_ARGS(&now), MAC_ARGS(station->addr), mgmt_count, data_count);

		deliver_expired_frames_queue(ctx, &station->mgmt_queue.frames, &now);
		deliver_expired_frames_queue(ctx, &station->data_queue.frames, &now);
	}
	printf("\n\n");
}

/*
 *	Send a tx_info frame to the kernel space.
 */
int send_tx_info_frame_nl(struct nl_sock *sock,
			  u8 *src,
			  unsigned int flags, int signal,
			  struct hwsim_tx_rate *tx_attempts,
			  u64 cookie)
{

	msg = nlmsg_alloc();
	if (!msg) {
		printf("Error allocating new message MSG!\n");
		goto out;
	}

	genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, genl_family_get_id(family),
		    0, NLM_F_REQUEST, HWSIM_CMD_TX_INFO_FRAME, VERSION_NR);

	int rc;
	rc = nla_put(msg, HWSIM_ATTR_ADDR_TRANSMITTER, ETH_ALEN, src);
	rc = nla_put_u32(msg, HWSIM_ATTR_FLAGS, flags);
	rc = nla_put_u32(msg, HWSIM_ATTR_SIGNAL, signal);
	rc = nla_put(msg, HWSIM_ATTR_TX_INFO,
		     IEEE80211_TX_MAX_RATES * sizeof(struct hwsim_tx_rate),
		     tx_attempts);

	rc = nla_put_u64(msg, HWSIM_ATTR_COOKIE, cookie);

	if(rc!=0) {
		printf("Error filling payload\n");
		goto out;
	}

	nl_send_auto_complete(sock,msg);
	nlmsg_free(msg);
	return 0;
out:
	nlmsg_free(msg);
	return -1;
}

/*
 *	Send a cloned frame to the kernel space.
 */
int send_cloned_frame_msg(struct nl_sock *sock, u8 *dst,
			  char *data, int data_len, int rate_idx, int signal)
{

	msg = nlmsg_alloc();
	if (!msg) {
		printf("Error allocating new message MSG!\n");
		goto out;
	}

	genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, genl_family_get_id(family),
		    0, NLM_F_REQUEST, HWSIM_CMD_FRAME, VERSION_NR);

	int rc;

	rc = nla_put(msg, HWSIM_ATTR_ADDR_RECEIVER, ETH_ALEN, dst);
	rc = nla_put(msg, HWSIM_ATTR_FRAME, data_len, data);
	rc = nla_put_u32(msg, HWSIM_ATTR_RX_RATE, 1);
	rc = nla_put_u32(msg, HWSIM_ATTR_SIGNAL, -50);

	if(rc!=0) {
		printf("Error filling payload\n");
		goto out;
	}
	printf("cloned msg dest " MAC_FMT " len %d \n", MAC_ARGS(dst), data_len);

	nl_send_auto_complete(sock,msg);
	nlmsg_free(msg);
	return 0;
out:
	nlmsg_free(msg);
	return -1;
}

static
int nl_err_cb(struct sockaddr_nl *nla, struct nlmsgerr *nlerr, void *arg)
{
	struct genlmsghdr *gnlh = nlmsg_data(&nlerr->msg);

	fprintf(stderr, "nl: cmd %d, seq %d: %s\n", gnlh->cmd,
		nlerr->msg.nlmsg_seq, strerror(abs(nlerr->error)));

	return NL_SKIP;
}

static struct station *get_station_by_addr(struct wmediumd *ctx, u8 *addr)
{
	struct station *station;
	list_for_each_entry(station, &ctx->stations, list) {
		if (memcmp(station->addr, addr, ETH_ALEN) == 0)
			return station;
	}
	return NULL;
}


/*
 *	Callback function to process messages received from kernel
 */
static int process_messages_cb(struct nl_msg *msg, void *arg)
{
	struct wmediumd *ctx = arg;
	struct nl_sock *sock = ctx->sock;
	struct nlattr *attrs[HWSIM_ATTR_MAX+1];
	/* netlink header */
	struct nlmsghdr *nlh = nlmsg_hdr(msg);
	/* generic netlink header*/
	struct genlmsghdr *gnlh = nlmsg_data(nlh);

	struct station *sender;
	struct frame *frame;

	if(gnlh->cmd == HWSIM_CMD_FRAME) {
		/* we get the attributes*/
		genlmsg_parse(nlh, 0, attrs, HWSIM_ATTR_MAX, NULL);
		if (attrs[HWSIM_ATTR_ADDR_TRANSMITTER]) {
			u8 *src = (u8 *) nla_data(attrs[HWSIM_ATTR_ADDR_TRANSMITTER]);

			unsigned int data_len =
				nla_len(attrs[HWSIM_ATTR_FRAME]);
			char* data = (char*)nla_data(attrs[HWSIM_ATTR_FRAME]);
			unsigned int flags =
				nla_get_u32(attrs[HWSIM_ATTR_FLAGS]);
			unsigned int tx_rates_len =
				nla_len(attrs[HWSIM_ATTR_TX_INFO]);
			struct hwsim_tx_rate *tx_rates =
				(struct hwsim_tx_rate*)
				nla_data(attrs[HWSIM_ATTR_TX_INFO]);
			u64 cookie = nla_get_u64(attrs[HWSIM_ATTR_COOKIE]);

			received++;

			sender = get_station_by_addr(ctx, src);
			if (!sender) {
				fprintf(stderr, "Unable to find sender station\n");
				goto out;
			}

			frame = malloc(sizeof(*frame) + data_len);
			if (!frame)
				goto out;

			memcpy(frame->data, data, data_len);
			frame->data_len = data_len;
			frame->flags = flags;
			frame->cookie = cookie;
			frame->sender = sender;
			frame->tx_rates_count =
				tx_rates_len / sizeof(struct hwsim_tx_rate);
			memcpy(frame->tx_rates, tx_rates,
			       min(tx_rates_len, sizeof(frame->tx_rates)));
			queue_frame(ctx, sender, frame);
		}
	}
out:
	return 0;

out_free_frame:
	free(frame);
	return 0;
}

/*
 *	Send a register message to kernel
 */
int send_register_msg(struct nl_sock *sock)
{
	msg = nlmsg_alloc();
	if (!msg) {
		printf("Error allocating new message MSG!\n");
		return -1;
	}

	genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, genl_family_get_id(family),
		    0, NLM_F_REQUEST, HWSIM_CMD_REGISTER, VERSION_NR);
	nl_send_auto_complete(sock,msg);
	nlmsg_free(msg);

	return 0;
}

/*
 *	Signal handler
 */
void kill_handler() {
	running = 0;
}

static void sock_event_cb(int fd, short what, void *data)
{
	struct wmediumd *ctx = data;
	nl_recvmsgs_default(ctx->sock);
}

/*
 *	Init netlink
 */
void init_netlink(struct wmediumd *ctx)
{
	struct nl_sock *sock;

	cb = nl_cb_alloc(NL_CB_CUSTOM);
	if (!cb) {
		printf("Error allocating netlink callbacks\n");
		exit(EXIT_FAILURE);
	}

	sock = nl_socket_alloc_cb(cb);
	if (!sock) {
		printf("Error allocationg netlink socket\n");
		exit(EXIT_FAILURE);
	}

	ctx->sock = sock;

	genl_connect(sock);
	genl_ctrl_alloc_cache(sock, &cache);

	family = genl_ctrl_search_by_name(cache, "MAC80211_HWSIM");

	if (!family) {
		printf("Family MAC80211_HWSIM not registered\n");
		exit(EXIT_FAILURE);
	}

	nl_cb_set(cb, NL_CB_MSG_IN, NL_CB_CUSTOM, process_messages_cb, ctx);
	nl_cb_err(cb, NL_CB_CUSTOM, nl_err_cb, ctx);
}

/*
 *	Print the CLI help
 */
void print_help(int exval)
{
	printf("wmediumd v%s - a wireless medium simulator\n", VERSION_STR);
	printf("wmediumd [-h] [-V] [-c FILE] [-o FILE]\n\n");

	printf("  -h              print this help and exit\n");
	printf("  -V              print version and exit\n\n");

	printf("  -c FILE         set intput config file\n");
	printf("  -o FILE         set output config file\n\n");

	exit(exval);
}

static void timer_cb(int fd, short what, void *data)
{
	struct wmediumd *ctx = data;
	deliver_expired_frames(ctx);
	rearm_timer(ctx);
}

int main(int argc, char* argv[])
{
	int i;
	int opt, ifaces;
	int fd;
	struct nl_sock *sock;
	struct event ev_cmd;
	struct event ev_timer;
	struct wmediumd ctx;
	struct station *station;

	/* Set stdout buffering to line mode */
	setvbuf (stdout, NULL, _IOLBF, BUFSIZ);

	/* no arguments given */
	if(argc == 1) {
		fprintf(stderr, "This program needs arguments....\n\n");
		print_help(EXIT_FAILURE);
	}

	while((opt = getopt(argc, argv, "hVc:o:")) != -1) {
		switch(opt) {
		case 'h':
			print_help(EXIT_SUCCESS);
			break;
		case 'V':
			printf("wmediumd v%s - a wireless medium simulator "
			       "for mac80211_hwsim\n", VERSION_STR);
			exit(EXIT_SUCCESS);
			break;
		case 'c':
			printf("Input configuration file: %s\n", optarg);
			load_config(optarg);
			break;
		case 'o':
			printf("Output configuration file: %s\n", optarg);
			printf("How many interfaces are active?\n");
			scanf("%d",&ifaces);
			if (ifaces < 2) {
				printf("active interfaces must be at least 2\n");
				exit(EXIT_FAILURE);
			}
				write_config(optarg, ifaces, 0.0);
			break;
		case ':':
			printf("wmediumd: Error - Option `%c' "
			       "needs a value\n\n", optopt);
			print_help(EXIT_FAILURE);
			break;
		case '?':
			printf("wmediumd: Error - No such option:"
			       " `%c'\n\n", optopt);
			print_help(EXIT_FAILURE);
			break;
		}

	}

	if (optind < argc)
		print_help(EXIT_FAILURE);

	/* Handle kill signals */
	running = 1;
	signal(SIGUSR1, kill_handler);

	INIT_LIST_HEAD(&ctx.stations);
	for (i=0; i < size; i++) {
		station = malloc(sizeof(*station));
		if (!station) {
			fprintf(stderr, "Out of memory!");
			exit(1);
		}
		memcpy(station->addr, get_mac_address(i), ETH_ALEN);
		wqueue_init(&station->data_queue, 15, 1023);
		wqueue_init(&station->mgmt_queue, 3, 7);
		list_add_tail(&station->list, &ctx.stations);
	}

	/* init libevent */
	event_init();

	/* init netlink */
	init_netlink(&ctx);
	event_set(&ev_cmd, nl_socket_get_fd(ctx.sock), EV_READ | EV_PERSIST,
		  sock_event_cb, &ctx);
	event_add(&ev_cmd, NULL);

	/* setup timers */
	ctx.timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
	event_set(&ev_timer, ctx.timerfd, EV_READ | EV_PERSIST, timer_cb, &ctx);
	event_add(&ev_timer, NULL);

	/* Send a register msg to the kernel */
	if (send_register_msg(ctx.sock)==0)
		printf("REGISTER SENT!\n");

	/* enter libevent main loop */
	event_dispatch();

	/* Free all memory */
	free(ctx.sock);
	free(msg);
	free(cb);
	free(cache);
	free(family);

	return EXIT_SUCCESS;
}
