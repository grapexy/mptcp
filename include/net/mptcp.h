/*
 *	MPTCP implementation
 *
 *	Initial Design & Implementation:
 *	Sébastien Barré <sebastien.barre@uclouvain.be>
 *
 *	Current Maintainer & Author:
 *	Christoph Paasch <christoph.paasch@uclouvain.be>
 *
 *	Additional authors:
 *	Jaakko Korkeaniemi <jaakko.korkeaniemi@aalto.fi>
 *	Gregory Detal <gregory.detal@uclouvain.be>
 *	Fabien Duchêne <fabien.duchene@uclouvain.be>
 *	Andreas Seelinger <Andreas.Seelinger@rwth-aachen.de>
 *	Lavkesh Lahngir <lavkesh51@gmail.com>
 *	Andreas Ripke <ripke@neclab.eu>
 *	Vlad Dogaru <vlad.dogaru@intel.com>
 *	Octavian Purdila <octavian.purdila@intel.com>
 *	John Ronan <jronan@tssg.org>
 *	Catalin Nicutar <catalin.nicutar@gmail.com>
 *	Brandon Heller <brandonh@stanford.edu>
 *
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#ifndef _MPTCP_H
#define _MPTCP_H

#include <linux/inetdevice.h>
#include <linux/ipv6.h>
#include <linux/list.h>
#include <linux/net.h>
#include <linux/netpoll.h>
#include <linux/siphash.h>
#include <linux/skbuff.h>
#include <linux/socket.h>
#include <linux/tcp.h>
#include <linux/kernel.h>

#include <asm/byteorder.h>
#include <asm/unaligned.h>
#include <crypto/hash.h>
#include <crypto/sha.h>
#include <net/tcp.h>

#if defined(__LITTLE_ENDIAN_BITFIELD)
	#define ntohll(x)  be64_to_cpu(x)
	#define htonll(x)  cpu_to_be64(x)
#elif defined(__BIG_ENDIAN_BITFIELD)
	#define ntohll(x) (x)
	#define htonll(x) (x)
#endif

struct mptcp_loc4 {
	u8		loc4_id;
	u8		low_prio:1;
	int		if_idx;
	struct in_addr	addr;
};

struct mptcp_rem4 {
	u8		rem4_id;
	__be16		port;
	struct in_addr	addr;
};

struct mptcp_loc6 {
	u8		loc6_id;
	u8		low_prio:1;
	int		if_idx;
	struct in6_addr	addr;
};

struct mptcp_rem6 {
	u8		rem6_id;
	__be16		port;
	struct in6_addr	addr;
};

struct mptcp_request_sock {
	struct tcp_request_sock		req;
	struct hlist_nulls_node		hash_entry;

	union {
		struct {
			/* Only on initial subflows */
			u64		mptcp_loc_key;
			u64		mptcp_rem_key;
			u32		mptcp_loc_token;
		};

		struct {
			/* Only on additional subflows */
			u32		mptcp_rem_nonce;
			u32		mptcp_loc_nonce;
			u64		mptcp_hash_tmac;
		};
	};

	u8				loc_id;
	u8				rem_id; /* Address-id in the MP_JOIN */
	u16				dss_csum:1,
					rem_key_set:1,
					is_sub:1, /* Is this a new subflow? */
					low_prio:1, /* Interface set to low-prio? */
					rcv_low_prio:1,
					mptcp_ver:4;
};

struct mptcp_options_received {
	u16	saw_mpc:1,
		dss_csum:1,
		drop_me:1,

		is_mp_join:1,
		join_ack:1,

		saw_low_prio:2, /* 0x1 - low-prio set for this subflow
				 * 0x2 - low-prio set for another subflow
				 */
		low_prio:1,

		saw_add_addr:2, /* Saw at least one add_addr option:
				 * 0x1: IPv4 - 0x2: IPv6
				 */
		more_add_addr:1, /* Saw one more add-addr. */

		saw_rem_addr:1, /* Saw at least one rem_addr option */
		more_rem_addr:1, /* Saw one more rem-addr. */

		mp_fail:1,
		mp_fclose:1;
	u8	rem_id;		/* Address-id in the MP_JOIN */
	u8	prio_addr_id;	/* Address-id in the MP_PRIO */

	const unsigned char *add_addr_ptr; /* Pointer to add-address option */
	const unsigned char *rem_addr_ptr; /* Pointer to rem-address option */

	u32	data_ack;
	u32	data_seq;
	u16	data_len;

	u8	mptcp_ver; /* MPTCP version */

	/* Key inside the option (from mp_capable or fast_close) */
	u64	mptcp_sender_key;
	u64	mptcp_receiver_key;

	u32	mptcp_rem_token; /* Remote token */

	u32	mptcp_recv_nonce;
	u64	mptcp_recv_tmac;
	u8	mptcp_recv_mac[20];
};

struct mptcp_tcp_sock {
	struct hlist_node node;
	struct hlist_node cb_list;
	struct mptcp_options_received rx_opt;

	 /* Those three fields record the current mapping */
	u64	map_data_seq;
	u32	map_subseq;
	u16	map_data_len;
	u16	slave_sk:1,
		fully_established:1,
		second_packet:1,
		attached:1,
		send_mp_fail:1,
		include_mpc:1,
		mapping_present:1,
		map_data_fin:1,
		low_prio:1, /* use this socket as backup */
		rcv_low_prio:1, /* Peer sent low-prio option to us */
		send_mp_prio:1, /* Trigger to send mp_prio on this socket */
		pre_established:1; /* State between sending 3rd ACK and
				    * receiving the fourth ack of new subflows.
				    */

	/* isn: needed to translate abs to relative subflow seqnums */
	u32	snt_isn;
	u32	rcv_isn;
	u8	path_index;
	u8	loc_id;
	u8	rem_id;
	u8	sk_err;

#define MPTCP_SCHED_SIZE 16
	u8	mptcp_sched[MPTCP_SCHED_SIZE] __aligned(8);

	int	init_rcv_wnd;
	u32	infinite_cutoff_seq;
	struct delayed_work work;
	u32	mptcp_loc_nonce;
	struct tcp_sock *tp;
	u32	last_end_data_seq;

	/* MP_JOIN subflow: timer for retransmitting the 3rd ack */
	struct timer_list mptcp_ack_timer;

	/* HMAC of the third ack */
	char sender_mac[SHA256_DIGEST_SIZE];
};

struct mptcp_tw {
	struct list_head list;
	u64 loc_key;
	u64 rcv_nxt;
	struct mptcp_cb __rcu *mpcb;
	u8 meta_tw:1,
	   in_list:1;
};

#define MPTCP_PM_NAME_MAX 16
struct mptcp_pm_ops {
	struct list_head list;

	/* Signal the creation of a new MPTCP-session. */
	void (*new_session)(const struct sock *meta_sk);
	void (*release_sock)(struct sock *meta_sk);
	void (*fully_established)(struct sock *meta_sk);
	void (*close_session)(struct sock *meta_sk);
	void (*new_remote_address)(struct sock *meta_sk);
	int  (*get_local_id)(const struct sock *meta_sk, sa_family_t family,
			     union inet_addr *addr, bool *low_prio);
	void (*addr_signal)(struct sock *sk, unsigned *size,
			    struct tcp_out_options *opts, struct sk_buff *skb);
	void (*add_raddr)(struct mptcp_cb *mpcb, const union inet_addr *addr,
			  sa_family_t family, __be16 port, u8 id);
	void (*rem_raddr)(struct mptcp_cb *mpcb, u8 rem_id);
	void (*init_subsocket_v4)(struct sock *sk, struct in_addr addr);
	void (*init_subsocket_v6)(struct sock *sk, struct in6_addr addr);
	void (*established_subflow)(struct sock *sk);
	void (*delete_subflow)(struct sock *sk);
	void (*prio_changed)(struct sock *sk, int low_prio);

	char		name[MPTCP_PM_NAME_MAX];
	struct module	*owner;
};

struct mptcp_sched_ops {
	struct list_head list;

	struct sock *		(*get_subflow)(struct sock *meta_sk,
					       struct sk_buff *skb,
					       bool zero_wnd_test);
	struct sk_buff *	(*next_segment)(struct sock *meta_sk,
						int *reinject,
						struct sock **subsk,
						unsigned int *limit);
	void			(*init)(struct sock *sk);
	void			(*release)(struct sock *sk);

	char			name[MPTCP_SCHED_NAME_MAX];
	struct module		*owner;
};

struct mptcp_cb {
	/* list of sockets in this multipath connection */
	struct hlist_head conn_list;
	/* list of sockets that need a call to release_cb */
	struct hlist_head callback_list;

	/* Lock used for protecting the different rcu-lists of mptcp_cb */
	spinlock_t mpcb_list_lock;

	/* High-order bits of 64-bit sequence numbers */
	u32 snd_high_order[2];
	u32 rcv_high_order[2];

	u16	send_infinite_mapping:1,
		send_mptcpv1_mpcapable:1,
		rem_key_set:1,
		in_time_wait:1,
		list_rcvd:1, /* XXX TO REMOVE */
		addr_signal:1, /* Path-manager wants us to call addr_signal */
		dss_csum:1,
		server_side:1,
		infinite_mapping_rcv:1,
		infinite_mapping_snd:1,
		infinite_send_una_ahead:1, /* While falling back, the snd_una
					    *on meta is ahead of the subflow.
					    */
		dfin_combined:1,   /* Was the DFIN combined with subflow-fin? */
		passive_close:1,
		snd_hiseq_index:1, /* Index in snd_high_order of snd_nxt */
		rcv_hiseq_index:1, /* Index in rcv_high_order of rcv_nxt */
		tcp_ca_explicit_set:1; /* was meta CC set by app? */

#define MPTCP_SCHED_DATA_SIZE 8
	u8 mptcp_sched[MPTCP_SCHED_DATA_SIZE] __aligned(8);
	const struct mptcp_sched_ops *sched_ops;

	struct sk_buff_head reinject_queue;
	/* First cache-line boundary is here minus 8 bytes. But from the
	 * reinject-queue only the next and prev pointers are regularly
	 * accessed. Thus, the whole data-path is on a single cache-line.
	 */

	u64	csum_cutoff_seq;
	u64	infinite_rcv_seq;

	/***** Start of fields, used for connection closure */
	unsigned char	 mptw_state;
	u8		 dfin_path_index;

	struct list_head tw_list;

	/***** Start of fields, used for subflow establishment and closure */
	refcount_t	mpcb_refcnt;

	/* Mutex needed, because otherwise mptcp_close will complain that the
	 * socket is owned by the user.
	 * E.g., mptcp_sub_close_wq is taking the meta-lock.
	 */
	struct mutex	mpcb_mutex;

	/***** Start of fields, used for subflow establishment */
	struct sock *meta_sk;

	/* Master socket, also part of the conn_list, this
	 * socket is the one that the application sees.
	 */
	struct sock *master_sk;

	__u64	mptcp_loc_key;
	__u64	mptcp_rem_key;
	__u32	mptcp_loc_token;
	__u32	mptcp_rem_token;

#define MPTCP_PM_SIZE 608
	u8 mptcp_pm[MPTCP_PM_SIZE] __aligned(8);
	const struct mptcp_pm_ops *pm_ops;

	unsigned long path_index_bits;

	__u8	mptcp_ver;

	/* Original snd/rcvbuf of the initial subflow.
	 * Used for the new subflows on the server-side to allow correct
	 * autotuning
	 */
	int orig_sk_rcvbuf;
	int orig_sk_sndbuf;
	u32 orig_window_clamp;

	struct tcp_info	*master_info;

	u8 add_addr_signal;
	u8 add_addr_accepted;
};

#define MPTCP_VERSION_0 0
#define MPTCP_VERSION_1 1

#define MPTCP_SUB_CAPABLE			0
#define MPTCP_SUB_LEN_CAPABLE_SYN		12
#define MPTCP_SUB_LEN_CAPABLE_SYN_ALIGN		12
#define MPTCP_SUB_LEN_CAPABLE_ACK		20
#define MPTCP_SUB_LEN_CAPABLE_ACK_ALIGN		20

#define MPTCPV1_SUB_LEN_CAPABLE_SYN		4
#define MPTCPV1_SUB_LEN_CAPABLE_SYN_ALIGN	4
#define MPTCPV1_SUB_LEN_CAPABLE_SYNACK		12
#define MPTCPV1_SUB_LEN_CAPABLE_SYNACK_ALIGN	12
#define MPTCPV1_SUB_LEN_CAPABLE_ACK		20
#define MPTCPV1_SUB_LEN_CAPABLE_ACK_ALIGN	20
#define MPTCPV1_SUB_LEN_CAPABLE_DATA		22
#define MPTCPV1_SUB_LEN_CAPABLE_DATA_CSUM	24
#define MPTCPV1_SUB_LEN_CAPABLE_DATA_ALIGN	24

#define MPTCP_SUB_JOIN			1
#define MPTCP_SUB_LEN_JOIN_SYN		12
#define MPTCP_SUB_LEN_JOIN_SYN_ALIGN	12
#define MPTCP_SUB_LEN_JOIN_SYNACK	16
#define MPTCP_SUB_LEN_JOIN_SYNACK_ALIGN	16
#define MPTCP_SUB_LEN_JOIN_ACK		24
#define MPTCP_SUB_LEN_JOIN_ACK_ALIGN	24

#define MPTCP_SUB_DSS		2
#define MPTCP_SUB_LEN_DSS	4
#define MPTCP_SUB_LEN_DSS_ALIGN	4

/* Lengths for seq and ack are the ones without the generic MPTCP-option header,
 * as they are part of the DSS-option.
 * To get the total length, just add the different options together.
 */
#define MPTCP_SUB_LEN_SEQ	10
#define MPTCP_SUB_LEN_SEQ_CSUM	12
#define MPTCP_SUB_LEN_SEQ_ALIGN	12

#define MPTCP_SUB_LEN_SEQ_64		14
#define MPTCP_SUB_LEN_SEQ_CSUM_64	16
#define MPTCP_SUB_LEN_SEQ_64_ALIGN	16

#define MPTCP_SUB_LEN_ACK	4
#define MPTCP_SUB_LEN_ACK_ALIGN	4

#define MPTCP_SUB_LEN_ACK_64		8
#define MPTCP_SUB_LEN_ACK_64_ALIGN	8

/* This is the "default" option-length we will send out most often.
 * MPTCP DSS-header
 * 32-bit data sequence number
 * 32-bit data ack
 *
 * It is necessary to calculate the effective MSS we will be using when
 * sending data.
 */
#define MPTCP_SUB_LEN_DSM_ALIGN  (MPTCP_SUB_LEN_DSS_ALIGN +		\
				  MPTCP_SUB_LEN_SEQ_ALIGN +		\
				  MPTCP_SUB_LEN_ACK_ALIGN)

#define MPTCP_SUB_ADD_ADDR		3
#define MPTCP_SUB_LEN_ADD_ADDR4		8
#define MPTCP_SUB_LEN_ADD_ADDR4_VER1	16
#define MPTCP_SUB_LEN_ADD_ADDR6		20
#define MPTCP_SUB_LEN_ADD_ADDR6_VER1	28
#define MPTCP_SUB_LEN_ADD_ADDR4_ALIGN	8
#define MPTCP_SUB_LEN_ADD_ADDR4_ALIGN_VER1	16
#define MPTCP_SUB_LEN_ADD_ADDR6_ALIGN	20
#define MPTCP_SUB_LEN_ADD_ADDR6_ALIGN_VER1	28

#define MPTCP_SUB_REMOVE_ADDR	4
#define MPTCP_SUB_LEN_REMOVE_ADDR	4

#define MPTCP_SUB_PRIO		5
#define MPTCP_SUB_LEN_PRIO	3
#define MPTCP_SUB_LEN_PRIO_ADDR	4
#define MPTCP_SUB_LEN_PRIO_ALIGN	4

#define MPTCP_SUB_FAIL		6
#define MPTCP_SUB_LEN_FAIL	12
#define MPTCP_SUB_LEN_FAIL_ALIGN	12

#define MPTCP_SUB_FCLOSE	7
#define MPTCP_SUB_LEN_FCLOSE	12
#define MPTCP_SUB_LEN_FCLOSE_ALIGN	12


#define OPTION_MPTCP		(1 << 5)

/* Max number of fastclose retransmissions */
#define MPTCP_FASTCLOSE_RETRIES 3

#ifdef CONFIG_MPTCP

/* Used for checking if the mptcp initialization has been successful */
extern bool mptcp_init_failed;

/* MPTCP options */
#define OPTION_TYPE_SYN		(1 << 0)
#define OPTION_TYPE_SYNACK	(1 << 1)
#define OPTION_TYPE_ACK		(1 << 2)
#define OPTION_MP_CAPABLE	(1 << 3)
#define OPTION_DATA_ACK		(1 << 4)
#define OPTION_ADD_ADDR		(1 << 5)
#define OPTION_MP_JOIN		(1 << 6)
#define OPTION_MP_FAIL		(1 << 7)
#define OPTION_MP_FCLOSE	(1 << 8)
#define OPTION_REMOVE_ADDR	(1 << 9)
#define OPTION_MP_PRIO		(1 << 10)

/* MPTCP flags: both TX and RX */
#define MPTCPHDR_SEQ		0x01 /* DSS.M option is present */
#define MPTCPHDR_FIN		0x02 /* DSS.F option is present */
#define MPTCPHDR_SEQ64_INDEX	0x04 /* index of seq in mpcb->snd_high_order */
#define MPTCPHDR_MPC_DATA	0x08
/* MPTCP flags: RX only */
#define MPTCPHDR_ACK		0x10
#define MPTCPHDR_SEQ64_SET	0x20 /* Did we received a 64-bit seq number?  */
#define MPTCPHDR_SEQ64_OFO	0x40 /* Is it not in our circular array? */
#define MPTCPHDR_DSS_CSUM	0x80
/* MPTCP flags: TX only */
#define MPTCPHDR_INF		0x10
#define MPTCP_REINJECT		0x20 /* Did we reinject this segment? */

struct mptcp_option {
	__u8	kind;
	__u8	len;
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u8	ver:4,
		sub:4;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__u8	sub:4,
		ver:4;
#else
#error	"Adjust your <asm/byteorder.h> defines"
#endif
};

struct mp_capable {
	__u8	kind;
	__u8	len;
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u8	ver:4,
		sub:4;
	__u8	h:1,
		rsv:5,
		b:1,
		a:1;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__u8	sub:4,
		ver:4;
	__u8	a:1,
		b:1,
		rsv:5,
		h:1;
#else
#error	"Adjust your <asm/byteorder.h> defines"
#endif
	__u64	sender_key;
	__u64	receiver_key;
} __attribute__((__packed__));

struct mp_join {
	__u8	kind;
	__u8	len;
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u8	b:1,
		rsv:3,
		sub:4;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__u8	sub:4,
		rsv:3,
		b:1;
#else
#error	"Adjust your <asm/byteorder.h> defines"
#endif
	__u8	addr_id;
	union {
		struct {
			u32	token;
			u32	nonce;
		} syn;
		struct {
			__u64	mac;
			u32	nonce;
		} synack;
		struct {
			__u8	mac[20];
		} ack;
	} u;
} __attribute__((__packed__));

struct mp_dss {
	__u8	kind;
	__u8	len;
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u16	rsv1:4,
		sub:4,
		A:1,
		a:1,
		M:1,
		m:1,
		F:1,
		rsv2:3;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__u16	sub:4,
		rsv1:4,
		rsv2:3,
		F:1,
		m:1,
		M:1,
		a:1,
		A:1;
#else
#error	"Adjust your <asm/byteorder.h> defines"
#endif
};

struct mp_add_addr {
	__u8	kind;
	__u8	len;
#if defined(__LITTLE_ENDIAN_BITFIELD)
	union {
		struct {
			__u8	ipver:4,
				sub:4;
		} v0;
		struct {
			__u8	echo:1,
				rsv:3,
				sub:4;
		} v1;
	} u_bit;
#elif defined(__BIG_ENDIAN_BITFIELD)
	union {
		struct {
			__u8	sub:4,
				ipver:4;
		} v0;
		struct {
			__u8	sub:4,
				rsv:3,
				echo:1;
		} v1;
	} u_bit;
#else
#error	"Adjust your <asm/byteorder.h> defines"
#endif
	__u8	addr_id;
	union {
		struct {
			struct in_addr	addr;
			__be16		port;
			__u8		mac[8];
		} v4;
		struct {
			struct in6_addr	addr;
			__be16		port;
			__u8		mac[8];
		} v6;
	} u;
} __attribute__((__packed__));

struct mp_remove_addr {
	__u8	kind;
	__u8	len;
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u8	rsv:4,
		sub:4;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__u8	sub:4,
		rsv:4;
#else
#error "Adjust your <asm/byteorder.h> defines"
#endif
	/* list of addr_id */
	__u8	addrs_id;
};

struct mp_fail {
	__u8	kind;
	__u8	len;
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u16	rsv1:4,
		sub:4,
		rsv2:8;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__u16	sub:4,
		rsv1:4,
		rsv2:8;
#else
#error	"Adjust your <asm/byteorder.h> defines"
#endif
	__be64	data_seq;
} __attribute__((__packed__));

struct mp_fclose {
	__u8	kind;
	__u8	len;
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u16	rsv1:4,
		sub:4,
		rsv2:8;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__u16	sub:4,
		rsv1:4,
		rsv2:8;
#else
#error	"Adjust your <asm/byteorder.h> defines"
#endif
	__u64	key;
} __attribute__((__packed__));

struct mp_prio {
	__u8	kind;
	__u8	len;
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u8	b:1,
		rsv:3,
		sub:4;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__u8	sub:4,
		rsv:3,
		b:1;
#else
#error	"Adjust your <asm/byteorder.h> defines"
#endif
	__u8	addr_id;
} __attribute__((__packed__));

struct mptcp_hashtable {
	struct hlist_nulls_head *hashtable;
	unsigned int mask;
};

static inline int mptcp_sub_len_dss(const struct mp_dss *m, const int csum)
{
	return 4 + m->A * (4 + m->a * 4) + m->M * (10 + m->m * 4 + csum * 2);
}

#define MPTCP_ENABLE		0x01
#define MPTCP_SOCKOPT		0x02
#define MPTCP_CLIENT_DISABLE	0x04
#define MPTCP_SERVER_DISABLE	0x08

extern int sysctl_mptcp_enabled;
extern int sysctl_mptcp_version;
extern int sysctl_mptcp_checksum;
extern int sysctl_mptcp_debug;
extern int sysctl_mptcp_syn_retries;

extern struct workqueue_struct *mptcp_wq;

#define mptcp_debug(fmt, args...)						\
	do {									\
		if (unlikely(sysctl_mptcp_debug))				\
			pr_err(fmt, ##args);					\
	} while (0)

static inline struct sock *mptcp_to_sock(const struct mptcp_tcp_sock *mptcp)
{
	return (struct sock *)mptcp->tp;
}

#define mptcp_for_each_sub(__mpcb, __mptcp)					\
	hlist_for_each_entry_rcu(__mptcp, &((__mpcb)->conn_list), node)

/* Must be called with the appropriate lock held */
#define mptcp_for_each_sub_safe(__mpcb, __mptcp, __tmp)				\
	hlist_for_each_entry_safe(__mptcp, __tmp, &((__mpcb)->conn_list), node)

/* Iterates over all bit set to 1 in a bitset */
#define mptcp_for_each_bit_set(b, i)					\
	for (i = ffs(b) - 1; i >= 0; i = ffs(b >> (i + 1) << (i + 1)) - 1)

#define mptcp_for_each_bit_unset(b, i)					\
	mptcp_for_each_bit_set(~b, i)

#define MPTCP_INC_STATS(net, field)	SNMP_INC_STATS((net)->mptcp.mptcp_statistics, field)
#define MPTCP_DEC_STATS(net, field)	SNMP_DEC_STATS((net)->mptcp.mptcp_statistics, field)
#define MPTCP_ADD_STATS(net, field, val)	SNMP_ADD_STATS((net)->mptcp.mptcp_statistics, field, val)

enum
{
	MPTCP_MIB_NUM = 0,
	MPTCP_MIB_MPCAPABLEPASSIVE,	/* Received SYN with MP_CAPABLE */
	MPTCP_MIB_MPCAPABLEACTIVE,	/* Sent SYN with MP_CAPABLE */
	MPTCP_MIB_MPCAPABLEACTIVEACK,	/* Received SYN/ACK with MP_CAPABLE */
	MPTCP_MIB_MPCAPABLEPASSIVEACK,	/* Received third ACK with MP_CAPABLE */
	MPTCP_MIB_MPCAPABLEPASSIVEFALLBACK,/* Server-side fallback during 3-way handshake */
	MPTCP_MIB_MPCAPABLEACTIVEFALLBACK, /* Client-side fallback during 3-way handshake */
	MPTCP_MIB_MPCAPABLERETRANSFALLBACK,/* Client-side stopped sending MP_CAPABLE after too many SYN-retransmissions */
	MPTCP_MIB_CSUMENABLED,		/* Created MPTCP-connection with DSS-checksum enabled */
	MPTCP_MIB_RETRANSSEGS,		/* Segments retransmitted at the MPTCP-level */
	MPTCP_MIB_MPFAILRX,		/* Received an MP_FAIL */
	MPTCP_MIB_CSUMFAIL,		/* Received segment with invalid checksum */
	MPTCP_MIB_FASTCLOSERX,		/* Recevied a FAST_CLOSE */
	MPTCP_MIB_FASTCLOSETX,		/* Sent a FAST_CLOSE */
	MPTCP_MIB_FBACKSUB,		/* Fallback upon ack without data-ack on new subflow */
	MPTCP_MIB_FBACKINIT,		/* Fallback upon ack without data-ack on initial subflow */
	MPTCP_MIB_FBDATASUB,		/* Fallback upon data without DSS at the beginning on new subflow */
	MPTCP_MIB_FBDATAINIT,		/* Fallback upon data without DSS at the beginning on initial subflow */
	MPTCP_MIB_REMADDRSUB,		/* Remove subflow due to REMOVE_ADDR */
	MPTCP_MIB_JOINNOTOKEN,		/* Received MP_JOIN but the token was not found */
	MPTCP_MIB_JOINFALLBACK,		/* Received MP_JOIN on session that has fallen back to reg. TCP */
	MPTCP_MIB_JOINSYNTX,		/* Sent a SYN + MP_JOIN */
	MPTCP_MIB_JOINSYNRX,		/* Received a SYN + MP_JOIN */
	MPTCP_MIB_JOINSYNACKRX,		/* Received a SYN/ACK + MP_JOIN */
	MPTCP_MIB_JOINSYNACKMAC,	/* HMAC was wrong on SYN/ACK + MP_JOIN */
	MPTCP_MIB_JOINACKRX,		/* Received an ACK + MP_JOIN */
	MPTCP_MIB_JOINACKMAC,		/* HMAC was wrong on ACK + MP_JOIN */
	MPTCP_MIB_JOINACKFAIL,		/* Third ACK on new subflow did not contain an MP_JOIN */
	MPTCP_MIB_JOINACKRTO,		/* Retransmission timer for third ACK + MP_JOIN timed out */
	MPTCP_MIB_JOINACKRXMIT,		/* Retransmitted an ACK + MP_JOIN */
	MPTCP_MIB_NODSSWINDOW,		/* Received too many packets without a DSS-option */
	MPTCP_MIB_DSSNOMATCH,		/* Received a new mapping that did not match the previous one */
	MPTCP_MIB_INFINITEMAPRX,	/* Received an infinite mapping */
	MPTCP_MIB_DSSTCPMISMATCH,	/* DSS-mapping did not map with TCP's sequence numbers */
	MPTCP_MIB_DSSTRIMHEAD,		/* Trimmed segment at the head (coalescing middlebox) */
	MPTCP_MIB_DSSSPLITTAIL,		/* Trimmed segment at the tail (coalescing middlebox) */
	MPTCP_MIB_PURGEOLD,		/* Removed old skb from the rcv-queue due to missing DSS-mapping */
	MPTCP_MIB_ADDADDRRX,		/* Received an ADD_ADDR */
	MPTCP_MIB_ADDADDRTX,		/* Sent an ADD_ADDR */
	MPTCP_MIB_REMADDRRX,		/* Received a REMOVE_ADDR */
	MPTCP_MIB_REMADDRTX,		/* Sent a REMOVE_ADDR */
	MPTCP_MIB_JOINALTERNATEPORT,	/* Established a subflow on a different destination port-number */
	MPTCP_MIB_CURRESTAB,		/* Current established MPTCP connections */
	__MPTCP_MIB_MAX
};

#define MPTCP_MIB_MAX __MPTCP_MIB_MAX
struct mptcp_mib {
	unsigned long	mibs[MPTCP_MIB_MAX];
};

extern struct lock_class_key meta_key;
extern char *meta_key_name;
extern struct lock_class_key meta_slock_key;
extern char *meta_slock_key_name;

extern siphash_key_t mptcp_secret;

/* This is needed to ensure that two subsequent key/nonce-generation result in
 * different keys/nonces if the IPs and ports are the same.
 */
extern u32 mptcp_seed;

extern struct mptcp_hashtable mptcp_tk_htable;

/* Request-sockets can be hashed in the tk_htb for collision-detection or in
 * the regular htb for join-connections. We need to define different NULLS
 * values so that we can correctly detect a request-socket that has been
 * recycled. See also c25eb3bfb9729.
 */
#define MPTCP_REQSK_NULLS_BASE (1U << 29)


void mptcp_data_ready(struct sock *sk);
void mptcp_write_space(struct sock *sk);

void mptcp_add_meta_ofo_queue(const struct sock *meta_sk, struct sk_buff *skb,
			      struct sock *sk);
void mptcp_cleanup_rbuf(struct sock *meta_sk, int copied);
int mptcp_add_sock(struct sock *meta_sk, struct sock *sk, u8 loc_id, u8 rem_id,
		   gfp_t flags);
void mptcp_del_sock(struct sock *sk);
void mptcp_update_metasocket(const struct sock *meta_sk);
void mptcp_reinject_data(struct sock *orig_sk, int clone_it);
void mptcp_update_sndbuf(const struct tcp_sock *tp);
void mptcp_send_fin(struct sock *meta_sk);
void mptcp_send_active_reset(struct sock *meta_sk, gfp_t priority);
bool mptcp_write_xmit(struct sock *sk, unsigned int mss_now, int nonagle,
		      int push_one, gfp_t gfp);
void tcp_parse_mptcp_options(const struct sk_buff *skb,
			     struct mptcp_options_received *mopt);
bool mptcp_handle_ack_in_infinite(struct sock *sk, const struct sk_buff *skb,
				  int flag);
void mptcp_parse_options(const uint8_t *ptr, int opsize,
			 struct mptcp_options_received *mopt,
			 const struct sk_buff *skb,
			 struct tcp_sock *tp);
void mptcp_syn_options(const struct sock *sk, struct tcp_out_options *opts,
		       unsigned *remaining);
void mptcp_synack_options(struct request_sock *req,
			  struct tcp_out_options *opts,
			  unsigned *remaining);
void mptcp_established_options(struct sock *sk, struct sk_buff *skb,
			       struct tcp_out_options *opts, unsigned *size);
void mptcp_options_write(__be32 *ptr, struct tcp_sock *tp,
			 const struct tcp_out_options *opts,
			 struct sk_buff *skb);
void mptcp_close(struct sock *meta_sk, long timeout);
bool mptcp_doit(struct sock *sk);
int mptcp_create_master_sk(struct sock *meta_sk, __u64 remote_key,
			   int rem_key_set, __u8 mptcp_ver, u32 window);
int mptcp_check_req_fastopen(struct sock *child, struct request_sock *req);
int mptcp_check_req_master(struct sock *sk, struct sock *child,
			   struct request_sock *req, const struct sk_buff *skb,
			   const struct mptcp_options_received *mopt,
			   int drop, u32 tsoff);
struct sock *mptcp_check_req_child(struct sock *meta_sk,
				   struct sock *child,
				   struct request_sock *req,
				   struct sk_buff *skb,
				   const struct mptcp_options_received *mopt);
u32 __mptcp_select_window(struct sock *sk);
void mptcp_select_initial_window(const struct sock *sk, int __space, __u32 mss,
				 __u32 *rcv_wnd, __u32 *window_clamp,
				 int wscale_ok, __u8 *rcv_wscale,
				 __u32 init_rcv_wnd);
unsigned int mptcp_current_mss(struct sock *meta_sk);
void mptcp_hmac(u8 ver, const u8 *key_1, const u8 *key_2, u8 *hash_out,
		int arg_num, ...);
void mptcp_fin(struct sock *meta_sk);
void mptcp_meta_retransmit_timer(struct sock *meta_sk);
void mptcp_sub_retransmit_timer(struct sock *sk);
int mptcp_write_wakeup(struct sock *meta_sk, int mib);
void mptcp_sub_close_wq(struct work_struct *work);
void mptcp_sub_close(struct sock *sk, unsigned long delay);
struct sock *mptcp_select_ack_sock(const struct sock *meta_sk);
int mptcp_getsockopt(struct sock *meta_sk, int level, int optname,
		     char __user *optval, int __user *optlen);
void mptcp_prepare_for_backlog(struct sock *sk, struct sk_buff *skb);
void mptcp_initialize_recv_vars(struct tcp_sock *meta_tp, struct mptcp_cb *mpcb,
				__u64 remote_key);
int mptcp_backlog_rcv(struct sock *meta_sk, struct sk_buff *skb);
void mptcp_ack_handler(struct timer_list *t);
bool mptcp_check_rtt(const struct tcp_sock *tp, int time);
int mptcp_check_snd_buf(const struct tcp_sock *tp);
bool mptcp_handle_options(struct sock *sk, const struct tcphdr *th,
			  const struct sk_buff *skb);
void __init mptcp_init(void);
void mptcp_destroy_sock(struct sock *sk);
int mptcp_rcv_synsent_state_process(struct sock *sk, struct sock **skptr,
				    const struct sk_buff *skb,
				    const struct mptcp_options_received *mopt);
unsigned int mptcp_xmit_size_goal(const struct sock *meta_sk, u32 mss_now,
				  int large_allowed);
int mptcp_init_tw_sock(struct sock *sk, struct tcp_timewait_sock *tw);
void mptcp_twsk_destructor(struct tcp_timewait_sock *tw);
void mptcp_time_wait(struct sock *sk, int state, int timeo);
void mptcp_disconnect(struct sock *meta_sk);
bool mptcp_should_expand_sndbuf(const struct sock *sk);
int mptcp_retransmit_skb(struct sock *meta_sk, struct sk_buff *skb);
void mptcp_tsq_flags(struct sock *sk);
void mptcp_tsq_sub_deferred(struct sock *meta_sk);
struct mp_join *mptcp_find_join(const struct sk_buff *skb);
void mptcp_hash_remove_bh(struct tcp_sock *meta_tp);
struct sock *mptcp_hash_find(const struct net *net, const u32 token);
int mptcp_lookup_join(struct sk_buff *skb, struct inet_timewait_sock *tw);
int mptcp_do_join_short(struct sk_buff *skb,
			const struct mptcp_options_received *mopt,
			struct net *net);
void mptcp_reqsk_destructor(struct request_sock *req);
void mptcp_connect_init(struct sock *sk);
void mptcp_sub_force_close(struct sock *sk);
int mptcp_sub_len_remove_addr_align(u16 bitfield);
void mptcp_join_reqsk_init(const struct mptcp_cb *mpcb,
			   const struct request_sock *req,
			   struct sk_buff *skb);
void mptcp_reqsk_init(struct request_sock *req, const struct sock *sk,
		      const struct sk_buff *skb, bool want_cookie);
int mptcp_conn_request(struct sock *sk, struct sk_buff *skb);
void mptcp_enable_sock(struct sock *sk);
void mptcp_disable_sock(struct sock *sk);
void mptcp_cookies_reqsk_init(struct request_sock *req,
			      struct mptcp_options_received *mopt,
			      struct sk_buff *skb);
void mptcp_mpcb_put(struct mptcp_cb *mpcb);
int mptcp_finish_handshake(struct sock *child, struct sk_buff *skb);
int mptcp_get_info(const struct sock *meta_sk, char __user *optval, int optlen);
void mptcp_clear_sk(struct sock *sk, int size);

/* MPTCP-path-manager registration/initialization functions */
int mptcp_register_path_manager(struct mptcp_pm_ops *pm);
void mptcp_unregister_path_manager(struct mptcp_pm_ops *pm);
void mptcp_init_path_manager(struct mptcp_cb *mpcb);
void mptcp_cleanup_path_manager(struct mptcp_cb *mpcb);
void mptcp_fallback_default(struct mptcp_cb *mpcb);
void mptcp_get_default_path_manager(char *name);
int mptcp_set_scheduler(struct sock *sk, const char *name);
int mptcp_set_path_manager(struct sock *sk, const char *name);
int mptcp_set_default_path_manager(const char *name);
extern struct mptcp_pm_ops mptcp_pm_default;

/* MPTCP-scheduler registration/initialization functions */
int mptcp_register_scheduler(struct mptcp_sched_ops *sched);
void mptcp_unregister_scheduler(struct mptcp_sched_ops *sched);
void mptcp_init_scheduler(struct mptcp_cb *mpcb);
void mptcp_cleanup_scheduler(struct mptcp_cb *mpcb);
void mptcp_get_default_scheduler(char *name);
int mptcp_set_default_scheduler(const char *name);
bool mptcp_is_available(struct sock *sk, const struct sk_buff *skb,
			bool zero_wnd_test);
bool mptcp_is_def_unavailable(struct sock *sk);
bool subflow_is_active(const struct tcp_sock *tp);
bool subflow_is_backup(const struct tcp_sock *tp);
struct sock *get_available_subflow(struct sock *meta_sk, struct sk_buff *skb,
				   bool zero_wnd_test);
struct sk_buff *mptcp_next_segment(struct sock *meta_sk,
				   int *reinject,
				   struct sock **subsk,
				   unsigned int *limit);
extern struct mptcp_sched_ops mptcp_sched_default;

/* Initializes function-pointers and MPTCP-flags */
static inline void mptcp_init_tcp_sock(struct sock *sk)
{
	if (!mptcp_init_failed && sysctl_mptcp_enabled == MPTCP_ENABLE)
		mptcp_enable_sock(sk);
}

static inline void mptcp_init_listen(struct sock *sk)
{
	if (!mptcp_init_failed &&
	    sk->sk_type == SOCK_STREAM && sk->sk_protocol == IPPROTO_TCP &&
#ifdef CONFIG_TCP_MD5SIG
	    !rcu_access_pointer(tcp_sk(sk)->md5sig_info) &&
#endif
	    sysctl_mptcp_enabled & MPTCP_ENABLE &&
	    !(sysctl_mptcp_enabled & MPTCP_SERVER_DISABLE))
		mptcp_enable_sock(sk);
}

static inline void mptcp_init_connect(struct sock *sk)
{
	if (!mptcp_init_failed &&
	    sk->sk_type == SOCK_STREAM && sk->sk_protocol == IPPROTO_TCP &&
#ifdef CONFIG_TCP_MD5SIG
	    !rcu_access_pointer(tcp_sk(sk)->md5sig_info) &&
#endif
	    sysctl_mptcp_enabled & MPTCP_ENABLE &&
	    !(sysctl_mptcp_enabled & MPTCP_CLIENT_DISABLE))
		mptcp_enable_sock(sk);
}

static inline int mptcp_pi_to_flag(int pi)
{
	return 1 << (pi - 1);
}

static inline
struct mptcp_request_sock *mptcp_rsk(const struct request_sock *req)
{
	return (struct mptcp_request_sock *)req;
}

static inline
struct request_sock *rev_mptcp_rsk(const struct mptcp_request_sock *req)
{
	return (struct request_sock *)req;
}

static inline bool mptcp_can_sendpage(struct sock *sk)
{
	struct mptcp_tcp_sock *mptcp;

	if (tcp_sk(sk)->mpcb->dss_csum)
		return false;

	mptcp_for_each_sub(tcp_sk(sk)->mpcb, mptcp) {
		struct sock *sk_it = mptcp_to_sock(mptcp);

		if (!(sk_it->sk_route_caps & NETIF_F_SG))
			return false;
	}

	return true;
}

static inline void mptcp_push_pending_frames(struct sock *meta_sk)
{
	/* We check packets out and send-head here. TCP only checks the
	 * send-head. But, MPTCP also checks packets_out, as this is an
	 * indication that we might want to do opportunistic reinjection.
	 */
	if (tcp_sk(meta_sk)->packets_out || tcp_send_head(meta_sk)) {
		struct tcp_sock *tp = tcp_sk(meta_sk);

		/* We don't care about the MSS, because it will be set in
		 * mptcp_write_xmit.
		 */
		__tcp_push_pending_frames(meta_sk, 0, tp->nonagle);
	}
}

static inline void mptcp_send_reset(struct sock *sk)
{
	if (tcp_need_reset(sk->sk_state))
		tcp_sk(sk)->ops->send_active_reset(sk, GFP_ATOMIC);
	mptcp_sub_force_close(sk);
}

static inline void mptcp_sub_force_close_all(struct mptcp_cb *mpcb,
					     struct sock *except)
{
	struct mptcp_tcp_sock *mptcp;
	struct hlist_node *tmp;

	mptcp_for_each_sub_safe(mpcb, mptcp, tmp) {
		struct sock *sk_it = mptcp_to_sock(mptcp);

		if (sk_it != except)
			mptcp_send_reset(sk_it);
	}
}

static inline bool mptcp_is_data_mpcapable(const struct sk_buff *skb)
{
	return TCP_SKB_CB(skb)->mptcp_flags & MPTCPHDR_MPC_DATA;
}

static inline bool mptcp_is_data_seq(const struct sk_buff *skb)
{
	return TCP_SKB_CB(skb)->mptcp_flags & MPTCPHDR_SEQ;
}

static inline bool mptcp_is_data_fin(const struct sk_buff *skb)
{
	return TCP_SKB_CB(skb)->mptcp_flags & MPTCPHDR_FIN;
}

/* Is it a data-fin while in infinite mapping mode?
 * In infinite mode, a subflow-fin is in fact a data-fin.
 */
static inline bool mptcp_is_data_fin2(const struct sk_buff *skb,
				     const struct tcp_sock *tp)
{
	return mptcp_is_data_fin(skb) ||
	       (tp->mpcb->infinite_mapping_rcv &&
	        (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN));
}

static inline u8 mptcp_get_64_bit(u64 data_seq, struct mptcp_cb *mpcb)
{
	u64 data_seq_high = (u32)(data_seq >> 32);

	if (mpcb->rcv_high_order[0] == data_seq_high)
		return 0;
	else if (mpcb->rcv_high_order[1] == data_seq_high)
		return MPTCPHDR_SEQ64_INDEX;
	else
		return MPTCPHDR_SEQ64_OFO;
}

/* Sets the data_seq and returns pointer to the in-skb field of the data_seq.
 * If the packet has a 64-bit dseq, the pointer points to the last 32 bits.
 */
static inline __u32 *mptcp_skb_set_data_seq(const struct sk_buff *skb,
					    u32 *data_seq,
					    struct mptcp_cb *mpcb)
{
	__u32 *ptr = (__u32 *)(skb_transport_header(skb) + TCP_SKB_CB(skb)->dss_off);

	if (TCP_SKB_CB(skb)->mptcp_flags & MPTCPHDR_SEQ64_SET) {
		u64 data_seq64 = get_unaligned_be64(ptr);

		if (mpcb)
			TCP_SKB_CB(skb)->mptcp_flags |= mptcp_get_64_bit(data_seq64, mpcb);

		*data_seq = (u32)data_seq64;
		ptr++;
	} else {
		*data_seq = get_unaligned_be32(ptr);
	}

	return ptr;
}

static inline struct sock *mptcp_meta_sk(const struct sock *sk)
{
	return tcp_sk(sk)->meta_sk;
}

static inline struct tcp_sock *mptcp_meta_tp(const struct tcp_sock *tp)
{
	return tcp_sk(tp->meta_sk);
}

static inline int is_meta_tp(const struct tcp_sock *tp)
{
	return tp->mpcb && mptcp_meta_tp(tp) == tp;
}

static inline int is_meta_sk(const struct sock *sk)
{
	return sk->sk_state != TCP_NEW_SYN_RECV &&
	       sk->sk_type == SOCK_STREAM && sk->sk_protocol == IPPROTO_TCP &&
	       mptcp(tcp_sk(sk)) && mptcp_meta_sk(sk) == sk;
}

static inline int is_master_tp(const struct tcp_sock *tp)
{
	return !mptcp(tp) || (!tp->mptcp->slave_sk && !is_meta_tp(tp));
}

static inline void mptcp_init_mp_opt(struct mptcp_options_received *mopt)
{
	mopt->saw_mpc = 0;
	mopt->dss_csum = 0;
	mopt->drop_me = 0;

	mopt->is_mp_join = 0;
	mopt->join_ack = 0;

	mopt->saw_low_prio = 0;
	mopt->low_prio = 0;

	mopt->saw_add_addr = 0;
	mopt->more_add_addr = 0;

	mopt->saw_rem_addr = 0;
	mopt->more_rem_addr = 0;

	mopt->mp_fail = 0;
	mopt->mp_fclose = 0;
}

static inline void mptcp_reset_mopt(struct tcp_sock *tp)
{
	struct mptcp_options_received *mopt = &tp->mptcp->rx_opt;

	mopt->saw_low_prio = 0;
	mopt->saw_add_addr = 0;
	mopt->more_add_addr = 0;
	mopt->saw_rem_addr = 0;
	mopt->more_rem_addr = 0;
	mopt->join_ack = 0;
	mopt->mp_fail = 0;
	mopt->mp_fclose = 0;
}

static inline __be32 mptcp_get_highorder_sndbits(const struct sk_buff *skb,
						 const struct mptcp_cb *mpcb)
{
	return htonl(mpcb->snd_high_order[(TCP_SKB_CB(skb)->mptcp_flags &
			MPTCPHDR_SEQ64_INDEX) ? 1 : 0]);
}

static inline u64 mptcp_get_data_seq_64(const struct mptcp_cb *mpcb, int index,
					u32 data_seq_32)
{
	return ((u64)mpcb->rcv_high_order[index] << 32) | data_seq_32;
}

static inline u64 mptcp_get_rcv_nxt_64(const struct tcp_sock *meta_tp)
{
	struct mptcp_cb *mpcb = meta_tp->mpcb;
	return mptcp_get_data_seq_64(mpcb, mpcb->rcv_hiseq_index,
				     meta_tp->rcv_nxt);
}

static inline void mptcp_check_sndseq_wrap(struct tcp_sock *meta_tp, int inc)
{
	if (unlikely(meta_tp->snd_nxt > meta_tp->snd_nxt + inc)) {
		struct mptcp_cb *mpcb = meta_tp->mpcb;
		mpcb->snd_hiseq_index = mpcb->snd_hiseq_index ? 0 : 1;
		mpcb->snd_high_order[mpcb->snd_hiseq_index] += 2;
	}
}

static inline void mptcp_check_rcvseq_wrap(struct tcp_sock *meta_tp,
					   u32 old_rcv_nxt)
{
	if (unlikely(old_rcv_nxt > meta_tp->rcv_nxt)) {
		struct mptcp_cb *mpcb = meta_tp->mpcb;
		mpcb->rcv_high_order[mpcb->rcv_hiseq_index] += 2;
		mpcb->rcv_hiseq_index = mpcb->rcv_hiseq_index ? 0 : 1;
	}
}

static inline int mptcp_sk_can_send(const struct sock *sk)
{
	return tcp_passive_fastopen(sk) ||
	       ((1 << sk->sk_state) & (TCPF_ESTABLISHED | TCPF_CLOSE_WAIT) &&
		!tcp_sk(sk)->mptcp->pre_established);
}

static inline int mptcp_sk_can_recv(const struct sock *sk)
{
	return (1 << sk->sk_state) & (TCPF_ESTABLISHED | TCPF_FIN_WAIT1 | TCPF_FIN_WAIT2);
}

static inline int mptcp_sk_can_send_ack(const struct sock *sk)
{
	return !((1 << sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV |
					TCPF_CLOSE | TCPF_LISTEN)) &&
	       !tcp_sk(sk)->mptcp->pre_established;
}

static inline bool mptcp_can_sg(const struct sock *meta_sk)
{
	struct mptcp_tcp_sock *mptcp;

	if (tcp_sk(meta_sk)->mpcb->dss_csum)
		return false;

	mptcp_for_each_sub(tcp_sk(meta_sk)->mpcb, mptcp) {
		struct sock *sk = mptcp_to_sock(mptcp);

		if (!mptcp_sk_can_send(sk))
			continue;
		if (!(sk->sk_route_caps & NETIF_F_SG))
			return false;
	}
	return true;
}

static inline void mptcp_set_rto(struct sock *sk)
{
	struct inet_connection_sock *micsk = inet_csk(mptcp_meta_sk(sk));
	struct tcp_sock *tp = tcp_sk(sk);
	struct mptcp_tcp_sock *mptcp;
	__u32 max_rto = 0;

	/* We are in recovery-phase on the MPTCP-level. Do not update the
	 * RTO, because this would kill exponential backoff.
	 */
	if (micsk->icsk_retransmits)
		return;

	mptcp_for_each_sub(tp->mpcb, mptcp) {
		struct sock *sk_it = mptcp_to_sock(mptcp);

		if ((mptcp_sk_can_send(sk_it) || sk_it->sk_state == TCP_SYN_RECV) &&
		    inet_csk(sk_it)->icsk_retransmits == 0 &&
		    inet_csk(sk_it)->icsk_backoff == 0 &&
		    inet_csk(sk_it)->icsk_rto > max_rto)
			max_rto = inet_csk(sk_it)->icsk_rto;
	}
	if (max_rto) {
		micsk->icsk_rto = max_rto << 1;

		/* A successfull rto-measurement - reset backoff counter */
		micsk->icsk_backoff = 0;
	}
}

static inline void mptcp_sub_close_passive(struct sock *sk)
{
	struct sock *meta_sk = mptcp_meta_sk(sk);
	struct tcp_sock *tp = tcp_sk(sk), *meta_tp = tcp_sk(meta_sk);

	/* Only close, if the app did a send-shutdown (passive close), and we
	 * received the data-ack of the data-fin.
	 */
	if (tp->mpcb->passive_close && meta_tp->snd_una == meta_tp->write_seq)
		mptcp_sub_close(sk, 0);
}

/* Returns true if all subflows were closed */
static inline bool mptcp_fallback_close(struct mptcp_cb *mpcb,
					struct sock *except)
{
	/* It can happen that the meta is already closed. In that case, don't
	 * keep the subflow alive - close everything!
	 */
	if (mpcb->meta_sk->sk_state == TCP_CLOSE)
		except = NULL;

	mptcp_sub_force_close_all(mpcb, except);

	if (mpcb->pm_ops->close_session)
		mpcb->pm_ops->close_session(mptcp_meta_sk(except));

	return !except;
}

static inline bool mptcp_v6_is_v4_mapped(const struct sock *sk)
{
	return sk->sk_family == AF_INET6 &&
	       ipv6_addr_type(&inet6_sk(sk)->saddr) == IPV6_ADDR_MAPPED;
}

/* We are in or are becoming to be in infinite mapping mode */
static inline bool mptcp_in_infinite_mapping_weak(const struct mptcp_cb *mpcb)
{
	return mpcb->infinite_mapping_rcv ||
	       mpcb->infinite_mapping_snd ||
	       mpcb->send_infinite_mapping;
}

static inline bool mptcp_can_new_subflow(const struct sock *meta_sk)
{
	/* Has been removed from the tk-table. Thus, no new subflows.
	 *
	 * Check for close-state is necessary, because we may have been closed
	 * without passing by mptcp_close().
	 *
	 * When falling back, no new subflows are allowed either.
	 */
	return meta_sk->sk_state != TCP_CLOSE &&
	       tcp_sk(meta_sk)->inside_tk_table &&
	       !tcp_sk(meta_sk)->mpcb->infinite_mapping_rcv &&
	       !tcp_sk(meta_sk)->mpcb->send_infinite_mapping;
}

static inline int mptcp_subflow_count(const struct mptcp_cb *mpcb)
{
	struct mptcp_tcp_sock *mptcp;
	int i = 0;

	mptcp_for_each_sub(mpcb, mptcp)
		i++;

	return i;
}

/* TCP and MPTCP mpc flag-depending functions */
u16 mptcp_select_window(struct sock *sk);
void mptcp_tcp_set_rto(struct sock *sk);

#else /* CONFIG_MPTCP */
#define mptcp_debug(fmt, args...)	\
	do {				\
	} while (0)

static inline struct sock *mptcp_to_sock(const struct mptcp_tcp_sock *mptcp)
{
	return NULL;
}

#define mptcp_for_each_sub(__mpcb, __mptcp)					\
	if (0)

#define MPTCP_INC_STATS(net, field)	\
	do {				\
	} while(0)

#define MPTCP_DEC_STATS(net, field)	\
	do {				\
	} while(0)

static inline bool mptcp_is_data_fin(const struct sk_buff *skb)
{
	return false;
}
static inline bool mptcp_is_data_seq(const struct sk_buff *skb)
{
	return false;
}
static inline struct sock *mptcp_meta_sk(const struct sock *sk)
{
	return NULL;
}
static inline struct tcp_sock *mptcp_meta_tp(const struct tcp_sock *tp)
{
	return NULL;
}
static inline int is_meta_sk(const struct sock *sk)
{
	return 0;
}
static inline int is_master_tp(const struct tcp_sock *tp)
{
	return 0;
}
static inline void mptcp_del_sock(const struct sock *sk) {}
static inline void mptcp_update_metasocket(const struct sock *meta_sk) {}
static inline void mptcp_reinject_data(struct sock *orig_sk, int clone_it) {}
static inline void mptcp_update_sndbuf(const struct tcp_sock *tp) {}
static inline void mptcp_sub_close(struct sock *sk, unsigned long delay) {}
static inline int mptcp_getsockopt(struct sock *meta_sk, int level, int optname,
				   char __user *optval, int __user *optlen)
{
	return -EOPNOTSUPP;
}
static inline void mptcp_set_rto(const struct sock *sk) {}
static inline void mptcp_send_fin(const struct sock *meta_sk) {}
static inline void mptcp_parse_options(const uint8_t *ptr, const int opsize,
				       struct mptcp_options_received *mopt,
				       const struct sk_buff *skb,
				       const struct tcp_sock *tp) {}
static inline void mptcp_syn_options(const struct sock *sk,
				     struct tcp_out_options *opts,
				     unsigned *remaining) {}
static inline void mptcp_synack_options(struct request_sock *req,
					struct tcp_out_options *opts,
					unsigned *remaining) {}

static inline void mptcp_established_options(struct sock *sk,
					     struct sk_buff *skb,
					     struct tcp_out_options *opts,
					     unsigned *size) {}
static inline void mptcp_options_write(__be32 *ptr, struct tcp_sock *tp,
				       const struct tcp_out_options *opts,
				       struct sk_buff *skb) {}
static inline void mptcp_close(struct sock *meta_sk, long timeout) {}
static inline bool mptcp_doit(struct sock *sk)
{
	return false;
}
static inline int mptcp_check_req_fastopen(struct sock *child,
					   struct request_sock *req)
{
	return 1;
}
static inline int mptcp_check_req_master(const struct sock *sk,
					 const struct sock *child,
					 const struct request_sock *req,
					 const struct sk_buff *skb,
					 const struct mptcp_options_received *mopt,
					 int drop,
					 u32 tsoff)
{
	return 1;
}
static inline struct sock *mptcp_check_req_child(const struct sock *meta_sk,
						 const struct sock *child,
						 const struct request_sock *req,
						 struct sk_buff *skb,
						 const struct mptcp_options_received *mopt)
{
	return NULL;
}
static inline unsigned int mptcp_current_mss(struct sock *meta_sk)
{
	return 0;
}
static inline void mptcp_sub_close_passive(struct sock *sk) {}
static inline bool mptcp_handle_ack_in_infinite(const struct sock *sk,
						const struct sk_buff *skb,
						int flag)
{
	return false;
}
static inline void mptcp_init_mp_opt(const struct mptcp_options_received *mopt) {}
static inline void mptcp_prepare_for_backlog(struct sock *sk, struct sk_buff *skb) {}
static inline bool mptcp_check_rtt(const struct tcp_sock *tp, int time)
{
	return false;
}
static inline int mptcp_check_snd_buf(const struct tcp_sock *tp)
{
	return 0;
}
static inline void mptcp_push_pending_frames(struct sock *meta_sk) {}
static inline void mptcp_send_reset(const struct sock *sk) {}
static inline void mptcp_sub_force_close_all(struct mptcp_cb *mpcb,
					     struct sock *except) {}
static inline bool mptcp_handle_options(struct sock *sk,
					const struct tcphdr *th,
					struct sk_buff *skb)
{
	return false;
}
static inline void mptcp_reset_mopt(struct tcp_sock *tp) {}
static inline void  __init mptcp_init(void) {}
static inline bool mptcp_can_sg(const struct sock *meta_sk)
{
	return false;
}
static inline unsigned int mptcp_xmit_size_goal(const struct sock *meta_sk,
						u32 mss_now, int large_allowed)
{
	return 0;
}
static inline void mptcp_destroy_sock(struct sock *sk) {}
static inline int mptcp_rcv_synsent_state_process(struct sock *sk,
						  struct sock **skptr,
						  struct sk_buff *skb,
						  const struct mptcp_options_received *mopt)
{
	return 0;
}
static inline bool mptcp_can_sendpage(struct sock *sk)
{
	return false;
}
static inline int mptcp_init_tw_sock(struct sock *sk,
				     struct tcp_timewait_sock *tw)
{
	return 0;
}
static inline void mptcp_twsk_destructor(struct tcp_timewait_sock *tw) {}
static inline void mptcp_disconnect(struct sock *meta_sk) {}
static inline void mptcp_tsq_flags(struct sock *sk) {}
static inline void mptcp_tsq_sub_deferred(struct sock *meta_sk) {}
static inline void mptcp_hash_remove_bh(struct tcp_sock *meta_tp) {}
static inline void mptcp_remove_shortcuts(const struct mptcp_cb *mpcb,
					  const struct sk_buff *skb) {}
static inline void mptcp_init_tcp_sock(struct sock *sk) {}
static inline void mptcp_init_listen(struct sock *sk) {}
static inline void mptcp_init_connect(struct sock *sk) {}
static inline void mptcp_disable_static_key(void) {}
static inline void mptcp_cookies_reqsk_init(struct request_sock *req,
					    struct mptcp_options_received *mopt,
					    struct sk_buff *skb) {}
static inline void mptcp_mpcb_put(struct mptcp_cb *mpcb) {}
static inline void mptcp_fin(struct sock *meta_sk) {}
static inline bool mptcp_in_infinite_mapping_weak(const struct mptcp_cb *mpcb)
{
	return false;
}
static inline bool mptcp_can_new_subflow(const struct sock *meta_sk)
{
	return false;
}

#endif /* CONFIG_MPTCP */

#endif /* _MPTCP_H */
