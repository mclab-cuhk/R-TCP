/*
 * R-TCP-BBRv1: BBRv1 with R-TCP
 *
 * This is based on the TCP BBRv1 implementation from Linux v5.4.0
 * (see below for TCP BBR's original License information).
 *
 * The R-TCP module was designed and implemented by
 * Shengtong Zhu, Yan Liu, Lingfeng Guo and Jack Y. B. Lee,
 *  "R-TCP: A Framework to Optimize TCP Performance Over Rate-Limiting Networks", NSDI, 2026.
 *
 * Contact  : zs021@ie.cuhk.edu.hk
 */

/* Bottleneck Bandwidth and RTT (BBR) congestion control
 *
 * BBR congestion control computes the sending rate based on the delivery
 * rate (throughput) estimated from ACKs. In a nutshell:
 *
 *   On each ACK, update our model of the network path:
 *      bottleneck_bandwidth = windowed_max(delivered / elapsed, 10 round trips)
 *      min_rtt = windowed_min(rtt, 10 seconds)
 *   pacing_rate = pacing_gain * bottleneck_bandwidth
 *   cwnd = max(cwnd_gain * bottleneck_bandwidth * min_rtt, 4)
 *
 * The core algorithm does not react directly to packet losses or delays,
 * although BBR may adjust the size of next send per ACK when loss is
 * observed, or adjust the sending rate if it estimates there is a
 * traffic policer, in order to keep the drop rate reasonable.
 *
 * Here is a state transition diagram for BBR:
 *
 *             |
 *             V
 *    +---> STARTUP  ----+
 *    |        |         |
 *    |        V         |
 *    |      DRAIN   ----+
 *    |        |         |
 *    |        V         |
 *    +---> PROBE_BW ----+
 *    |      ^    |      |
 *    |      |    |      |
 *    |      +----+      |
 *    |                  |
 *    +---- PROBE_RTT <--+
 *
 * A BBR flow starts in STARTUP, and ramps up its sending rate quickly.
 * When it estimates the pipe is full, it enters DRAIN to drain the queue.
 * In steady state a BBR flow only uses PROBE_BW and PROBE_RTT.
 * A long-lived BBR flow spends the vast majority of its time remaining
 * (repeatedly) in PROBE_BW, fully probing and utilizing the pipe's bandwidth
 * in a fair manner, with a small, bounded queue. *If* a flow has been
 * continuously sending for the entire min_rtt window, and hasn't seen an RTT
 * sample that matches or decreases its min_rtt estimate for 10 seconds, then
 * it briefly enters PROBE_RTT to cut inflight to a minimum value to re-probe
 * the path's two-way propagation delay (min_rtt). When exiting PROBE_RTT, if
 * we estimated that we reached the full bw of the pipe then we enter PROBE_BW;
 * otherwise we enter STARTUP to try to fill the pipe.
 *
 * BBR is described in detail in:
 *   "BBR: Congestion-Based Congestion Control",
 *   Neal Cardwell, Yuchung Cheng, C. Stephen Gunn, Soheil Hassas Yeganeh,
 *   Van Jacobson. ACM Queue, Vol. 14 No. 5, September-October 2016.
 *
 * There is a public e-mail list for discussing BBR development and testing:
 *   https://groups.google.com/forum/#!forum/bbr-dev
 *
 * NOTE: BBR might be used with the fq qdisc ("man tc-fq") with pacing enabled,
 * otherwise TCP stack falls back to an internal pacing using one high
 * resolution timer per TCP socket and may use more resources.
 */
#include <linux/module.h>
#include <net/tcp.h>
#include <linux/inet_diag.h>
#include <linux/inet.h>
#include <linux/random.h>
#include <linux/win_minmax.h>

/* Scale factor for rate in pkt/uSec unit to avoid truncation in bandwidth
 * estimation. The rate unit ~= (1500 bytes / 1 usec / 2^24) ~= 715 bps.
 * This handles bandwidths from 0.06pps (715bps) to 256Mpps (3Tbps) in a u32.
 * Since the minimum window is >=4 packets, the lower bound isn't
 * an issue. The upper bound isn't an issue with existing technologies.
 */
#define BW_SCALE 24
#define BW_UNIT (1 << BW_SCALE)

#define BBR_SCALE 8	/* scaling factor for fractions in BBR (e.g. gains) */
#define BBR_UNIT (1 << BBR_SCALE)

/* BBR has the following modes for deciding how fast to send: */
enum bbr_mode {
	BBR_STARTUP,	/* ramp up sending rate rapidly to fill pipe */
	BBR_DRAIN,	/* drain any queue created during startup */
	BBR_PROBE_BW,	/* discover, share bw: pace around estimated bw */
	BBR_PROBE_RTT,	/* cut inflight to min to probe min_rtt */
};

void nothing_to_do(char* a, ...) {}

#define printA nothing_to_do
#define MAX_STR_LEN 5000
#define STORE_INTERVAL 400

#define BASED_SCALE 8
#define BASED_UNIT (1 << BASED_SCALE)
// static const u8 percent_arr_num = 13;
// static const int percent_arr[] = {BW_UNIT,BW_UNIT*11/12,BW_UNIT*10/12,BW_UNIT*9/12,BW_UNIT*8/12,BW_UNIT*7/12,BW_UNIT*6/12,BW_UNIT*5/12,BW_UNIT*4/12,BW_UNIT*3/12,BW_UNIT*2/12,BW_UNIT*1/12,0};
static const u8 percent_arr_num = 9;
static const int percent_arr[] = {BW_UNIT,BW_UNIT*7/8,BW_UNIT*6/8,BW_UNIT*5/8,BW_UNIT*4/8,BW_UNIT*3/8,BW_UNIT*2/8,BW_UNIT*1/8,0};
/* If lost/delivered ratio > 20*/
static const u32 loss_thresh = 50;
/* If goodput diff / before empty > 40*/
static const u32 abrupt_decrease_thresh = 150;
static int probe_interval = 20;
static int probe_per = 24;
static int optimize_flag = 1;
static int high_loss_disclassify = 2;
static int monitor_peroid = 3;
static int use_goodput = 1;
static int exclude_RTO = 0;
static int exclude_rwnd = 0;
static int exclude_applimited = 0;
static int enable_printk = 1;

struct PMODRL {
	u64   B_arr[9];
	u64   R_arr[9];
	u8 best_index;
	u8 classify;
	u32 classify_time_us;
	u8 high_loss_flag;
	u32 loss_start_time_us;
	u32 before_loss_delivered;
	u32 before_loss_time_us;
	u32 before_loss_lost;
	u32 bbr_start_us;
	u64 bef_empty_goodput;
	u32 nominator;

	u32 latest_ack_us;
	u32 lastest_ack_loss;
	u64 detected_bytes_acked;
	u32 detected_time;

	u8 disable_flag;

	u64 mem_B;
	u64 mem_R;

	u8 probe_rtt_flag;

	u8 upper_bound;
	u32 round_count;
	u32 round_count_no;
	u32 next_rtt_delivered;
	u8 round_start;

	u32 transfer_start_deliverd;
	u32 transfer_start_lost;

	u8 reset_ltbw_flag;

	char* buffer;
	u32 store_interval;

	u64 acc_rto_dur;

	u64	cycle_mstamp;	     /* time of this cycle phase start */

	u64 dis_loss_start;
	u64 dis_deliver_start;
	u8 dis_enable_flag;
};


/* BBR congestion control block */
struct bbr {
	u32	min_rtt_us;	        /* min RTT in min_rtt_win_sec window */
	u32	min_rtt_stamp;	        /* timestamp of min_rtt_us */
	u32	probe_rtt_done_stamp;   /* end time for BBR_PROBE_RTT mode */
	struct minmax bw;	/* Max recent delivery rate in pkts/uS << 24 */
	u32	rtt_cnt;	    /* count of packet-timed rounds elapsed */
	u32     next_rtt_delivered; /* scb->tx.delivered at end of round */
	// u64	cycle_mstamp;	     /* time of this cycle phase start */
	u32     mode:3,		     /* current bbr_mode in state machine */
		prev_ca_state:3,     /* CA state on previous ACK */
		packet_conservation:1,  /* use packet conservation? */
		round_start:1,	     /* start of packet-timed tx->ack round? */
		idle_restart:1,	     /* restarting after idle? */
		probe_rtt_round_done:1,  /* a BBR_PROBE_RTT round at 4 pkts? */
		unused:13,
		lt_is_sampling:1,    /* taking long-term ("LT") samples now? */
		lt_rtt_cnt:7,	     /* round trips in long-term interval */
		lt_use_bw:1;	     /* use lt_bw as our bw estimate? */
	u32	lt_bw;		     /* LT est delivery rate in pkts/uS << 24 */
	u32	lt_last_delivered;   /* LT intvl start: tp->delivered */
	u32	lt_last_stamp;	     /* LT intvl start: tp->delivered_mstamp */
	u32	lt_last_lost;	     /* LT intvl start: tp->lost */
	u32	pacing_gain:10,	/* current gain for setting pacing rate */
		cwnd_gain:10,	/* current gain for setting cwnd */
		full_bw_reached:1,   /* reached full bw in Startup? */
		full_bw_cnt:2,	/* number of rounds without large bw gains */
		cycle_idx:3,	/* current index in pacing_gain cycle array */
		has_seen_rtt:1, /* have we seen an RTT sample yet? */
		unused_b:5;
	u32	prior_cwnd;	/* prior cwnd upon entering loss recovery */
	u32	full_bw;	/* recent bw, to estimate if pipe is full */

	/* For tracking ACK aggregation: */
	u64	ack_epoch_mstamp;	/* start of ACK sampling epoch */
	u16	extra_acked[2];		/* max excess data ACKed in epoch */
	u32	ack_epoch_acked:20,	/* packets (S)ACKed in sampling epoch */
		extra_acked_win_rtts:5,	/* age of extra_acked, in round trips */
		extra_acked_win_idx:1,	/* current index in extra_acked array */
		unused_c:6;

	struct PMODRL* pmodrl;
};

#define CYCLE_LEN	8	/* number of phases in a pacing gain cycle */

/* Window length of bw filter (in rounds): */
static const int bbr_bw_rtts = CYCLE_LEN + 2;
/* Window length of min_rtt filter (in sec): */
static const u32 bbr_min_rtt_win_sec = 10;
/* Minimum time (in ms) spent at bbr_cwnd_min_target in BBR_PROBE_RTT mode: */
static const u32 bbr_probe_rtt_mode_ms = 200;
/* Skip TSO below the following bandwidth (bits/sec): */
static const int bbr_min_tso_rate = 1200000;

/* Pace at ~1% below estimated bw, on average, to reduce queue at bottleneck.
 * In order to help drive the network toward lower queues and low latency while
 * maintaining high utilization, the average pacing rate aims to be slightly
 * lower than the estimated bandwidth. This is an important aspect of the
 * design.
 */
static const int bbr_pacing_margin_percent = 1;

/* We use a high_gain value of 2/ln(2) because it's the smallest pacing gain
 * that will allow a smoothly increasing pacing rate that will double each RTT
 * and send the same number of packets per RTT that an un-paced, slow-starting
 * Reno or CUBIC flow would:
 */
static const int bbr_high_gain  = BBR_UNIT * 2885 / 1000 + 1;
/* The pacing gain of 1/high_gain in BBR_DRAIN is calculated to typically drain
 * the queue created in BBR_STARTUP in a single round:
 */
static const int bbr_drain_gain = BBR_UNIT * 1000 / 2885;
/* The gain for deriving steady-state cwnd tolerates delayed/stretched ACKs: */
static const int bbr_cwnd_gain  = BBR_UNIT * 2;
/* The pacing_gain values for the PROBE_BW gain cycle, to discover/share bw: */
static const int bbr_pacing_gain[] = {
	BBR_UNIT * 5 / 4,	/* probe for more available bw */
	BBR_UNIT * 3 / 4,	/* drain queue and/or yield bw to other flows */
	BBR_UNIT, BBR_UNIT, BBR_UNIT,	/* cruise at 1.0*bw to utilize pipe, */
	BBR_UNIT, BBR_UNIT, BBR_UNIT	/* without creating excess queue... */
};
/* Randomize the starting gain cycling phase over N phases: */
static const u32 bbr_cycle_rand = 7;

/* Try to keep at least this many packets in flight, if things go smoothly. For
 * smooth functioning, a sliding window protocol ACKing every other packet
 * needs at least 4 packets in flight:
 */
static const u32 bbr_cwnd_min_target = 4;

/* To estimate if BBR_STARTUP mode (i.e. high_gain) has filled pipe... */
/* If bw has increased significantly (1.25x), there may be more bw available: */
static const u32 bbr_full_bw_thresh = BBR_UNIT * 5 / 4;
/* But after 3 rounds w/o significant bw growth, estimate pipe is full: */
static const u32 bbr_full_bw_cnt = 3;

/* "long-term" ("LT") bandwidth estimator parameters... */
/* The minimum number of rounds in an LT bw sampling interval: */
static const u32 bbr_lt_intvl_min_rtts = 4;
/* If lost/delivered ratio > 20%, interval is "lossy" and we may be policed: */
static const u32 bbr_lt_loss_thresh = 50;
/* If 2 intervals have a bw ratio <= 1/8, their bw is "consistent": */
static const u32 bbr_lt_bw_ratio = BBR_UNIT / 8;
/* If 2 intervals have a bw diff <= 4 Kbit/sec their bw is "consistent": */
static const u32 bbr_lt_bw_diff = 4000 / 8;
/* If we estimate we're policed, use lt_bw for this many round trips: */
static const u32 bbr_lt_bw_max_rtts = 48;

/* Gain factor for adding extra_acked to target cwnd: */
static const int bbr_extra_acked_gain = BBR_UNIT;
/* Window length of extra_acked window. */
static const u32 bbr_extra_acked_win_rtts = 5;
/* Max allowed val for ack_epoch_acked, after which sampling epoch is reset */
static const u32 bbr_ack_epoch_acked_reset_thresh = 1U << 20;
/* Time period for clamping cwnd increment due to ack aggregation */
static const u32 bbr_extra_acked_max_us = 100 * 1000;

static void bbr_check_probe_rtt_done(struct sock *sk);

/* Do we estimate that STARTUP filled the pipe? */
static bool bbr_full_bw_reached(const struct sock *sk)
{
	const struct bbr *bbr = inet_csk_ca(sk);

	return bbr->full_bw_reached;
}

/* Return the windowed max recent bandwidth sample, in pkts/uS << BW_SCALE. */
static u32 bbr_max_bw(const struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	return minmax_get(&bbr->bw);
}

/* Return the estimated bandwidth of the path, in pkts/uS << BW_SCALE. */
static u32 bbr_bw(const struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	return bbr->lt_use_bw ? bbr->lt_bw : bbr_max_bw(sk);
}

/* Return maximum extra acked in past k-2k round trips,
 * where k = bbr_extra_acked_win_rtts.
 */
static u16 bbr_extra_acked(const struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	return max(bbr->extra_acked[0], bbr->extra_acked[1]);
}

/* Return rate in bytes per second, optionally with a gain.
 * The order here is chosen carefully to avoid overflow of u64. This should
 * work for input rates of up to 2.9Tbit/sec and gain of 2.89x.
 */
static u64 bbr_rate_bytes_per_sec(struct sock *sk, u64 rate, int gain)
{
	unsigned int mss = tcp_sk(sk)->mss_cache;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - bbr_pacing_margin_percent);
	return rate >> BW_SCALE;
}

/* Convert a BBR bw and gain factor to a pacing rate in bytes per second. */
static unsigned long bbr_bw_to_pacing_rate(struct sock *sk, u32 bw, int gain)
{
	u64 rate = bw;

	rate = bbr_rate_bytes_per_sec(sk, rate, gain);
	rate = min_t(u64, rate, sk->sk_max_pacing_rate);
	return rate;
}

/* Initialize pacing rate to: high_gain * init_cwnd / RTT. */
static void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	u64 bw;
	u32 rtt_us;

	if (tp->srtt_us) {		/* any RTT sample yet? */
		rtt_us = max(tp->srtt_us >> 3, 1U);
		bbr->has_seen_rtt = 1;
	} else {			 /* no RTT sample yet */
		rtt_us = USEC_PER_MSEC;	 /* use nominal default RTT */
	}
	bw = (u64)tp->snd_cwnd * BW_UNIT;
	do_div(bw, rtt_us);
	sk->sk_pacing_rate = bbr_bw_to_pacing_rate(sk, bw, bbr_high_gain);
}


static unsigned long bbr_bw_to_pacing_rate_pmodrl(struct sock *sk, u32 bw, int gain, int nominator)
{
	// struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	u64 rate = bw;

	if(bbr->pmodrl && bbr->pmodrl->classify == 1 && nominator != 0){
		gain = gain * probe_per / 20;
	}
	rate = bbr_rate_bytes_per_sec(sk, rate, gain);
	rate = min_t(u64, rate, sk->sk_max_pacing_rate);
	return rate;
}

/* Pace using current bw estimate and a gain factor. */
static void bbr_set_pacing_rate(struct sock *sk, u32 bw, int gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	unsigned long rate = bbr_bw_to_pacing_rate(sk, bw, gain);

	u8 flag = 0;
	if(bbr->pmodrl && bbr->pmodrl->classify == 1 && bbr->pmodrl->upper_bound == 1){
		unsigned long pmodrl_rate = bbr_bw_to_pacing_rate_pmodrl(sk, bbr->pmodrl->R_arr[bbr->pmodrl->best_index], BBR_UNIT, bbr->pmodrl->nominator);
		// printA(KERN_INFO "!!! rate:%llu  pmodrl_rate:%llu\n",rate, pmodrl_rate);
		if(rate > pmodrl_rate && optimize_flag){
			rate = pmodrl_rate;
			flag = 1;
		}
	}

	if (unlikely(!bbr->has_seen_rtt && tp->srtt_us))
		bbr_init_pacing_rate_from_rtt(sk);
	if (bbr_full_bw_reached(sk) || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
	if(bbr->pmodrl && bbr->pmodrl->classify == 1 && flag && optimize_flag){
		sk->sk_pacing_rate = rate;
	}
}

/* override sysctl_tcp_min_tso_segs */
static u32 bbr_min_tso_segs(struct sock *sk)
{
	return sk->sk_pacing_rate < (bbr_min_tso_rate >> 3) ? 1 : 2;
}


// /* Return the number of segments BBR would like in a TSO/GSO skb, given
//  * a particular max gso size as a constraint.
//  */
// static u32 bbr_tso_segs_generic(struct sock *sk, unsigned int mss_now,
// 						u32 gso_max_size)
// {
// 	u32 segs;
// 	u64 bytes;

// 	/* Budget a TSO/GSO burst size allowance based on bw (pacing_rate). */
// 	bytes = sk->sk_pacing_rate >> sk->sk_pacing_shift;

// 	bytes = min_t(u32, bytes, gso_max_size - 1 - MAX_TCP_HEADER);
// 		segs = max_t(u32, bytes / mss_now, bbr_min_tso_segs(sk));
// 	return segs;
// }

// /* Custom tcp_tso_autosize() for BBR, used at transmit time to cap skb size. */
// static u32  bbr_tso_segs(struct sock *sk, unsigned int mss_now)
// {
// 	return bbr_tso_segs_generic(sk, mss_now, sk->sk_gso_max_size);
// }

static u32 bbr_tso_segs_goal(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 segs, bytes;

	/* Sort of tcp_tso_autosize() but ignoring
	 * driver provided sk_gso_max_size.
	 */
	bytes = min_t(unsigned long, sk->sk_pacing_rate >> sk->sk_pacing_shift,
		      GSO_MAX_SIZE - 1 - MAX_TCP_HEADER);
	segs = max_t(u32, bytes / tp->mss_cache, bbr_min_tso_segs(sk));

	return min(segs, 0x7FU);
}

/* Save "last known good" cwnd so we can restore it after losses or PROBE_RTT */
static void bbr_save_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);

	if (bbr->prev_ca_state < TCP_CA_Recovery && bbr->mode != BBR_PROBE_RTT)
		bbr->prior_cwnd = tp->snd_cwnd;  /* this cwnd is good enough */
	else  /* loss recovery or BBR_PROBE_RTT have temporarily cut cwnd */
		bbr->prior_cwnd = max(bbr->prior_cwnd, tp->snd_cwnd);
}

static void bbr_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	u32 now_us = jiffies_to_usecs(tcp_jiffies32);

	if (event == CA_EVENT_TX_START && tp->app_limited) {
		bbr->idle_restart = 1;
		bbr->ack_epoch_mstamp = tp->tcp_mstamp;
		bbr->ack_epoch_acked = 0;
		/* Avoid pointless buffer overflows: pace at est. bw if we don't
		 * need more speed (we're restarting from idle and app-limited).
		 */
		if (bbr->mode == BBR_PROBE_BW)
			bbr_set_pacing_rate(sk, bbr_bw(sk), BBR_UNIT);
		else if (bbr->mode == BBR_PROBE_RTT)
			bbr_check_probe_rtt_done(sk);

		// if(bbr->pmodrl){
		// 	if(bbr->pmodrl->probe_rtt_flag == 1){
		// 		bbr->pmodrl->probe_rtt_flag = 0;
		// 		return;
		// 	}
		// 	else{
		// 		char* p;
		// 		u64 cycle_mstamp = bbr->pmodrl->cycle_mstamp;
		// 		p = bbr->pmodrl->buffer;
		// 		now_us = jiffies_to_usecs(tcp_jiffies32);
		// 		srtt = tp->srtt_us >> 3;
		// 		idle_us = now_us - bbr->pmodrl->latest_ack_us;
		// 		memset(bbr->pmodrl,0, sizeof(struct PMODRL));
		// 		if(bbr->pmodrl->buffer){
		// 			memset(bbr->pmodrl->buffer, 0, MAX_STR_LEN);
		// 		}
		// 		bbr->pmodrl->bbr_start_us = now_us;
		// 		bbr->pmodrl->transfer_start_lost = tp->lost;
		// 		bbr->pmodrl->transfer_start_deliverd = tp->delivered;
		// 		bbr->pmodrl->buffer = p;
		// 		bbr->pmodrl->cycle_mstamp = cycle_mstamp;
		// 	}
		// }
		bbr->pmodrl->bbr_start_us = now_us;
		bbr->pmodrl->transfer_start_lost = tp->lost;
		bbr->pmodrl->transfer_start_deliverd = tp->delivered;
		if(use_goodput){
			bbr->pmodrl->transfer_start_deliverd = tp->snd_una / tp->mss_cache;
		}
	}
}

/* Calculate bdp based on min RTT and the estimated bottleneck bandwidth:
 *
 * bdp = ceil(bw * min_rtt * gain)
 *
 * The key factor, gain, controls the amount of queue. While a small gain
 * builds a smaller queue, it becomes more vulnerable to noise in RTT
 * measurements (e.g., delayed ACKs or other ACK compression effects). This
 * noise may cause BBR to under-estimate the rate.
 */
static u32 bbr_bdp(struct sock *sk, u32 bw, int gain)
{
	struct bbr *bbr = inet_csk_ca(sk);
	u32 bdp;
	u64 w;

	/* If we've never had a valid RTT sample, cap cwnd at the initial
	 * default. This should only happen when the connection is not using TCP
	 * timestamps and has retransmitted all of the SYN/SYNACK/data packets
	 * ACKed so far. In this case, an RTO can cut cwnd to 1, in which
	 * case we need to slow-start up toward something safe: TCP_INIT_CWND.
	 */
	if (unlikely(bbr->min_rtt_us == ~0U))	 /* no valid RTT samples yet? */
		return TCP_INIT_CWND;  /* be safe: cap at default initial cwnd*/

	w = (u64)bw * bbr->min_rtt_us;

	/* Apply a gain to the given value, remove the BW_SCALE shift, and
	 * round the value up to avoid a negative feedback loop.
	 */
	bdp = (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;

	return bdp;
}

/* To achieve full performance in high-speed paths, we budget enough cwnd to
 * fit full-sized skbs in-flight on both end hosts to fully utilize the path:
 *   - one skb in sending host Qdisc,
 *   - one skb in sending host TSO/GSO engine
 *   - one skb being received by receiver host LRO/GRO/delayed-ACK engine
 * Don't worry, at low rates (bbr_min_tso_rate) this won't bloat cwnd because
 * in such cases tso_segs_goal is 1. The minimum cwnd is 4 packets,
 * which allows 2 outstanding 2-packet sequences, to try to keep pipe
 * full even with ACK-every-other-packet delayed ACKs.
 */
static u32 bbr_quantization_budget(struct sock *sk, u32 cwnd)
{
	struct bbr *bbr = inet_csk_ca(sk);

	/* Allow enough full-sized skbs in flight to utilize end systems. */
	cwnd += 3 * bbr_tso_segs_goal(sk);

	/* Reduce delayed ACKs by rounding up cwnd to the next even number. */
	cwnd = (cwnd + 1) & ~1U;

	/* Ensure gain cycling gets inflight above BDP even for small BDPs. */
	if (bbr->mode == BBR_PROBE_BW && bbr->cycle_idx == 0)
		cwnd += 2;

	return cwnd;
}

/* Find inflight based on min RTT and the estimated bottleneck bandwidth. */
static u32 bbr_inflight(struct sock *sk, u32 bw, int gain)
{
	u32 inflight;

	inflight = bbr_bdp(sk, bw, gain);
	inflight = bbr_quantization_budget(sk, inflight);

	return inflight;
}

/* With pacing at lower layers, there's often less data "in the network" than
 * "in flight". With TSQ and departure time pacing at lower layers (e.g. fq),
 * we often have several skbs queued in the pacing layer with a pre-scheduled
 * earliest departure time (EDT). BBR adapts its pacing rate based on the
 * inflight level that it estimates has already been "baked in" by previous
 * departure time decisions. We calculate a rough estimate of the number of our
 * packets that might be in the network at the earliest departure time for the
 * next skb scheduled:
 *   in_network_at_edt = inflight_at_edt - (EDT - now) * bw
 * If we're increasing inflight, then we want to know if the transmit of the
 * EDT skb will push inflight above the target, so inflight_at_edt includes
 * bbr_tso_segs_goal() from the skb departing at EDT. If decreasing inflight,
 * then estimate if inflight will sink too low just before the EDT transmit.
 */
static u32 bbr_packets_in_net_at_edt(struct sock *sk, u32 inflight_now)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	u64 now_ns, edt_ns, interval_us;
	u32 interval_delivered, inflight_at_edt;

	now_ns = tp->tcp_clock_cache;
	edt_ns = max(tp->tcp_wstamp_ns, now_ns);
	interval_us = div_u64(edt_ns - now_ns, NSEC_PER_USEC);
	interval_delivered = (u64)bbr_bw(sk) * interval_us >> BW_SCALE;
	inflight_at_edt = inflight_now;
	if (bbr->pacing_gain > BBR_UNIT)              /* increasing inflight */
		inflight_at_edt += bbr_tso_segs_goal(sk);  /* include EDT skb */
	if (interval_delivered >= inflight_at_edt)
		return 0;
	return inflight_at_edt - interval_delivered;
}

/* Find the cwnd increment based on estimate of ack aggregation */
static u32 bbr_ack_aggregation_cwnd(struct sock *sk)
{
	u32 max_aggr_cwnd, aggr_cwnd = 0;

	if (bbr_extra_acked_gain && bbr_full_bw_reached(sk)) {
		max_aggr_cwnd = ((u64)bbr_bw(sk) * bbr_extra_acked_max_us)
				/ BW_UNIT;
		aggr_cwnd = (bbr_extra_acked_gain * bbr_extra_acked(sk))
			     >> BBR_SCALE;
		aggr_cwnd = min(aggr_cwnd, max_aggr_cwnd);
	}

	return aggr_cwnd;
}

/* An optimization in BBR to reduce losses: On the first round of recovery, we
 * follow the packet conservation principle: send P packets per P packets acked.
 * After that, we slow-start and send at most 2*P packets per P packets acked.
 * After recovery finishes, or upon undo, we restore the cwnd we had when
 * recovery started (capped by the target cwnd based on estimated BDP).
 *
 * TODO(ycheng/ncardwell): implement a rate-based approach.
 */
static bool bbr_set_cwnd_to_recover_or_restore(
	struct sock *sk, const struct rate_sample *rs, u32 acked, u32 *new_cwnd)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	u8 prev_state = bbr->prev_ca_state, state = inet_csk(sk)->icsk_ca_state;
	u32 cwnd = tp->snd_cwnd;

	// if(bbr->pmodrl && bbr->pmodrl->classify){
	// 	return false;
	// }

	/* An ACK for P pkts should release at most 2*P packets. We do this
	 * in two steps. First, here we deduct the number of lost packets.
	 * Then, in bbr_set_cwnd() we slow start up toward the target cwnd.
	 */
	if (rs->losses > 0)
		cwnd = max_t(s32, cwnd - rs->losses, 1);

	if (state == TCP_CA_Recovery && prev_state != TCP_CA_Recovery) {
		/* Starting 1st round of Recovery, so do packet conservation. */
		bbr->packet_conservation = 1;
		bbr->next_rtt_delivered = tp->delivered;  /* start round now */
		if(bbr->pmodrl){
			bbr->pmodrl->next_rtt_delivered = tp->delivered;
		}
		/* Cut unused cwnd from app behavior, TSQ, or TSO deferral: */
		cwnd = tcp_packets_in_flight(tp) + acked;
	} else if (prev_state >= TCP_CA_Recovery && state < TCP_CA_Recovery) {
		/* Exiting loss recovery; restore cwnd saved before recovery. */
		cwnd = max(cwnd, bbr->prior_cwnd);
		bbr->packet_conservation = 0;
	}
	bbr->prev_ca_state = state;

	if (bbr->packet_conservation) {
		*new_cwnd = max(cwnd, tcp_packets_in_flight(tp) + acked);
		return true;	/* yes, using packet conservation */
	}
	*new_cwnd = cwnd;
	return false;
}

/* Slow-start up toward target cwnd (if bw estimate is growing, or packet loss
 * has drawn us down below target), or snap down to target if we're above it.
 */
static void bbr_set_cwnd(struct sock *sk, const struct rate_sample *rs,
			 u32 acked, u32 bw, int gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	u32 cwnd = tp->snd_cwnd, target_cwnd = 0;

	if (!acked)
		goto done;  /* no packet fully ACKed; just apply caps */

	if (bbr_set_cwnd_to_recover_or_restore(sk, rs, acked, &cwnd))
		goto done;

	target_cwnd = bbr_bdp(sk, bw, gain);

	/* Increment the cwnd to account for excess ACKed data that seems
	 * due to aggregation (of data and/or ACKs) visible in the ACK stream.
	 */
	target_cwnd += bbr_ack_aggregation_cwnd(sk);
	target_cwnd = bbr_quantization_budget(sk, target_cwnd);

	/* If we're below target cwnd, slow start cwnd toward target cwnd. */
	if (bbr_full_bw_reached(sk))  /* only cut cwnd if we filled the pipe */
		cwnd = min(cwnd + acked, target_cwnd);
	else if (cwnd < target_cwnd || tp->delivered < TCP_INIT_CWND)
		cwnd = cwnd + acked;
	cwnd = max(cwnd, bbr_cwnd_min_target);

done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);	/* apply global cap */
	if (bbr->mode == BBR_PROBE_RTT)  /* drain queue, refresh min_rtt */
		tp->snd_cwnd = min(tp->snd_cwnd, bbr_cwnd_min_target);
}

/* End cycle phase if it's time and/or we hit the phase's in-flight target. */
static bool bbr_is_next_cycle_phase(struct sock *sk,
				    const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	bool is_full_length =
		tcp_stamp_us_delta(tp->delivered_mstamp, bbr->pmodrl->cycle_mstamp) >
		bbr->min_rtt_us;
	u32 inflight, bw;

	/* The pacing_gain of 1.0 paces at the estimated bw to try to fully
	 * use the pipe without increasing the queue.
	 */
	if (bbr->pacing_gain == BBR_UNIT)
		return is_full_length;		/* just use wall clock time */

	inflight = bbr_packets_in_net_at_edt(sk, rs->prior_in_flight);
	bw = bbr_max_bw(sk);

	/* A pacing_gain > 1.0 probes for bw by trying to raise inflight to at
	 * least pacing_gain*BDP; this may take more than min_rtt if min_rtt is
	 * small (e.g. on a LAN). We do not persist if packets are lost, since
	 * a path with small buffers may not hold that much.
	 */
	if (bbr->pacing_gain > BBR_UNIT)
		return is_full_length &&
			(rs->losses ||  /* perhaps pacing_gain*BDP won't fit */
			 inflight >= bbr_inflight(sk, bw, bbr->pacing_gain));

	/* A pacing_gain < 1.0 tries to drain extra queue we added if bw
	 * probing didn't find more bw. If inflight falls to match BDP then we
	 * estimate queue is drained; persisting would underutilize the pipe.
	 */
	return is_full_length ||
		inflight <= bbr_inflight(sk, bw, BBR_UNIT);
}

static void bbr_advance_cycle_phase(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);

	bbr->cycle_idx = (bbr->cycle_idx + 1) & (CYCLE_LEN - 1);
	bbr->pmodrl->cycle_mstamp = tp->delivered_mstamp;
}

/* Gain cycling: cycle pacing gain to converge to fair share of available bw. */
static void bbr_update_cycle_phase(struct sock *sk,
				   const struct rate_sample *rs)
{
	struct bbr *bbr = inet_csk_ca(sk);

	if (bbr->mode == BBR_PROBE_BW && bbr_is_next_cycle_phase(sk, rs))
		bbr_advance_cycle_phase(sk);
}

static void bbr_reset_startup_mode(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	bbr->mode = BBR_STARTUP;
}

static void bbr_reset_probe_bw_mode(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	bbr->mode = BBR_PROBE_BW;
	bbr->cycle_idx = CYCLE_LEN - 1 - prandom_u32_max(bbr_cycle_rand);
	bbr_advance_cycle_phase(sk);	/* flip to next phase of gain cycle */
}

static void bbr_reset_mode(struct sock *sk)
{
	if (!bbr_full_bw_reached(sk))
		bbr_reset_startup_mode(sk);
	else
		bbr_reset_probe_bw_mode(sk);
}

/* Start a new long-term sampling interval. */
static void bbr_reset_lt_bw_sampling_interval(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);

	bbr->lt_last_stamp = div_u64(tp->delivered_mstamp, USEC_PER_MSEC);
	bbr->lt_last_delivered = tp->delivered;
	bbr->lt_last_lost = tp->lost;
	bbr->lt_rtt_cnt = 0;
}

/* Completely reset long-term bandwidth sampling. */
static void bbr_reset_lt_bw_sampling(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	bbr->lt_bw = 0;
	bbr->lt_use_bw = 0;
	bbr->lt_is_sampling = false;
	bbr_reset_lt_bw_sampling_interval(sk);
}

/* Long-term bw sampling interval is done. Estimate whether we're policed. */
static void bbr_lt_bw_interval_done(struct sock *sk, u32 bw)
{
	struct bbr *bbr = inet_csk_ca(sk);
	u32 diff;

	if (bbr->lt_bw) {  /* do we have bw from a previous interval? */
		/* Is new bw close to the lt_bw from the previous interval? */
		diff = abs(bw - bbr->lt_bw);
		if ((diff * BBR_UNIT <= bbr_lt_bw_ratio * bbr->lt_bw) ||
		    (bbr_rate_bytes_per_sec(sk, diff, BBR_UNIT) <=
		     bbr_lt_bw_diff)) {
			/* All criteria are met; estimate we're policed. */
			bbr->lt_bw = (bw + bbr->lt_bw) >> 1;  /* avg 2 intvls */
			bbr->lt_use_bw = 1;
			bbr->pacing_gain = BBR_UNIT;  /* try to avoid drops */
			bbr->lt_rtt_cnt = 0;
			return;
		}
	}
	bbr->lt_bw = bw;
	bbr_reset_lt_bw_sampling_interval(sk);
}

/* Token-bucket traffic policers are common (see "An Internet-Wide Analysis of
 * Traffic Policing", SIGCOMM 2016). BBR detects token-bucket policers and
 * explicitly models their policed rate, to reduce unnecessary losses. We
 * estimate that we're policed if we see 2 consecutive sampling intervals with
 * consistent throughput and high packet loss. If we think we're being policed,
 * set lt_bw to the "long-term" average delivery rate from those 2 intervals.
 */
static void bbr_lt_bw_sampling(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	u32 lost, delivered;
	u64 bw;
	u32 t;

	if (bbr->lt_use_bw) {	/* already using long-term rate, lt_bw? */
		if (bbr->mode == BBR_PROBE_BW && bbr->round_start &&
		    ++bbr->lt_rtt_cnt >= bbr_lt_bw_max_rtts) {
			bbr_reset_lt_bw_sampling(sk);    /* stop using lt_bw */
			bbr_reset_probe_bw_mode(sk);  /* restart gain cycling */
		}
		return;
	}

	/* Wait for the first loss before sampling, to let the policer exhaust
	 * its tokens and estimate the steady-state rate allowed by the policer.
	 * Starting samples earlier includes bursts that over-estimate the bw.
	 */
	if (!bbr->lt_is_sampling) {
		if (!rs->losses)
			return;
		bbr_reset_lt_bw_sampling_interval(sk);
		bbr->lt_is_sampling = true;
	}

	/* To avoid underestimates, reset sampling if we run out of data. */
	if (rs->is_app_limited) {
		bbr_reset_lt_bw_sampling(sk);
		return;
	}

	if (bbr->round_start)
		bbr->lt_rtt_cnt++;	/* count round trips in this interval */
	if (bbr->lt_rtt_cnt < bbr_lt_intvl_min_rtts)
		return;		/* sampling interval needs to be longer */
	if (bbr->lt_rtt_cnt > 4 * bbr_lt_intvl_min_rtts) {
		bbr_reset_lt_bw_sampling(sk);  /* interval is too long */
		return;
	}

	/* End sampling interval when a packet is lost, so we estimate the
	 * policer tokens were exhausted. Stopping the sampling before the
	 * tokens are exhausted under-estimates the policed rate.
	 */
	if (!rs->losses)
		return;

	/* Calculate packets lost and delivered in sampling interval. */
	lost = tp->lost - bbr->lt_last_lost;
	delivered = tp->delivered - bbr->lt_last_delivered;
	/* Is loss rate (lost/delivered) >= lt_loss_thresh? If not, wait. */
	if (!delivered || (lost << BBR_SCALE) < bbr_lt_loss_thresh * delivered)
		return;

	/* Find average delivery rate in this sampling interval. */
	t = div_u64(tp->delivered_mstamp, USEC_PER_MSEC) - bbr->lt_last_stamp;
	if ((s32)t < 1)
		return;		/* interval is less than one ms, so wait */
	/* Check if can multiply without overflow */
	if (t >= ~0U / USEC_PER_MSEC) {
		bbr_reset_lt_bw_sampling(sk);  /* interval too long; reset */
		return;
	}
	t *= USEC_PER_MSEC;
	bw = (u64)delivered * BW_UNIT;
	do_div(bw, t);
	bbr_lt_bw_interval_done(sk, bw);
}

/* Estimate the bandwidth based on how fast packets are delivered */
static void bbr_update_bw(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	u64 bw;

	bbr->round_start = 0;
	if (rs->delivered < 0 || rs->interval_us <= 0)
		return; /* Not a valid observation */

	/* See if we've reached the next RTT */
	if (!before(rs->prior_delivered, bbr->next_rtt_delivered)) {
		bbr->next_rtt_delivered = tp->delivered;
		bbr->rtt_cnt++;
		bbr->round_start = 1;
		bbr->packet_conservation = 0;
	}

	bbr_lt_bw_sampling(sk, rs);

	/* Divide delivered by the interval to find a (lower bound) bottleneck
	 * bandwidth sample. Delivered is in packets and interval_us in uS and
	 * ratio will be <<1 for most connections. So delivered is first scaled.
	 */
	bw = (u64)rs->delivered * BW_UNIT;
	do_div(bw, rs->interval_us);

	/* If this sample is application-limited, it is likely to have a very
	 * low delivered count that represents application behavior rather than
	 * the available network rate. Such a sample could drag down estimated
	 * bw, causing needless slow-down. Thus, to continue to send at the
	 * last measured network rate, we filter out app-limited samples unless
	 * they describe the path bw at least as well as our bw model.
	 *
	 * So the goal during app-limited phase is to proceed with the best
	 * network rate no matter how long. We automatically leave this
	 * phase when app writes faster than the network can deliver :)
	 */
	if (!rs->is_app_limited || bw >= bbr_max_bw(sk)) {
		/* Incorporate new sample into our max bw filter. */
		minmax_running_max(&bbr->bw, bbr_bw_rtts, bbr->rtt_cnt, bw);
	}
}

/* Estimates the windowed max degree of ack aggregation.
 * This is used to provision extra in-flight data to keep sending during
 * inter-ACK silences.
 *
 * Degree of ack aggregation is estimated as extra data acked beyond expected.
 *
 * max_extra_acked = "maximum recent excess data ACKed beyond max_bw * interval"
 * cwnd += max_extra_acked
 *
 * Max extra_acked is clamped by cwnd and bw * bbr_extra_acked_max_us (100 ms).
 * Max filter is an approximate sliding window of 5-10 (packet timed) round
 * trips.
 */
static void bbr_update_ack_aggregation(struct sock *sk,
				       const struct rate_sample *rs)
{
	u32 epoch_us, expected_acked, extra_acked;
	struct bbr *bbr = inet_csk_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);

	if (!bbr_extra_acked_gain || rs->acked_sacked <= 0 ||
	    rs->delivered < 0 || rs->interval_us <= 0)
		return;

	if (bbr->round_start) {
		bbr->extra_acked_win_rtts = min(0x1F,
						bbr->extra_acked_win_rtts + 1);
		if (bbr->extra_acked_win_rtts >= bbr_extra_acked_win_rtts) {
			bbr->extra_acked_win_rtts = 0;
			bbr->extra_acked_win_idx = bbr->extra_acked_win_idx ?
						   0 : 1;
			bbr->extra_acked[bbr->extra_acked_win_idx] = 0;
		}
	}

	/* Compute how many packets we expected to be delivered over epoch. */
	epoch_us = tcp_stamp_us_delta(tp->delivered_mstamp,
				      bbr->ack_epoch_mstamp);
	expected_acked = ((u64)bbr_bw(sk) * epoch_us) / BW_UNIT;

	/* Reset the aggregation epoch if ACK rate is below expected rate or
	 * significantly large no. of ack received since epoch (potentially
	 * quite old epoch).
	 */
	if (bbr->ack_epoch_acked <= expected_acked ||
	    (bbr->ack_epoch_acked + rs->acked_sacked >=
	     bbr_ack_epoch_acked_reset_thresh)) {
		bbr->ack_epoch_acked = 0;
		bbr->ack_epoch_mstamp = tp->delivered_mstamp;
		expected_acked = 0;
	}

	/* Compute excess data delivered, beyond what was expected. */
	bbr->ack_epoch_acked = min_t(u32, 0xFFFFF,
				     bbr->ack_epoch_acked + rs->acked_sacked);
	extra_acked = bbr->ack_epoch_acked - expected_acked;
	extra_acked = min(extra_acked, tp->snd_cwnd);
	if (extra_acked > bbr->extra_acked[bbr->extra_acked_win_idx])
		bbr->extra_acked[bbr->extra_acked_win_idx] = extra_acked;
}

/* Estimate when the pipe is full, using the change in delivery rate: BBR
 * estimates that STARTUP filled the pipe if the estimated bw hasn't changed by
 * at least bbr_full_bw_thresh (25%) after bbr_full_bw_cnt (3) non-app-limited
 * rounds. Why 3 rounds: 1: rwin autotuning grows the rwin, 2: we fill the
 * higher rwin, 3: we get higher delivery rate samples. Or transient
 * cross-traffic or radio noise can go away. CUBIC Hystart shares a similar
 * design goal, but uses delay and inter-ACK spacing instead of bandwidth.
 */
static void bbr_check_full_bw_reached(struct sock *sk,
				      const struct rate_sample *rs)
{
	struct bbr *bbr = inet_csk_ca(sk);
	u32 bw_thresh;

	if (bbr_full_bw_reached(sk) || !bbr->round_start || rs->is_app_limited)
		return;

	bw_thresh = (u64)bbr->full_bw * bbr_full_bw_thresh >> BBR_SCALE;
	if (bbr_max_bw(sk) >= bw_thresh) {
		bbr->full_bw = bbr_max_bw(sk);
		bbr->full_bw_cnt = 0;
		return;
	}
	++bbr->full_bw_cnt;
	bbr->full_bw_reached = bbr->full_bw_cnt >= bbr_full_bw_cnt;
}

/* If pipe is probably full, drain the queue and then enter steady-state. */
static void bbr_check_drain(struct sock *sk, const struct rate_sample *rs)
{
	struct bbr *bbr = inet_csk_ca(sk);

	if (bbr->mode == BBR_STARTUP && bbr_full_bw_reached(sk)) {
		bbr->mode = BBR_DRAIN;	/* drain queue we created */
		tcp_sk(sk)->snd_ssthresh =
				bbr_inflight(sk, bbr_max_bw(sk), BBR_UNIT);
	}	/* fall through to check if in-flight is already small: */
	if (bbr->mode == BBR_DRAIN &&
	    bbr_packets_in_net_at_edt(sk, tcp_packets_in_flight(tcp_sk(sk))) <=
	    bbr_inflight(sk, bbr_max_bw(sk), BBR_UNIT))
		bbr_reset_probe_bw_mode(sk);  /* we estimate queue is drained */
}

static void bbr_check_probe_rtt_done(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);

	if (!(bbr->probe_rtt_done_stamp &&
	      after(tcp_jiffies32, bbr->probe_rtt_done_stamp)))
		return;

	bbr->min_rtt_stamp = tcp_jiffies32;  /* wait a while until PROBE_RTT */
	tp->snd_cwnd = max(tp->snd_cwnd, bbr->prior_cwnd);
	bbr_reset_mode(sk);
}

/* The goal of PROBE_RTT mode is to have BBR flows cooperatively and
 * periodically drain the bottleneck queue, to converge to measure the true
 * min_rtt (unloaded propagation delay). This allows the flows to keep queues
 * small (reducing queuing delay and packet loss) and achieve fairness among
 * BBR flows.
 *
 * The min_rtt filter window is 10 seconds. When the min_rtt estimate expires,
 * we enter PROBE_RTT mode and cap the cwnd at bbr_cwnd_min_target=4 packets.
 * After at least bbr_probe_rtt_mode_ms=200ms and at least one packet-timed
 * round trip elapsed with that flight size <= 4, we leave PROBE_RTT mode and
 * re-enter the previous mode. BBR uses 200ms to approximately bound the
 * performance penalty of PROBE_RTT's cwnd capping to roughly 2% (200ms/10s).
 *
 * Note that flows need only pay 2% if they are busy sending over the last 10
 * seconds. Interactive applications (e.g., Web, RPCs, video chunks) often have
 * natural silences or low-rate periods within 10 seconds where the rate is low
 * enough for long enough to drain its queue in the bottleneck. We pick up
 * these min RTT measurements opportunistically with our min_rtt filter. :-)
 */
static void bbr_update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	bool filter_expired;

	/* Track min RTT seen in the min_rtt_win_sec filter window: */
	filter_expired = after(tcp_jiffies32,
			       bbr->min_rtt_stamp + bbr_min_rtt_win_sec * HZ);
	if (rs->rtt_us >= 0 &&
	    (rs->rtt_us <= bbr->min_rtt_us ||
	     (filter_expired && !rs->is_ack_delayed))) {
		bbr->min_rtt_us = rs->rtt_us;
		bbr->min_rtt_stamp = tcp_jiffies32;
	}

	if (bbr_probe_rtt_mode_ms > 0 && filter_expired &&
	    !bbr->idle_restart && bbr->mode != BBR_PROBE_RTT) {
		bbr->mode = BBR_PROBE_RTT;  /* dip, drain queue */
		bbr_save_cwnd(sk);  /* note cwnd so we can restore it */
		bbr->probe_rtt_done_stamp = 0;
	}

	if (bbr->mode == BBR_PROBE_RTT) {
		/* Ignore low rate samples during this mode. */
		tp->app_limited =
			(tp->delivered + tcp_packets_in_flight(tp)) ? : 1;

		if(bbr->pmodrl){
			bbr->pmodrl->probe_rtt_flag = 1;
		}

		/* Maintain min packets in flight for max(200 ms, 1 round). */
		if (!bbr->probe_rtt_done_stamp &&
		    tcp_packets_in_flight(tp) <= bbr_cwnd_min_target) {
			bbr->probe_rtt_done_stamp = tcp_jiffies32 +
				msecs_to_jiffies(bbr_probe_rtt_mode_ms);
			bbr->probe_rtt_round_done = 0;
			bbr->next_rtt_delivered = tp->delivered;
			if(bbr->pmodrl){
				bbr->pmodrl->next_rtt_delivered = tp->delivered;
			}
		} else if (bbr->probe_rtt_done_stamp) {
			if (bbr->round_start)
				bbr->probe_rtt_round_done = 1;
			if (bbr->probe_rtt_round_done)
				bbr_check_probe_rtt_done(sk);
		}
	}
	/* Restart after idle ends only once we process a new S/ACK for data */
	if (rs->delivered > 0)
		bbr->idle_restart = 0;
}

static void bbr_update_gains(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	switch (bbr->mode) {
	case BBR_STARTUP:
		bbr->pacing_gain = bbr_high_gain;
		bbr->cwnd_gain	 = bbr_high_gain;
		break;
	case BBR_DRAIN:
		bbr->pacing_gain = bbr_drain_gain;	/* slow, to drain */
		bbr->cwnd_gain	 = bbr_high_gain;	/* keep cwnd */
		break;
	case BBR_PROBE_BW:
		bbr->pacing_gain = (bbr->lt_use_bw ?
				    BBR_UNIT :
				    bbr_pacing_gain[bbr->cycle_idx]);
		bbr->cwnd_gain	 = bbr_cwnd_gain;
		break;
	case BBR_PROBE_RTT:
		bbr->pacing_gain = BBR_UNIT;
		bbr->cwnd_gain	 = BBR_UNIT;
		break;
	default:
		WARN_ONCE(1, "BBR bad mode: %u\n", bbr->mode);
		break;
	}
}

static void bbr_update_model(struct sock *sk, const struct rate_sample *rs)
{
	bbr_update_bw(sk, rs);
	bbr_update_ack_aggregation(sk, rs);
	bbr_update_cycle_phase(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr_check_drain(sk, rs);
	bbr_update_min_rtt(sk, rs);
	bbr_update_gains(sk);
}

static int comp(struct sock *sk, u32 now_us){
	struct bbr *bbr = inet_csk_ca(sk);
	u8 best_index = 0;
	u64 b_diff;
	u64 r_diff;
	u64 flow_len_us;
	u8 i;
	for(i = 1; i < percent_arr_num; i++){
		b_diff = (u64)abs(bbr->pmodrl->B_arr[i] - bbr->pmodrl->B_arr[best_index]);
		r_diff = (u64)abs(bbr->pmodrl->R_arr[i] - bbr->pmodrl->R_arr[best_index]);
		flow_len_us = now_us - bbr->pmodrl->bbr_start_us;
		if(r_diff == 0){
			best_index = i;
		}
		else{
			if(div_u64(b_diff * BASED_SCALE * 2, r_diff) > flow_len_us * BASED_SCALE){
				best_index = i;
			}
			else{
				break;
			}	
		}
	}
	return best_index;
}

static void estimation_classify(struct sock *sk){
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	u32 now_us = jiffies_to_usecs(tcp_jiffies32);
	u32 cur_delivered = tp->delivered - bbr->pmodrl->transfer_start_deliverd;
	u32 cur_lost = tp->lost - bbr->pmodrl->transfer_start_lost;
	u32 d;
	u32 l;
	u64 bef_empty;
	u8 i;
	u64 h;
	u64 t;
	u64 R;
	u64 incr_diff;
	u8 abrupt_decrease_flag = 0;
	u8 best_index = 0;
	u64 lower_bound_B;

	if(use_goodput){
		cur_delivered = tp->snd_una / tp->mss_cache - bbr->pmodrl->transfer_start_deliverd;
	}

	if(bbr->pmodrl->high_loss_flag == 0){
		if(bbr->pmodrl->loss_start_time_us != 0 && bbr->pmodrl->loss_start_time_us + 7 * bbr->min_rtt_us < now_us){
			d = cur_delivered - bbr->pmodrl->before_loss_delivered;
			l = cur_lost - bbr->pmodrl->before_loss_lost;
			// if(d < 10) {
			// 	return;
			// }
			if((d + l) != 0 && (u64)l * 10 > (u64)(d + l) * 2){
				bbr->pmodrl->high_loss_flag = 1;
				t = div_u64(bbr->pmodrl->before_loss_time_us, USEC_PER_MSEC) - div_u64(bbr->pmodrl->bbr_start_us, USEC_PER_MSEC);
				if ((s32)t < 1){
					return;	
				}
				bef_empty = div_u64((u64)bbr->pmodrl->before_loss_delivered * BW_UNIT, bbr->pmodrl->before_loss_time_us - bbr->pmodrl->bbr_start_us);
				bbr->pmodrl->bef_empty_goodput = bef_empty;
				lower_bound_B = (u64)bbr->pmodrl->before_loss_delivered * (BASED_UNIT -  abrupt_decrease_thresh);
				for(i = 0; i < percent_arr_num; i++){
					if(percent_arr[i] == 0){
						bbr->pmodrl->B_arr[i] = 0;
					}
					else{
						t = (BW_UNIT - percent_arr[i]) * lower_bound_B;
						t = t >> BASED_SCALE;
						bbr->pmodrl->B_arr[i] = (u64)bbr->pmodrl->before_loss_delivered * percent_arr[i] + t;
					}
				}
				for(i = 0; i < percent_arr_num; i++){
					if((u64)bbr->pmodrl->before_loss_delivered * BW_UNIT > bbr->pmodrl->B_arr[i]){
						h = (u64)bbr->pmodrl->before_loss_delivered * BW_UNIT - bbr->pmodrl->B_arr[i];
						t = div_u64(bbr->pmodrl->before_loss_time_us, USEC_PER_MSEC) - div_u64(bbr->pmodrl->bbr_start_us, USEC_PER_MSEC);
						if ((s32)t < 1){
							return;	
						}
						R = div_u64(h, bbr->pmodrl->before_loss_time_us - bbr->pmodrl->bbr_start_us);
						bbr->pmodrl->R_arr[i] = max(bbr->pmodrl->R_arr[i], R);
					}
				}
			}
			else{
				bbr->pmodrl->loss_start_time_us = 0;
				return;
			}
		}
		else{
			return;
		}
	}
	for(i = 0; i < percent_arr_num; i++){
		if((u64)cur_delivered * BW_UNIT > bbr->pmodrl->B_arr[i]){
			h = (u64)cur_delivered * BW_UNIT - bbr->pmodrl->B_arr[i];
			t = div_u64(now_us, USEC_PER_MSEC) - div_u64(bbr->pmodrl->bbr_start_us, USEC_PER_MSEC);
			if ((s32)t < 1){
				return;	
			}
			R = div_u64(h, now_us - bbr->pmodrl->bbr_start_us);
			bbr->pmodrl->R_arr[i] = max(bbr->pmodrl->R_arr[i], R);
		}
	}
	best_index = comp(sk, now_us);
	bbr->pmodrl->best_index = best_index;
	while(best_index == 0){
		incr_diff = bbr->pmodrl->B_arr[0] - bbr->pmodrl->B_arr[1];
		for(i = percent_arr_num - 1; i>=1; i--){
			bbr->pmodrl->B_arr[i] = bbr->pmodrl->B_arr[i - 1];
			bbr->pmodrl->R_arr[i] = bbr->pmodrl->R_arr[i - 1];
		}
		bbr->pmodrl->B_arr[0] = bbr->pmodrl->B_arr[0] + incr_diff;
		bbr->pmodrl->R_arr[0] = 0;
		if((u64)cur_delivered * BW_UNIT > bbr->pmodrl->B_arr[0]){
			h = (u64)cur_delivered * BW_UNIT - bbr->pmodrl->B_arr[0];
			R = div_u64(h, now_us - bbr->pmodrl->bbr_start_us);
			bbr->pmodrl->R_arr[i] = max(bbr->pmodrl->R_arr[i], R);	
		}
		if((u64)bbr->pmodrl->before_loss_delivered * BW_UNIT > bbr->pmodrl->B_arr[0]){
			h = (u64)bbr->pmodrl->before_loss_delivered * BW_UNIT - bbr->pmodrl->B_arr[0];
			R = div_u64(h, bbr->pmodrl->before_loss_time_us - bbr->pmodrl->bbr_start_us);
			bbr->pmodrl->R_arr[i] = max(bbr->pmodrl->R_arr[i], R);			
		}
		best_index = comp(sk, now_us);
	}
	bbr->pmodrl->best_index = best_index;
	if(bbr->pmodrl->R_arr[best_index] * BASED_UNIT <= abrupt_decrease_thresh * bbr->pmodrl->bef_empty_goodput){
		abrupt_decrease_flag = 1;
	}
	if(bbr->pmodrl->classify == 1){
		if(!abrupt_decrease_flag){
			// printA(KERN_INFO "!!!Rate fail %llu", bbr->pmodrl->R_arr[best_index]);
			// u64 cycle_mstamp = bbr->pmodrl->cycle_mstamp;
			// memset(bbr->pmodrl, 0, sizeof(struct PMODRL));
			bbr->pmodrl->classify = 2;
			bbr->pmodrl->disable_flag = 1;
			// bbr->pmodrl->cycle_mstamp = cycle_mstamp;
		}
	}
	else{
		// printk(KERN_INFO "!!!%u %u %llu %llu %u %u", bbr->pmodrl->high_loss_flag, abrupt_decrease_flag, bbr->pmodrl->R_arr[best_index], bbr->pmodrl->bef_empty_goodput, now_us, bbr->pmodrl->classify_time_us);
		if(bbr->pmodrl->high_loss_flag && abrupt_decrease_flag){
			if(bbr->pmodrl->classify_time_us == 0){
				bbr->pmodrl->classify_time_us = now_us;
			}
			if(bbr->pmodrl->reset_ltbw_flag == 0){
				bbr_reset_lt_bw_sampling(sk);
				bbr->pmodrl->reset_ltbw_flag = 1;
			}
			
			if(bbr->pmodrl->R_arr[best_index] != bbr->pmodrl->mem_R || bbr->pmodrl->B_arr[best_index] != bbr->pmodrl->mem_B) {
				bbr->pmodrl->classify_time_us = now_us;
				bbr->pmodrl->mem_B = bbr->pmodrl->B_arr[best_index];
				bbr->pmodrl->mem_R = bbr->pmodrl->R_arr[best_index];

			}
			else{
				if(now_us - bbr->pmodrl->classify_time_us > 10 * bbr->min_rtt_us){
					bbr->pmodrl->classify = 1;
					bbr->pmodrl->upper_bound = 1;
					bbr->pmodrl->detected_time = now_us - bbr->pmodrl->bbr_start_us;
					bbr->pmodrl->detected_bytes_acked = tp->bytes_acked;
				}
			}

		}
		else{
			bbr->pmodrl->classify_time_us = 0;
		}
	}

}

static void probe_pmodrl(struct sock *sk) {
	struct bbr *bbr = inet_csk_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);

	if(bbr->pmodrl) {
		if(bbr->pmodrl->classify == 1 && optimize_flag){
			if(bbr->pmodrl->upper_bound != 1 || bbr->pmodrl->nominator != 0) {
				if(bbr->pmodrl->round_start){
					bbr->pmodrl->round_count_no++;
					if(bbr->pmodrl->round_count_no >= monitor_peroid && bbr->pmodrl->mem_B == bbr->pmodrl->B_arr[bbr->pmodrl->best_index] && bbr->pmodrl->mem_R == bbr->pmodrl->R_arr[bbr->pmodrl->best_index]){
						bbr->pmodrl->upper_bound = 1;
						bbr->pmodrl->nominator = 0;
						bbr->pmodrl->round_count_no = 0;
					}
				}
				if(bbr->pmodrl->mem_B != bbr->pmodrl->B_arr[bbr->pmodrl->best_index] || bbr->pmodrl->mem_R != bbr->pmodrl->R_arr[bbr->pmodrl->best_index]){
					bbr->pmodrl->upper_bound = 2;
					bbr->pmodrl->nominator = 0;
					bbr->pmodrl->mem_B = bbr->pmodrl->B_arr[bbr->pmodrl->best_index];
					bbr->pmodrl->mem_R = bbr->pmodrl->R_arr[bbr->pmodrl->best_index];
					bbr->pmodrl->round_count_no = 0;
					bbr->pmodrl->next_rtt_delivered = tp->delivered;

					bbr->pmodrl->dis_loss_start = 2;
				}				
			}
			else{
				if(bbr->pmodrl->round_start) {
					bbr->pmodrl->round_count++;
					if(bbr->pmodrl->round_count >= probe_interval){
						bbr->pmodrl->upper_bound = 1;
						bbr->pmodrl->nominator = 1;
						// bbr->pmodrl->acc_rto_dur = 0;
						bbr->pmodrl->mem_B = bbr->pmodrl->B_arr[bbr->pmodrl->best_index];
						bbr->pmodrl->mem_R = bbr->pmodrl->R_arr[bbr->pmodrl->best_index];
						bbr->pmodrl->round_count = 0;
						bbr->pmodrl->round_count_no = 0;
						bbr_advance_cycle_phase(sk);
						bbr->cycle_idx = 0;
						bbr->mode = BBR_PROBE_BW;
					}
				}
			}

		}
	}
}

static void reset_pmodrl(struct sock *sk, u8 res1, u8 res2){
	struct bbr *bbr = inet_csk_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	char* p;
	int flag = 0;
	if(bbr->pmodrl->classify == 1){
		flag = 1;
	}
	else if(bbr->pmodrl->classify == 2){
		flag = 2;
	}
	else if(bbr->pmodrl->classify != 0){
		flag = bbr->pmodrl->classify;
	}
	p = bbr->pmodrl->buffer;
	memset(bbr->pmodrl,0, sizeof(struct PMODRL));
	bbr->pmodrl->bbr_start_us = jiffies_to_usecs(tcp_jiffies32);
	bbr->pmodrl->transfer_start_lost = tp->lost;
	if(use_goodput){
		bbr->pmodrl->transfer_start_deliverd = tp->snd_una / tp->mss_cache;
	}
	else{
		bbr->pmodrl->transfer_start_deliverd = tp->delivered;
	}
	bbr->pmodrl->buffer = p;
	if(flag == 1){
		bbr->pmodrl->classify = res1;
	}
	else if(flag == 2){
		bbr->pmodrl->classify = res2;
	}
	else if(flag != 0){
		bbr->pmodrl->classify = flag;
	}
}

static void bbr_main(struct sock *sk, const struct rate_sample *rs)
{
	struct bbr *bbr = inet_csk_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	struct inet_sock *inet = inet_sk(sk);
	u32 bw;
	u32 now_us = jiffies_to_usecs(tcp_jiffies32);
	u64 srtt;
	srtt = tp->srtt_us >> 3;

	bbr_update_model(sk, rs);
	
	// bbr_reset_lt_bw_sampling(sk);
	
	if(bbr->pmodrl){
		bbr->pmodrl->latest_ack_us = now_us;

		if(bbr->pmodrl->bbr_start_us == 0){
			bbr->pmodrl->bbr_start_us = now_us;
		}
		if(bbr->pmodrl->disable_flag == 0){
			estimation_classify(sk);
		}

		if(bbr->pmodrl->lastest_ack_loss!=tp->lost){
			if(bbr->pmodrl->high_loss_flag == 0 && bbr->pmodrl->loss_start_time_us == 0){
				bbr->pmodrl->loss_start_time_us = now_us;
			}
		}
		else{
			if(bbr->pmodrl->high_loss_flag == 0 && bbr->pmodrl->loss_start_time_us == 0) {
				bbr->pmodrl->before_loss_delivered = tp->delivered - bbr->pmodrl->transfer_start_deliverd;
				bbr->pmodrl->before_loss_time_us = now_us;
				bbr->pmodrl->before_loss_lost = tp->lost - bbr->pmodrl->transfer_start_lost;
				if(use_goodput){
					bbr->pmodrl->before_loss_delivered = tp->snd_una / tp->mss_cache - bbr->pmodrl->transfer_start_deliverd;
				}
			}
		}
		bbr->pmodrl->lastest_ack_loss = tp->lost;

		if(bbr->pmodrl->classify == 1 && optimize_flag) {
			bbr_reset_lt_bw_sampling(sk);
		}

		if(tp->write_seq - tp->snd_nxt < tp->mss_cache && sk_wmem_alloc_get(sk) < SKB_TRUESIZE(1) && tcp_packets_in_flight(tp) < tp->snd_cwnd && tp->lost_out <= tp->retrans_out){
			bbr->pmodrl->probe_rtt_flag = 0;
		}

		bbr->pmodrl->round_start = 0;
		if (!before(rs->prior_delivered, bbr->pmodrl->next_rtt_delivered) && !(rs->delivered < 0 || rs->interval_us <= 0)) {
			bbr->pmodrl->next_rtt_delivered = tp->delivered;
			bbr->pmodrl->round_start = 1;
		}

		probe_pmodrl(sk);
	}

	bw = bbr_bw(sk);

	bbr_set_pacing_rate(sk, bw, bbr->pacing_gain);
	bbr_set_cwnd(sk, rs, rs->acked_sacked, bw, bbr->cwnd_gain);		

	if(bbr->pmodrl){
		u64 bw1;
		bbr->pmodrl->store_interval+=1;
		if(bbr->pmodrl->buffer && bbr->pmodrl->store_interval >= STORE_INTERVAL){
			bbr->pmodrl->store_interval = 0;
			if(strlen(bbr->pmodrl->buffer) + 90 < MAX_STR_LEN){
				char temp[90];
				memset(temp, 0, 90);
				snprintf(temp, sizeof(temp), "%llu;%u;%llu;%llu-", tp->bytes_acked, bbr->pmodrl->classify, bbr->pmodrl->B_arr[bbr->pmodrl->best_index], bbr->pmodrl->R_arr[bbr->pmodrl->best_index]);
				strcat(bbr->pmodrl->buffer, temp);
			}
		}
		if(exclude_rwnd && tp->chrono_type == TCP_CHRONO_RWND_LIMITED){
			reset_pmodrl(sk, (u8)5, (u8)6);
		}

		if(exclude_RTO && bbr->prev_ca_state == TCP_CA_Loss && inet_csk(sk)->icsk_ca_state != TCP_CA_Loss){
			reset_pmodrl(sk, (u8)7, (u8)8);
		}

		if(exclude_applimited && rs->is_app_limited){
			reset_pmodrl(sk, (u8)9, (u8)10);
		}
		bw1 = (u64)rs->delivered * BW_UNIT;
		do_div(bw1, rs->interval_us);
		if(enable_printk){
			printk(KERN_INFO "!!!ACK: ip:%pI4 port:%hu c:%u B:%llu R:%llu mode:%u idx:%u n:%u u_p:%lu r_p:%lu b:%llu d:%u l:%u rd:%u rl:%u u:%u rc:%u rcn:%u cl:%u def:%u srtt:%llu state:%u cwnd:%u adv:%u inflight:%u rate:%lu s:%llu remain:%u acc_rto:%llu lim:%u limit:%u", 
				&sk->sk_daddr, ntohs(inet->inet_dport), bbr->pmodrl->classify, bbr->pmodrl->B_arr[bbr->pmodrl->best_index], bbr->pmodrl->R_arr[bbr->pmodrl->best_index], 
				bbr->mode, bbr->cycle_idx, bbr->pmodrl->nominator, bbr_bw_to_pacing_rate_pmodrl(sk,bbr->pmodrl->R_arr[bbr->pmodrl->best_index],BBR_UNIT,bbr->pmodrl->nominator), sk->sk_pacing_rate, tp->bytes_acked, tp->delivered, tp->lost, 
				rs->delivered, rs->losses ,bbr->pmodrl->upper_bound, bbr->pmodrl->round_count, bbr->pmodrl->round_count_no, tcp_is_cwnd_limited(sk), bbr->pmodrl->dis_enable_flag, srtt, inet_csk(sk)->icsk_ca_state, tp->snd_cwnd, tp->rcv_wnd,tcp_packets_in_flight(tp),
				bbr_bw_to_pacing_rate(sk, bw1, BBR_UNIT), tp->bytes_sent, tp->write_seq - tp->snd_nxt, bbr->pmodrl->acc_rto_dur, bbr->lt_use_bw, bbr->lt_bw);	
		}	
	}
}

static void bbr_init(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);

	bbr->pmodrl = kmalloc(sizeof(struct PMODRL), GFP_KERNEL);
	if (bbr->pmodrl){
		memset(bbr->pmodrl,0, sizeof(struct PMODRL));
		bbr->pmodrl->bbr_start_us = jiffies_to_usecs(tcp_jiffies32);

	    bbr->pmodrl->buffer = (char*)kmalloc(MAX_STR_LEN, GFP_KERNEL);
	    if(bbr->pmodrl->buffer) {
	    	memset(bbr->pmodrl->buffer, 0, MAX_STR_LEN);
	    }
	}

	bbr->prior_cwnd = 0;
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	bbr->rtt_cnt = 0;
	bbr->next_rtt_delivered = 0;
	bbr->prev_ca_state = TCP_CA_Open;
	bbr->packet_conservation = 0;

	bbr->probe_rtt_done_stamp = 0;
	bbr->probe_rtt_round_done = 0;
	bbr->min_rtt_us = tcp_min_rtt(tp);
	bbr->min_rtt_stamp = tcp_jiffies32;

	minmax_reset(&bbr->bw, bbr->rtt_cnt, 0);  /* init max bw to 0 */

	bbr->has_seen_rtt = 0;
	bbr_init_pacing_rate_from_rtt(sk);

	bbr->round_start = 0;
	bbr->idle_restart = 0;
	bbr->full_bw_reached = 0;
	bbr->full_bw = 0;
	bbr->full_bw_cnt = 0;
	if(bbr->pmodrl){
		bbr->pmodrl->cycle_mstamp = 0;
	}
	bbr->cycle_idx = 0;
	bbr_reset_lt_bw_sampling(sk);
	bbr_reset_startup_mode(sk);

	bbr->ack_epoch_mstamp = tp->tcp_mstamp;
	bbr->ack_epoch_acked = 0;
	bbr->extra_acked_win_rtts = 0;
	bbr->extra_acked_win_idx = 0;
	bbr->extra_acked[0] = 0;
	bbr->extra_acked[1] = 0;

	cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
}

static void bbr_release(struct sock *sk)
{
   	struct bbr *bbr = inet_csk_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	struct inet_sock *inet = inet_sk(sk);

   	if (!bbr->pmodrl)
      		return;
    if(enable_printk){
		printk(KERN_INFO "!!!Release sip:%pI4 sp:%hu dip:%pI4 dp:%hu p:%u c:%u B:%llu R:%llu b:%llu history:%s\n",
				&sk->sk_rcv_saddr, ntohs(inet->inet_sport),
				&sk->sk_daddr, ntohs(inet->inet_dport),
				tp->delivered, bbr->pmodrl->classify,  bbr->pmodrl->B_arr[bbr->pmodrl->best_index], bbr->pmodrl->R_arr[bbr->pmodrl->best_index], bbr->pmodrl->detected_bytes_acked, bbr->pmodrl->buffer);
    }

    if(bbr->pmodrl->buffer){
	   	kfree(bbr->pmodrl->buffer);
	   	bbr->pmodrl->buffer = NULL;
    }
   	kfree(bbr->pmodrl);
   	bbr->pmodrl = NULL;

}


static u32 bbr_sndbuf_expand(struct sock *sk)
{
	/* Provision 3 * cwnd since BBR may slow-start even during recovery. */
	return 3;
}

/* In theory BBR does not need to undo the cwnd since it does not
 * always reduce cwnd on losses (see bbr_main()). Keep it for now.
 */
static u32 bbr_undo_cwnd(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	bbr->full_bw = 0;   /* spurious slow-down; reset full pipe detection */
	bbr->full_bw_cnt = 0;
	bbr_reset_lt_bw_sampling(sk);
	return tcp_sk(sk)->snd_cwnd;
}

/* Entering loss recovery, so save cwnd for when we exit or undo recovery. */
static u32 bbr_ssthresh(struct sock *sk)
{
	bbr_save_cwnd(sk);
	return tcp_sk(sk)->snd_ssthresh;
}

static size_t bbr_get_info(struct sock *sk, u32 ext, int *attr,
			   union tcp_cc_info *info)
{
	if (ext & (1 << (INET_DIAG_BBRINFO - 1)) ||
	    ext & (1 << (INET_DIAG_VEGASINFO - 1))) {
		struct tcp_sock *tp = tcp_sk(sk);
		struct bbr *bbr = inet_csk_ca(sk);
		u64 bw = bbr_bw(sk);

		bw = bw * tp->mss_cache * USEC_PER_SEC >> BW_SCALE;
		memset(&info->bbr, 0, sizeof(info->bbr));
		info->bbr.bbr_bw_lo		= (u32)bw;
		info->bbr.bbr_bw_hi		= (u32)(bw >> 32);
		info->bbr.bbr_min_rtt		= bbr->min_rtt_us;
		info->bbr.bbr_pacing_gain	= bbr->pacing_gain;
		info->bbr.bbr_cwnd_gain		= bbr->cwnd_gain;
		if(bbr->pmodrl){
			if(bbr->pmodrl->classify == 1){
				info->bbr.bbr_bw_lo		= bbr->pmodrl->classify;
				info->bbr.bbr_bw_hi		= bbr->pmodrl->detected_time / 1000;
				info->bbr.bbr_min_rtt		= bbr->pmodrl->detected_bytes_acked;
				info->bbr.bbr_pacing_gain	= (bbr->pmodrl->B_arr[bbr->pmodrl->best_index] * (u64)tcp_sk(sk)->mss_cache / 1024) >> BW_SCALE;
				info->bbr.bbr_cwnd_gain		= (bbr->pmodrl->R_arr[bbr->pmodrl->best_index] * (u64)tcp_sk(sk)->mss_cache * 1000) >> BW_SCALE;
			}
			else{
				info->bbr.bbr_bw_lo		= bbr->pmodrl->classify;
				info->bbr.bbr_bw_hi		= 0;
				info->bbr.bbr_min_rtt		= 0;
				info->bbr.bbr_pacing_gain	= 0;
				info->bbr.bbr_cwnd_gain		= 0;				
			}
		}
		*attr = INET_DIAG_BBRINFO;
		return sizeof(info->bbr);
	}
	return 0;
}

static void bbr_set_state(struct sock *sk, u8 new_state)
{
	struct bbr *bbr = inet_csk_ca(sk);
	// struct tcp_sock *tp = tcp_sk(sk);

	if (new_state == TCP_CA_Loss) {
		struct rate_sample rs = { .losses = 1 };
		// u32 now_us = jiffies_to_usecs(tcp_jiffies32);

		bbr->prev_ca_state = TCP_CA_Loss;
		bbr->full_bw = 0;
		bbr->round_start = 1;	/* treat RTO like end of a round */
		bbr_lt_bw_sampling(sk, &rs);

		// if(bbr->pmodrl && bbr->pmodrl->classify == 1){
		// 	bbr->pmodrl->round_start = 1;
		// 	probe_pmodrl(sk);
		// 	bbr->pmodrl->round_start = 0;

		// 	bbr->pmodrl->acc_rto_dur += now_us - bbr->pmodrl->latest_ack_us;
		// 	// printk(KERN_INFO "!!! now_us:%u lastest:%u rcv_t:%u", now_us, bbr->pmodrl->latest_ack_us, jiffies_to_usecs(tp->rcv_tstamp));
		// 	bbr->pmodrl->latest_ack_us = now_us;
		// 	if(bbr->pmodrl->buffer && strlen(bbr->pmodrl->buffer) + 45 < MAX_STR_LEN){
		// 		char temp[45];
		// 		memset(temp, 0, 45);
		// 		snprintf(temp, sizeof(temp), "%llu|%llu-", tp->bytes_acked, bbr->pmodrl->acc_rto_dur);
		// 		strcat(bbr->pmodrl->buffer, temp);
		// 	}
		// }
	}
}

module_param_named(probe_interval_external, probe_interval, int, 0644);
module_param_named(probe_per_external, probe_per, int, 0644);
module_param_named(optimize_flag_external, optimize_flag, int, 0644);
module_param_named(high_loss_disclassify_external, high_loss_disclassify, int, 0644);
module_param_named(monitor_peroid_external, monitor_peroid, int, 0644);
module_param_named(exclude_RTO_external, exclude_RTO, int, 0644);
module_param_named(exclude_rwnd_external, exclude_rwnd, int, 0644);
module_param_named(use_goodput_external, use_goodput, int, 0644);
module_param_named(exclude_applimited_external, exclude_applimited, int, 0644);
module_param_named(enable_printk_external, enable_printk, int, 0644);

static struct tcp_congestion_ops tcp_bbr_cong_ops __read_mostly = {
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "rtcp_bbr",
	.owner		= THIS_MODULE,
	.init		= bbr_init,
	.release	= bbr_release,
	.cong_control	= bbr_main,
	.sndbuf_expand	= bbr_sndbuf_expand,
	.undo_cwnd	= bbr_undo_cwnd,
	.cwnd_event	= bbr_cwnd_event,
	.ssthresh	= bbr_ssthresh,
	.min_tso_segs	= bbr_min_tso_segs,
	// .tso_segs	= bbr_tso_segs,
	.get_info	= bbr_get_info,
	.set_state	= bbr_set_state,
};

static int __init bbr_register(void)
{
	BUILD_BUG_ON(sizeof(struct bbr) > ICSK_CA_PRIV_SIZE);
	return tcp_register_congestion_control(&tcp_bbr_cong_ops);
}

static void __exit bbr_unregister(void)
{
	tcp_unregister_congestion_control(&tcp_bbr_cong_ops);
}

module_init(bbr_register);
module_exit(bbr_unregister);

MODULE_AUTHOR("Van Jacobson <vanj@google.com>");
MODULE_AUTHOR("Neal Cardwell <ncardwell@google.com>");
MODULE_AUTHOR("Yuchung Cheng <ycheng@google.com>");
MODULE_AUTHOR("Soheil Hassas Yeganeh <soheil@google.com>");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("TCP BBR (Bottleneck Bandwidth and RTT)");
