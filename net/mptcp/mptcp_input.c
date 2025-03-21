/*
 *	MPTCP implementation - Sending side
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

#include <asm/unaligned.h>

#include <net/mptcp.h>
#include <net/mptcp_v4.h>
#include <net/mptcp_v6.h>

#include <linux/kconfig.h>

/* is seq1 < seq2 ? */
static inline bool before64(const u64 seq1, const u64 seq2)
{
	return (s64)(seq1 - seq2) < 0;
}

/* is seq1 > seq2 ? */
#define after64(seq1, seq2)	before64(seq2, seq1)

static inline void mptcp_become_fully_estab(struct sock *sk)
{
	tcp_sk(sk)->mptcp->fully_established = 1;

	if (is_master_tp(tcp_sk(sk)) &&
	    tcp_sk(sk)->mpcb->pm_ops->fully_established)
		tcp_sk(sk)->mpcb->pm_ops->fully_established(mptcp_meta_sk(sk));
}

/* Similar to tcp_tso_acked without any memory accounting */
static inline int mptcp_tso_acked_reinject(const struct sock *meta_sk,
					   struct sk_buff *skb)
{
	const struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	u32 packets_acked, len, delta_truesize;

	BUG_ON(!after(TCP_SKB_CB(skb)->end_seq, meta_tp->snd_una));

	packets_acked = tcp_skb_pcount(skb);

	if (skb_unclone(skb, GFP_ATOMIC))
		return 0;

	len = meta_tp->snd_una - TCP_SKB_CB(skb)->seq;
	delta_truesize = __pskb_trim_head(skb, len);

	TCP_SKB_CB(skb)->seq += len;
	skb->ip_summed = CHECKSUM_PARTIAL;

	if (delta_truesize)
		skb->truesize -= delta_truesize;

	/* Any change of skb->len requires recalculation of tso factor. */
	if (tcp_skb_pcount(skb) > 1)
		tcp_set_skb_tso_segs(skb, tcp_skb_mss(skb));
	packets_acked -= tcp_skb_pcount(skb);

	if (packets_acked) {
		BUG_ON(tcp_skb_pcount(skb) == 0);
		BUG_ON(!before(TCP_SKB_CB(skb)->seq, TCP_SKB_CB(skb)->end_seq));
	}

	return packets_acked;
}

/* Cleans the meta-socket retransmission queue and the reinject-queue. */
static void mptcp_clean_rtx_queue(struct sock *meta_sk, u32 prior_snd_una)
{
	struct sk_buff *skb, *tmp, *next;
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct mptcp_cb *mpcb = meta_tp->mpcb;
	bool fully_acked = true;
	bool acked = false;
	u32 acked_pcount;

	for (skb = skb_rb_first(&meta_sk->tcp_rtx_queue); skb; skb = next) {
		struct tcp_skb_cb *scb = TCP_SKB_CB(skb);

		tcp_ack_tstamp(meta_sk, skb, prior_snd_una);

		if (after(scb->end_seq, meta_tp->snd_una)) {
			if (tcp_skb_pcount(skb) == 1 ||
			    !after(meta_tp->snd_una, scb->seq))
				break;

			acked_pcount = tcp_tso_acked(meta_sk, skb);
			if (!acked_pcount)
				break;
			fully_acked = false;
		} else {
			acked_pcount = tcp_skb_pcount(skb);
		}

		acked = true;
		meta_tp->packets_out -= acked_pcount;
		meta_tp->retrans_stamp = 0;

		if (!fully_acked)
			break;

		next = skb_rb_next(skb);

		if (mptcp_is_data_fin(skb)) {
			struct mptcp_tcp_sock *mptcp;
			struct hlist_node *tmp;

			/* DATA_FIN has been acknowledged - now we can close
			 * the subflows
			 */
			mptcp_for_each_sub_safe(mpcb, mptcp, tmp) {
				struct sock *sk_it = mptcp_to_sock(mptcp);
				unsigned long delay = 0;

				/* If we are the passive closer, don't trigger
				 * subflow-fin until the subflow has been finned
				 * by the peer - thus we add a delay.
				 */
				if (mpcb->passive_close &&
				    sk_it->sk_state == TCP_ESTABLISHED)
					delay = inet_csk(sk_it)->icsk_rto << 3;

				mptcp_sub_close(sk_it, delay);
			}
		}
		tcp_rtx_queue_unlink_and_free(skb, meta_sk);
	}
	/* Remove acknowledged data from the reinject queue */
	skb_queue_walk_safe(&mpcb->reinject_queue, skb, tmp) {
		if (before(meta_tp->snd_una, TCP_SKB_CB(skb)->end_seq)) {
			if (tcp_skb_pcount(skb) == 1 ||
			    !after(meta_tp->snd_una, TCP_SKB_CB(skb)->seq))
				break;

			mptcp_tso_acked_reinject(meta_sk, skb);
			break;
		}

		__skb_unlink(skb, &mpcb->reinject_queue);
		__kfree_skb(skb);
	}

	if (likely(between(meta_tp->snd_up, prior_snd_una, meta_tp->snd_una)))
		meta_tp->snd_up = meta_tp->snd_una;

	if (acked) {
		tcp_rearm_rto(meta_sk);
		/* Normally this is done in tcp_try_undo_loss - but MPTCP
		 * does not call this function.
		 */
		inet_csk(meta_sk)->icsk_retransmits = 0;
	}
}

/* Inspired by tcp_rcv_state_process */
/* Returns 0 if processing the packet can continue
 *	   -1 if connection was closed with an active reset
 *	   1 if connection was closed and processing should stop.
 */
static int mptcp_rcv_state_process(struct sock *meta_sk, struct sock *sk,
				   const struct sk_buff *skb, u32 data_seq,
				   u16 data_len)
{
	struct tcp_sock *meta_tp = tcp_sk(meta_sk), *tp = tcp_sk(sk);
	const struct tcphdr *th = tcp_hdr(skb);

	/* State-machine handling if FIN has been enqueued and he has
	 * been acked (snd_una == write_seq) - it's important that this
	 * here is after sk_wmem_free_skb because otherwise
	 * sk_forward_alloc is wrong upon inet_csk_destroy_sock()
	 */
	switch (meta_sk->sk_state) {
	case TCP_FIN_WAIT1: {
		struct dst_entry *dst;
		int tmo;

		if (meta_tp->snd_una != meta_tp->write_seq)
			break;

		tcp_set_state(meta_sk, TCP_FIN_WAIT2);
		meta_sk->sk_shutdown |= SEND_SHUTDOWN;

		dst = __sk_dst_get(sk);
		if (dst)
			dst_confirm(dst);

		if (!sock_flag(meta_sk, SOCK_DEAD)) {
			/* Wake up lingering close() */
			meta_sk->sk_state_change(meta_sk);
			break;
		}

		if (meta_tp->linger2 < 0 ||
		    (data_len &&
		     after(data_seq + data_len - (mptcp_is_data_fin2(skb, tp) ? 1 : 0),
			   meta_tp->rcv_nxt))) {
			mptcp_send_active_reset(meta_sk, GFP_ATOMIC);
			tcp_done(meta_sk);
			NET_INC_STATS(sock_net(meta_sk), LINUX_MIB_TCPABORTONDATA);
			return -1;
		}

		tmo = tcp_fin_time(meta_sk);
		if (tmo > TCP_TIMEWAIT_LEN) {
			inet_csk_reset_keepalive_timer(meta_sk, tmo - TCP_TIMEWAIT_LEN);
		} else if (mptcp_is_data_fin2(skb, tp) || sock_owned_by_user(meta_sk)) {
			/* Bad case. We could lose such FIN otherwise.
			 * It is not a big problem, but it looks confusing
			 * and not so rare event. We still can lose it now,
			 * if it spins in bh_lock_sock(), but it is really
			 * marginal case.
			 */
			inet_csk_reset_keepalive_timer(meta_sk, tmo);
		} else {
			meta_tp->ops->time_wait(meta_sk, TCP_FIN_WAIT2, tmo);
		}
		break;
	}
	case TCP_CLOSING:
	case TCP_LAST_ACK:
		if (meta_tp->snd_una == meta_tp->write_seq) {
			tcp_done(meta_sk);
			return 1;
		}
		break;
	}

	/* step 7: process the segment text */
	switch (meta_sk->sk_state) {
	case TCP_FIN_WAIT1:
	case TCP_FIN_WAIT2:
		/* RFC 793 says to queue data in these states,
		 * RFC 1122 says we MUST send a reset.
		 * BSD 4.4 also does reset.
		 */
		if (meta_sk->sk_shutdown & RCV_SHUTDOWN) {
			if (TCP_SKB_CB(skb)->end_seq != TCP_SKB_CB(skb)->seq &&
			    after(TCP_SKB_CB(skb)->end_seq - th->fin, tp->rcv_nxt) &&
			    !mptcp_is_data_fin2(skb, tp)) {
				NET_INC_STATS(sock_net(meta_sk), LINUX_MIB_TCPABORTONDATA);
				mptcp_send_active_reset(meta_sk, GFP_ATOMIC);
				tcp_reset(meta_sk);
				return -1;
			}
		}
		break;
	}

	return 0;
}

/**
 * @return:
 *  i) 1: Everything's fine.
 *  ii) -1: A reset has been sent on the subflow - csum-failure
 *  iii) 0: csum-failure but no reset sent, because it's the last subflow.
 *	 Last packet should not be destroyed by the caller because it has
 *	 been done here.
 */
static int mptcp_verif_dss_csum(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *tmp, *tmp1, *last = NULL;
	__wsum csum_tcp = 0; /* cumulative checksum of pld + mptcp-header */
	int ans = 1, overflowed = 0, offset = 0, dss_csum_added = 0;
	int iter = 0;
	u32 next_seq, offset_seq;

	skb_queue_walk_safe(&sk->sk_receive_queue, tmp, tmp1) {
		unsigned int csum_len;

		/* init next seq in first round  */
		if (!iter)
			next_seq = TCP_SKB_CB(tmp)->seq;
		offset_seq = next_seq - TCP_SKB_CB(tmp)->seq;

		if (before(tp->mptcp->map_subseq + tp->mptcp->map_data_len, TCP_SKB_CB(tmp)->end_seq))
			/* Mapping ends in the middle of the packet -
			 * csum only these bytes
			 */
			csum_len = tp->mptcp->map_subseq + tp->mptcp->map_data_len - TCP_SKB_CB(tmp)->seq;
		else
			csum_len = tmp->len;

		csum_len -= offset_seq;
		offset = 0;
		if (overflowed) {
			char first_word[4];
			first_word[0] = 0;
			first_word[1] = 0;
			first_word[2] = 0;
			first_word[3] = *(tmp->data + offset_seq);
			csum_tcp = csum_partial(first_word, 4, csum_tcp);
			offset = 1;
			csum_len--;
			overflowed = 0;
		}

		csum_tcp = skb_checksum(tmp, offset + offset_seq, csum_len,
					csum_tcp);

		/* Was it on an odd-length? Then we have to merge the next byte
		 * correctly (see above)
		 */
		if (csum_len != (csum_len & (~1)))
			overflowed = 1;

		if (mptcp_is_data_seq(tmp) && !dss_csum_added) {
			__be32 data_seq = htonl((u32)(tp->mptcp->map_data_seq >> 32));

			/* If a 64-bit dss is present, we increase the offset
			 * by 4 bytes, as the high-order 64-bits will be added
			 * in the final csum_partial-call.
			 */
			u32 offset = skb_transport_offset(tmp) +
				     TCP_SKB_CB(tmp)->dss_off;
			if (TCP_SKB_CB(tmp)->mptcp_flags & MPTCPHDR_SEQ64_SET)
				offset += 4;

			csum_tcp = skb_checksum(tmp, offset,
						MPTCP_SUB_LEN_SEQ_CSUM,
						csum_tcp);

			csum_tcp = csum_partial(&data_seq,
						sizeof(data_seq), csum_tcp);

			dss_csum_added = 1; /* Just do it once */
		} else if (mptcp_is_data_mpcapable(tmp) && !dss_csum_added) {
			u32 offset = skb_transport_offset(tmp) + TCP_SKB_CB(tmp)->dss_off;
			__be64 data_seq = htonll(tp->mptcp->map_data_seq);
			__be32 rel_seq = htonl(tp->mptcp->map_subseq - tp->mptcp->rcv_isn);

			csum_tcp = csum_partial(&data_seq, sizeof(data_seq), csum_tcp);
			csum_tcp = csum_partial(&rel_seq, sizeof(rel_seq), csum_tcp);

			csum_tcp = skb_checksum(tmp, offset, 4, csum_tcp);

			dss_csum_added = 1;
		}
		last = tmp;
		iter++;

		if (!skb_queue_is_last(&sk->sk_receive_queue, tmp) &&
		    !before(TCP_SKB_CB(tmp1)->seq,
			    tp->mptcp->map_subseq + tp->mptcp->map_data_len))
			break;
		next_seq = TCP_SKB_CB(tmp)->end_seq;
	}

	/* Now, checksum must be 0 */
	if (unlikely(csum_fold(csum_tcp))) {
		struct mptcp_tcp_sock *mptcp;
		struct sock *sk_it = NULL;

		pr_debug("%s csum is wrong: %#x tcp-seq %u dss_csum_added %d overflowed %d iterations %d\n",
			 __func__, csum_fold(csum_tcp), TCP_SKB_CB(last)->seq,
			 dss_csum_added, overflowed, iter);

		MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_CSUMFAIL);
		tp->mptcp->send_mp_fail = 1;

		/* map_data_seq is the data-seq number of the
		 * mapping we are currently checking
		 */
		tp->mpcb->csum_cutoff_seq = tp->mptcp->map_data_seq;

		/* Search for another subflow that is fully established */
		mptcp_for_each_sub(tp->mpcb, mptcp) {
			sk_it = mptcp_to_sock(mptcp);

			if (sk_it != sk &&
			    tcp_sk(sk_it)->mptcp->fully_established)
				break;

			sk_it = NULL;
		}

		if (sk_it) {
			mptcp_send_reset(sk);
			ans = -1;
		} else {
			tp->mpcb->send_infinite_mapping = 1;

			/* Need to purge the rcv-queue as it's no more valid */
			while ((tmp = __skb_dequeue(&sk->sk_receive_queue)) != NULL) {
				tp->copied_seq = TCP_SKB_CB(tmp)->end_seq;
				kfree_skb(tmp);
			}

			if (mptcp_fallback_close(tp->mpcb, sk))
				ans = -1;
			else
				ans = 0;
		}
	}

	return ans;
}

static inline void mptcp_prepare_skb(struct sk_buff *skb,
				     const struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct tcp_skb_cb *tcb = TCP_SKB_CB(skb);
	u32 inc = 0, end_seq = tcb->end_seq;

	if (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN)
		end_seq--;
	/* If skb is the end of this mapping (end is always at mapping-boundary
	 * thanks to the splitting/trimming), then we need to increase
	 * data-end-seq by 1 if this here is a data-fin.
	 *
	 * We need to do -1 because end_seq includes the subflow-FIN.
	 */
	if (tp->mptcp->map_data_fin &&
	    end_seq == tp->mptcp->map_subseq + tp->mptcp->map_data_len) {
		inc = 1;

		/* We manually set the fin-flag if it is a data-fin. For easy
		 * processing in tcp_recvmsg.
		 */
		TCP_SKB_CB(skb)->tcp_flags |= TCPHDR_FIN;
	} else {
		/* We may have a subflow-fin with data but without data-fin */
		TCP_SKB_CB(skb)->tcp_flags &= ~TCPHDR_FIN;
	}

	/* Adapt data-seq's to the packet itself. We kinda transform the
	 * dss-mapping to a per-packet granularity. This is necessary to
	 * correctly handle overlapping mappings coming from different
	 * subflows. Otherwise it would be a complete mess.
	 */
	tcb->seq = ((u32)tp->mptcp->map_data_seq) + tcb->seq - tp->mptcp->map_subseq;
	tcb->end_seq = tcb->seq + skb->len + inc;
}

static inline void mptcp_reset_mapping(struct tcp_sock *tp, u32 old_copied_seq)
{
	tp->mptcp->map_data_len = 0;
	tp->mptcp->map_data_seq = 0;
	tp->mptcp->map_subseq = 0;
	tp->mptcp->map_data_fin = 0;
	tp->mptcp->mapping_present = 0;

	/* In infinite mapping receiver mode, we have to advance the implied
	 * data-sequence number when we progress the subflow's data.
	 */
	if (tp->mpcb->infinite_mapping_rcv)
		tp->mpcb->infinite_rcv_seq += (tp->copied_seq - old_copied_seq);
}

/* The DSS-mapping received on the sk only covers the second half of the skb
 * (cut at seq). We trim the head from the skb.
 * Data will be freed upon kfree().
 *
 * Inspired by tcp_trim_head().
 */
static void mptcp_skb_trim_head(struct sk_buff *skb, struct sock *sk, u32 seq)
{
	int len = seq - TCP_SKB_CB(skb)->seq;
	u32 new_seq = TCP_SKB_CB(skb)->seq + len;
	u32 delta_truesize;

	delta_truesize = __pskb_trim_head(skb, len);

	TCP_SKB_CB(skb)->seq = new_seq;

	if (delta_truesize) {
		skb->truesize -= delta_truesize;
		atomic_sub(delta_truesize, &sk->sk_rmem_alloc);
		sk_mem_uncharge(sk, delta_truesize);
	}
}

/* The DSS-mapping received on the sk only covers the first half of the skb
 * (cut at seq). We create a second skb (@return), and queue it in the rcv-queue
 * as further packets may resolve the mapping of the second half of data.
 *
 * Inspired by tcp_fragment().
 */
static int mptcp_skb_split_tail(struct sk_buff *skb, struct sock *sk, u32 seq)
{
	struct sk_buff *buff;
	int nsize;
	int nlen, len;
	u8 flags;

	len = seq - TCP_SKB_CB(skb)->seq;
	nsize = skb_headlen(skb) - len + tcp_sk(sk)->tcp_header_len;
	if (nsize < 0)
		nsize = 0;

	/* Get a new skb... force flag on. */
	buff = alloc_skb(nsize, GFP_ATOMIC);
	if (buff == NULL)
		return -ENOMEM;

	skb_reserve(buff, tcp_sk(sk)->tcp_header_len);
	skb_reset_transport_header(buff);

	flags = TCP_SKB_CB(skb)->tcp_flags;
	TCP_SKB_CB(skb)->tcp_flags = flags & ~(TCPHDR_FIN);
	TCP_SKB_CB(buff)->tcp_flags = flags;

	/* We absolutly need to call skb_set_owner_r before refreshing the
	 * truesize of buff, otherwise the moved data will account twice.
	 */
	skb_set_owner_r(buff, sk);
	nlen = skb->len - len - nsize;
	buff->truesize += nlen;
	skb->truesize -= nlen;

	/* Correct the sequence numbers. */
	TCP_SKB_CB(buff)->seq = TCP_SKB_CB(skb)->seq + len;
	TCP_SKB_CB(buff)->end_seq = TCP_SKB_CB(skb)->end_seq;
	TCP_SKB_CB(skb)->end_seq = TCP_SKB_CB(buff)->seq;

	skb_split(skb, buff, len);

	__skb_queue_after(&sk->sk_receive_queue, skb, buff);

	return 0;
}

/* @return: 0  everything is fine. Just continue processing
 *	    1  subflow is broken stop everything
 *	    -1 this packet was broken - continue with the next one.
 */
static int mptcp_prevalidate_skb(struct sock *sk, struct sk_buff *skb)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct mptcp_cb *mpcb = tp->mpcb;

	/* If we are in infinite mode, the subflow-fin is in fact a data-fin. */
	if (!skb->len && (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN) &&
	    !mptcp_is_data_fin(skb) && !mpcb->infinite_mapping_rcv) {
		/* Remove a pure subflow-fin from the queue and increase
		 * copied_seq.
		 */
		tp->copied_seq = TCP_SKB_CB(skb)->end_seq;
		__skb_unlink(skb, &sk->sk_receive_queue);
		__kfree_skb(skb);
		return -1;
	}

	/* If we are not yet fully established and do not know the mapping for
	 * this segment, this path has to fallback to infinite or be torn down.
	 */
	if (!tp->mptcp->fully_established && !mptcp_is_data_seq(skb) &&
	    !mptcp_is_data_mpcapable(skb) &&
	    !tp->mptcp->mapping_present && !mpcb->infinite_mapping_rcv) {
		pr_debug("%s %#x will fallback - pi %d from %pS, seq %u mptcp-flags %#x\n",
			 __func__, mpcb->mptcp_loc_token,
			 tp->mptcp->path_index, __builtin_return_address(0),
			 TCP_SKB_CB(skb)->seq, TCP_SKB_CB(skb)->mptcp_flags);

		if (!is_master_tp(tp)) {
			MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_FBDATASUB);
			mptcp_send_reset(sk);
			return 1;
		}

		MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_FBDATAINIT);

		mpcb->infinite_mapping_snd = 1;
		mpcb->infinite_mapping_rcv = 1;
		mpcb->infinite_rcv_seq = mptcp_get_rcv_nxt_64(mptcp_meta_tp(tp));

		if (mptcp_fallback_close(mpcb, sk))
			return 1;

		/* We do a seamless fallback and should not send a inf.mapping. */
		mpcb->send_infinite_mapping = 0;
		tp->mptcp->fully_established = 1;
	}

	/* Receiver-side becomes fully established when a whole rcv-window has
	 * been received without the need to fallback due to the previous
	 * condition.
	 */
	if (!tp->mptcp->fully_established) {
		tp->mptcp->init_rcv_wnd -= skb->len;
		if (tp->mptcp->init_rcv_wnd < 0)
			mptcp_become_fully_estab(sk);
	}

	return 0;
}

static void mptcp_restart_sending(struct sock *meta_sk, uint32_t in_flight_seq)
{
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct mptcp_cb *mpcb = meta_tp->mpcb;
	struct sk_buff *wq_head, *skb, *tmp;

	skb = tcp_rtx_queue_head(meta_sk);

	/* We resend everything that has not been acknowledged and is not in-flight,
	 * thus we need to move it from the rtx-tree to the write-queue.
	 */
	wq_head = tcp_write_queue_head(meta_sk);

	/* We artificially restart parts of the send-queue. Thus,
	 * it is as if no packets are in flight, minus the one that are.
	 */
	meta_tp->packets_out = 0;

	skb_rbtree_walk_from_safe(skb, tmp) {
		if (!after(TCP_SKB_CB(skb)->end_seq, in_flight_seq)) {
			meta_tp->packets_out += tcp_skb_pcount(skb);
			continue;
		}

		list_del(&skb->tcp_tsorted_anchor);
		tcp_rtx_queue_unlink(skb, meta_sk);
		INIT_LIST_HEAD(&skb->tcp_tsorted_anchor);

		if (wq_head)
			__skb_queue_before(&meta_sk->sk_write_queue, wq_head, skb);
		else
			tcp_add_write_queue_tail(meta_sk, skb);
	}

	/* If the snd_nxt already wrapped around, we have to
	 * undo the wrapping, as we are restarting from in_flight_seq
	 * on.
	 */
	if (meta_tp->snd_nxt < in_flight_seq) {
		mpcb->snd_high_order[mpcb->snd_hiseq_index] -= 2;
		mpcb->snd_hiseq_index = mpcb->snd_hiseq_index ? 0 : 1;
	}
	meta_tp->snd_nxt = in_flight_seq;

	/* Trigger a sending on the meta. */
	mptcp_push_pending_frames(meta_sk);
}

/* @return: 0  everything is fine. Just continue processing
 *	    1  subflow is broken stop everything
 *	    -1 this packet was broken - continue with the next one.
 */
static int mptcp_detect_mapping(struct sock *sk, struct sk_buff *skb)
{
	struct tcp_sock *tp = tcp_sk(sk), *meta_tp = mptcp_meta_tp(tp);
	struct mptcp_cb *mpcb = tp->mpcb;
	struct tcp_skb_cb *tcb = TCP_SKB_CB(skb);
	u32 *ptr;
	u32 data_seq, sub_seq, data_len, tcp_end_seq;
	bool set_infinite_rcv = false;

	/* If we are in infinite-mapping-mode, the subflow is guaranteed to be
	 * in-order at the data-level. Thus data-seq-numbers can be inferred
	 * from what is expected at the data-level.
	 */
	if (mpcb->infinite_mapping_rcv) {
		/* copied_seq may be bigger than tcb->seq (e.g., when the peer
		 * retransmits data that actually has already been acknowledged with
		 * newer data, if he did not receive our acks). Thus, we need
		 * to account for this overlap as well.
		 */
		tp->mptcp->map_data_seq = mpcb->infinite_rcv_seq - (tp->copied_seq - tcb->seq);
		tp->mptcp->map_subseq = tcb->seq;
		tp->mptcp->map_data_len = skb->len;
		tp->mptcp->map_data_fin = !!(TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN);
		tp->mptcp->mapping_present = 1;
		return 0;
	}

	if (!tp->mptcp->mapping_present && mptcp_is_data_mpcapable(skb)) {
		__u32 *ptr = (__u32 *)(skb_transport_header(skb) + TCP_SKB_CB(skb)->dss_off);

		sub_seq = 1 + tp->mptcp->rcv_isn;
		data_seq = meta_tp->rcv_nxt;
		data_len = get_unaligned_be16(ptr);
	} else if (!mptcp_is_data_seq(skb)) {
		/* No mapping here?
		 * Exit - it is either already set or still on its way
		 */
		if (!tp->mptcp->mapping_present &&
		    tp->rcv_nxt - tp->copied_seq > 65536) {
			/* Too many packets without a mapping,
			 * this subflow is broken
			 */
			MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_NODSSWINDOW);
			mptcp_send_reset(sk);
			return 1;
		}

		return 0;
	} else {
		/* Well, then the DSS-mapping is there. So, read it! */
		ptr = mptcp_skb_set_data_seq(skb, &data_seq, mpcb);
		ptr++;
		sub_seq = get_unaligned_be32(ptr) + tp->mptcp->rcv_isn;
		ptr++;
		data_len = get_unaligned_be16(ptr);
	}

	/* If it's an empty skb with DATA_FIN, sub_seq must get fixed.
	 * The draft sets it to 0, but we really would like to have the
	 * real value, to have an easy handling afterwards here in this
	 * function.
	 */
	if (mptcp_is_data_fin(skb) && skb->len == 0)
		sub_seq = TCP_SKB_CB(skb)->seq;

	/* If there is already a mapping - we check if it maps with the current
	 * one. If not - we reset.
	 */
	if (tp->mptcp->mapping_present &&
	    (data_seq != (u32)tp->mptcp->map_data_seq ||
	     sub_seq != tp->mptcp->map_subseq ||
	     data_len != tp->mptcp->map_data_len + tp->mptcp->map_data_fin ||
	     mptcp_is_data_fin(skb) != tp->mptcp->map_data_fin)) {
		/* Mapping in packet is different from what we want */
		pr_debug("%s Mappings do not match!\n", __func__);
		pr_debug("%s dseq %u mdseq %u, sseq %u msseq %u dlen %u mdlen %u dfin %d mdfin %d\n",
			 __func__, data_seq, (u32)tp->mptcp->map_data_seq,
			 sub_seq, tp->mptcp->map_subseq, data_len,
			 tp->mptcp->map_data_len, mptcp_is_data_fin(skb),
			 tp->mptcp->map_data_fin);
		MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_DSSNOMATCH);
		mptcp_send_reset(sk);
		return 1;
	}

	/* If the previous check was good, the current mapping is valid and we exit. */
	if (tp->mptcp->mapping_present)
		return 0;

	/* Mapping not yet set on this subflow - we set it here! */

	if (!data_len) {
		mpcb->infinite_mapping_rcv = 1;
		mpcb->send_infinite_mapping = 1;
		tp->mptcp->fully_established = 1;
		/* We need to repeat mp_fail's until the sender felt
		 * back to infinite-mapping - here we stop repeating it.
		 */
		tp->mptcp->send_mp_fail = 0;

		/* We have to fixup data_len - it must be the same as skb->len */
		data_len = skb->len + (mptcp_is_data_fin(skb) ? 1 : 0);
		sub_seq = tcb->seq;

		if (mptcp_fallback_close(mpcb, sk))
			return 1;

		mptcp_restart_sending(tp->meta_sk, meta_tp->snd_una);

		/* data_seq and so on are set correctly */

		/* At this point, the meta-ofo-queue has to be emptied,
		 * as the following data is guaranteed to be in-order at
		 * the data and subflow-level
		 */
		skb_rbtree_purge(&meta_tp->out_of_order_queue);

		set_infinite_rcv = true;
		MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_INFINITEMAPRX);
	}

	/* We are sending mp-fail's and thus are in fallback mode.
	 * Ignore packets which do not announce the fallback and still
	 * want to provide a mapping.
	 */
	if (tp->mptcp->send_mp_fail) {
		tp->copied_seq = TCP_SKB_CB(skb)->end_seq;
		__skb_unlink(skb, &sk->sk_receive_queue);
		__kfree_skb(skb);
		return -1;
	}

	/* FIN increased the mapping-length by 1 */
	if (mptcp_is_data_fin(skb))
		data_len--;

	/* Subflow-sequences of packet must be
	 * (at least partially) be part of the DSS-mapping's
	 * subflow-sequence-space.
	 *
	 * Basically the mapping is not valid, if either of the
	 * following conditions is true:
	 *
	 * 1. It's not a data_fin and
	 *    MPTCP-sub_seq >= TCP-end_seq
	 *
	 * 2. It's a data_fin and TCP-end_seq > TCP-seq and
	 *    MPTCP-sub_seq >= TCP-end_seq
	 *
	 * The previous two can be merged into:
	 *    TCP-end_seq > TCP-seq and MPTCP-sub_seq >= TCP-end_seq
	 *    Because if it's not a data-fin, TCP-end_seq > TCP-seq
	 *
	 * 3. It's a data_fin and skb->len == 0 and
	 *    MPTCP-sub_seq > TCP-end_seq
	 *
	 * 4. It's not a data_fin and TCP-end_seq > TCP-seq and
	 *    MPTCP-sub_seq + MPTCP-data_len <= TCP-seq
	 */

	/* subflow-fin is not part of the mapping - ignore it here ! */
	tcp_end_seq = tcb->end_seq;
	if (tcb->tcp_flags & TCPHDR_FIN)
		tcp_end_seq--;
	if ((!before(sub_seq, tcb->end_seq) && after(tcp_end_seq, tcb->seq)) ||
	    (mptcp_is_data_fin(skb) && skb->len == 0 && after(sub_seq, tcb->end_seq)) ||
	    (!after(sub_seq + data_len, tcb->seq) && after(tcp_end_seq, tcb->seq))) {
		/* Subflow-sequences of packet is different from what is in the
		 * packet's dss-mapping. The peer is misbehaving - reset
		 */
		pr_debug("%s Packet's mapping does not map to the DSS sub_seq %u end_seq %u, tcp_end_seq %u seq %u dfin %u len %u data_len %u copied_seq %u\n",
			 __func__, sub_seq, tcb->end_seq, tcp_end_seq,
			 tcb->seq, mptcp_is_data_fin(skb),
			 skb->len, data_len, tp->copied_seq);
		MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_DSSTCPMISMATCH);
		mptcp_send_reset(sk);
		return 1;
	}

	/* Does the DSS had 64-bit seqnum's ? */
	if (!(tcb->mptcp_flags & MPTCPHDR_SEQ64_SET)) {
		/* Wrapped around? */
		if (unlikely(after(data_seq, meta_tp->rcv_nxt) && data_seq < meta_tp->rcv_nxt)) {
			tp->mptcp->map_data_seq = mptcp_get_data_seq_64(mpcb, !mpcb->rcv_hiseq_index, data_seq);
		} else {
			/* Else, access the default high-order bits */
			tp->mptcp->map_data_seq = mptcp_get_data_seq_64(mpcb, mpcb->rcv_hiseq_index, data_seq);
		}
	} else {
		tp->mptcp->map_data_seq = mptcp_get_data_seq_64(mpcb, (tcb->mptcp_flags & MPTCPHDR_SEQ64_INDEX) ? 1 : 0, data_seq);

		if (unlikely(tcb->mptcp_flags & MPTCPHDR_SEQ64_OFO)) {
			/* We make sure that the data_seq is invalid.
			 * It will be dropped later.
			 */
			tp->mptcp->map_data_seq += 0xFFFFFFFF;
			tp->mptcp->map_data_seq += 0xFFFFFFFF;
		}
	}

	if (set_infinite_rcv)
		mpcb->infinite_rcv_seq = tp->mptcp->map_data_seq;

	tp->mptcp->map_data_len = data_len;
	tp->mptcp->map_subseq = sub_seq;
	tp->mptcp->map_data_fin = mptcp_is_data_fin(skb) ? 1 : 0;
	tp->mptcp->mapping_present = 1;

	return 0;
}

/* Similar to tcp_sequence(...) */
static inline bool mptcp_sequence(const struct tcp_sock *meta_tp,
				 u64 data_seq, u64 end_data_seq)
{
	const struct mptcp_cb *mpcb = meta_tp->mpcb;
	u64 rcv_wup64;

	/* Wrap-around? */
	if (meta_tp->rcv_wup > meta_tp->rcv_nxt) {
		rcv_wup64 = ((u64)(mpcb->rcv_high_order[mpcb->rcv_hiseq_index] - 1) << 32) |
				meta_tp->rcv_wup;
	} else {
		rcv_wup64 = mptcp_get_data_seq_64(mpcb, mpcb->rcv_hiseq_index,
						  meta_tp->rcv_wup);
	}

	return	!before64(end_data_seq, rcv_wup64) &&
		!after64(data_seq, mptcp_get_rcv_nxt_64(meta_tp) + tcp_receive_window_now(meta_tp));
}

/* @return: 0  everything is fine. Just continue processing
 *	    -1 this packet was broken - continue with the next one.
 */
static int mptcp_validate_mapping(struct sock *sk, struct sk_buff *skb)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *tmp, *tmp1;
	u32 tcp_end_seq;

	if (!tp->mptcp->mapping_present)
		return 0;

	/* either, the new skb gave us the mapping and the first segment
	 * in the sub-rcv-queue has to be trimmed ...
	 */
	tmp = skb_peek(&sk->sk_receive_queue);
	if (before(TCP_SKB_CB(tmp)->seq, tp->mptcp->map_subseq) &&
	    after(TCP_SKB_CB(tmp)->end_seq, tp->mptcp->map_subseq)) {
		MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_DSSTRIMHEAD);
		mptcp_skb_trim_head(tmp, sk, tp->mptcp->map_subseq);
	}

	skb_queue_walk_from(&sk->sk_receive_queue, skb) {
		/* ... or the new skb (tail) has to be split at the end. */
		tcp_end_seq = TCP_SKB_CB(skb)->end_seq;
		if (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN)
			tcp_end_seq--;

		if (tcp_end_seq == tp->mptcp->map_subseq + tp->mptcp->map_data_len)
			break;

		if (after(tcp_end_seq, tp->mptcp->map_subseq + tp->mptcp->map_data_len)) {
			u32 seq = tp->mptcp->map_subseq + tp->mptcp->map_data_len;

			MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_DSSSPLITTAIL);
			if (mptcp_skb_split_tail(skb, sk, seq)) {
				if (net_ratelimit())
					pr_err("MPTCP: Could not allocate memory for mptcp_skb_split_tail on seq %u\n", seq);

				/* allocations are failing - there is not much to do.
				 * Let's try the best and trigger meta-rexmit on the
				 * sender-side by simply dropping all packets up to sk
				 * in the receive-queue.
				 */

				skb_queue_walk_safe(&sk->sk_receive_queue, tmp, tmp1) {
					tp->copied_seq = TCP_SKB_CB(tmp)->end_seq;
					__skb_unlink(tmp, &sk->sk_receive_queue);
					__kfree_skb(tmp);

					if (tmp == skb)
						break;
				}
			}

			/* We just split an skb in the receive-queue (or removed
			 * a whole bunch of them).
			 * We have to restart as otherwise the list-processing
			 * will fail - thus return -1.
			 */
			return -1;
		}
	}

	/* Now, remove old sk_buff's from the receive-queue.
	 * This may happen if the mapping has been lost for these segments and
	 * the next mapping has already been received.
	 */
	if (before(TCP_SKB_CB(skb_peek(&sk->sk_receive_queue))->seq, tp->mptcp->map_subseq)) {
		skb_queue_walk_safe(&sk->sk_receive_queue, tmp1, tmp) {
			if (!before(TCP_SKB_CB(tmp1)->seq, tp->mptcp->map_subseq))
				break;

			tp->copied_seq = TCP_SKB_CB(tmp1)->end_seq;
			__skb_unlink(tmp1, &sk->sk_receive_queue);

			MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_PURGEOLD);
			/* Impossible that we could free skb here, because his
			 * mapping is known to be valid from previous checks
			 */
			__kfree_skb(tmp1);
		}
	}

	return 0;
}

/* @return: 0  everything is fine. Just continue processing
 *	    1  subflow is broken stop everything
 *	    -1 this mapping has been put in the meta-receive-queue
 *	    -2 this mapping has been eaten by the application
 */
static int mptcp_queue_skb(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk), *meta_tp = mptcp_meta_tp(tp);
	struct sock *meta_sk = mptcp_meta_sk(sk);
	struct mptcp_cb *mpcb = tp->mpcb;
	struct sk_buff *tmp, *tmp1;
	u64 rcv_nxt64 = mptcp_get_rcv_nxt_64(meta_tp);
	u32 old_copied_seq = tp->copied_seq;
	bool data_queued = false;

	/* Have we not yet received the full mapping? */
	if (!tp->mptcp->mapping_present ||
	    before(tp->rcv_nxt, tp->mptcp->map_subseq + tp->mptcp->map_data_len))
		return 0;

	/* Is this an overlapping mapping? rcv_nxt >= end_data_seq
	 * OR
	 * This mapping is out of window
	 */
	if (!before64(rcv_nxt64, tp->mptcp->map_data_seq + tp->mptcp->map_data_len + tp->mptcp->map_data_fin) ||
	    !mptcp_sequence(meta_tp, tp->mptcp->map_data_seq,
			    tp->mptcp->map_data_seq + tp->mptcp->map_data_len + tp->mptcp->map_data_fin)) {
		skb_queue_walk_safe(&sk->sk_receive_queue, tmp1, tmp) {
			__skb_unlink(tmp1, &sk->sk_receive_queue);
			tp->copied_seq = TCP_SKB_CB(tmp1)->end_seq;
			__kfree_skb(tmp1);

			if (!skb_queue_empty(&sk->sk_receive_queue) &&
			    !before(TCP_SKB_CB(tmp)->seq,
				    tp->mptcp->map_subseq + tp->mptcp->map_data_len))
				break;
		}

		mptcp_reset_mapping(tp, old_copied_seq);

		return -1;
	}

	/* Record it, because we want to send our data_fin on the same path */
	if (tp->mptcp->map_data_fin) {
		mpcb->dfin_path_index = tp->mptcp->path_index;
		mpcb->dfin_combined = !!(sk->sk_shutdown & RCV_SHUTDOWN);
	}

	/* Verify the checksum */
	if (mpcb->dss_csum && !mpcb->infinite_mapping_rcv) {
		int ret = mptcp_verif_dss_csum(sk);

		if (ret <= 0) {
			mptcp_reset_mapping(tp, old_copied_seq);
			return 1;
		}
	}

	if (before64(rcv_nxt64, tp->mptcp->map_data_seq)) {
		/* Seg's have to go to the meta-ofo-queue */
		skb_queue_walk_safe(&sk->sk_receive_queue, tmp1, tmp) {
			tp->copied_seq = TCP_SKB_CB(tmp1)->end_seq;
			mptcp_prepare_skb(tmp1, sk);
			__skb_unlink(tmp1, &sk->sk_receive_queue);
			sk_forced_mem_schedule(meta_sk, tmp1->truesize);
			/* MUST be done here, because fragstolen may be true later.
			 * Then, kfree_skb_partial will not account the memory.
			 */
			skb_orphan(tmp1);

			if (!mpcb->in_time_wait) /* In time-wait, do not receive data */
				tcp_data_queue_ofo(meta_sk, tmp1);
			else
				__kfree_skb(tmp1);

			if (!skb_queue_empty(&sk->sk_receive_queue) &&
			    !before(TCP_SKB_CB(tmp)->seq,
				    tp->mptcp->map_subseq + tp->mptcp->map_data_len))
				break;
		}

		/* Quick ACK if more 3/4 of the receive window is filled */
		if (after64(tp->mptcp->map_data_seq,
			    rcv_nxt64 + 3 * (tcp_receive_window_now(meta_tp) >> 2)))
			tcp_enter_quickack_mode(sk, TCP_MAX_QUICKACKS);

	} else {
		/* Ready for the meta-rcv-queue */
		skb_queue_walk_safe(&sk->sk_receive_queue, tmp1, tmp) {
			int eaten = 0;
			bool fragstolen = false;
			u32 old_rcv_nxt = meta_tp->rcv_nxt;

			tp->copied_seq = TCP_SKB_CB(tmp1)->end_seq;
			mptcp_prepare_skb(tmp1, sk);
			__skb_unlink(tmp1, &sk->sk_receive_queue);
			sk_forced_mem_schedule(meta_sk, tmp1->truesize);
			/* MUST be done here, because fragstolen may be true.
			 * Then, kfree_skb_partial will not account the memory.
			 */
			skb_orphan(tmp1);

			/* This segment has already been received */
			if (!after(TCP_SKB_CB(tmp1)->end_seq, meta_tp->rcv_nxt)) {
				__kfree_skb(tmp1);
				goto next;
			}

			if (mpcb->in_time_wait) /* In time-wait, do not receive data */
				eaten = 1;

			if (!eaten)
				eaten = tcp_queue_rcv(meta_sk, tmp1, &fragstolen);

			meta_tp->rcv_nxt = TCP_SKB_CB(tmp1)->end_seq;

			if (TCP_SKB_CB(tmp1)->tcp_flags & TCPHDR_FIN)
				mptcp_fin(meta_sk);

			/* Check if this fills a gap in the ofo queue */
			if (!RB_EMPTY_ROOT(&meta_tp->out_of_order_queue))
				tcp_ofo_queue(meta_sk);

			mptcp_check_rcvseq_wrap(meta_tp, old_rcv_nxt);

			if (eaten)
				kfree_skb_partial(tmp1, fragstolen);

			data_queued = true;
next:
			if (!skb_queue_empty(&sk->sk_receive_queue) &&
			    !before(TCP_SKB_CB(tmp)->seq,
				    tp->mptcp->map_subseq + tp->mptcp->map_data_len))
				break;
		}
	}

	inet_csk(meta_sk)->icsk_ack.lrcvtime = tcp_jiffies32;
	mptcp_reset_mapping(tp, old_copied_seq);

	return data_queued ? -1 : -2;
}

void mptcp_data_ready(struct sock *sk)
{
	struct sock *meta_sk = mptcp_meta_sk(sk);
	struct sk_buff *skb, *tmp;
	int queued = 0;

	tcp_mstamp_refresh(tcp_sk(meta_sk));

	/* restart before the check, because mptcp_fin might have changed the
	 * state.
	 */
restart:
	/* If the meta cannot receive data, there is no point in pushing data.
	 * If we are in time-wait, we may still be waiting for the final FIN.
	 * So, we should proceed with the processing.
	 */
	if (!mptcp_sk_can_recv(meta_sk) && !tcp_sk(sk)->mpcb->in_time_wait) {
		skb_queue_purge(&sk->sk_receive_queue);
		tcp_sk(sk)->copied_seq = tcp_sk(sk)->rcv_nxt;
		goto exit;
	}

	/* Iterate over all segments, detect their mapping (if we don't have
	 * one yet), validate them and push everything one level higher.
	 */
	skb_queue_walk_safe(&sk->sk_receive_queue, skb, tmp) {
		int ret;
		/* Pre-validation - e.g., early fallback */
		ret = mptcp_prevalidate_skb(sk, skb);
		if (ret < 0)
			goto restart;
		else if (ret > 0)
			break;

		/* Set the current mapping */
		ret = mptcp_detect_mapping(sk, skb);
		if (ret < 0)
			goto restart;
		else if (ret > 0)
			break;

		/* Validation */
		if (mptcp_validate_mapping(sk, skb) < 0)
			goto restart;

		/* Push a level higher */
		ret = mptcp_queue_skb(sk);
		if (ret < 0) {
			if (ret == -1)
				queued = ret;
			goto restart;
		} else if (ret == 0) {
			continue;
		} else { /* ret == 1 */
			break;
		}
	}

exit:
	if (tcp_sk(sk)->close_it && sk->sk_state == TCP_FIN_WAIT2) {
		tcp_send_ack(sk);
		tcp_sk(sk)->ops->time_wait(sk, TCP_TIME_WAIT, 0);
	}

	if (queued == -1 && !sock_flag(meta_sk, SOCK_DEAD))
		meta_sk->sk_data_ready(meta_sk);
}

struct mp_join *mptcp_find_join(const struct sk_buff *skb)
{
	const struct tcphdr *th = tcp_hdr(skb);
	unsigned char *ptr;
	int length = (th->doff * 4) - sizeof(struct tcphdr);

	/* Jump through the options to check whether JOIN is there */
	ptr = (unsigned char *)(th + 1);
	while (length > 0) {
		int opcode = *ptr++;
		int opsize;

		switch (opcode) {
		case TCPOPT_EOL:
			return NULL;
		case TCPOPT_NOP:	/* Ref: RFC 793 section 3.1 */
			length--;
			continue;
		default:
			opsize = *ptr++;
			if (opsize < 2)	/* "silly options" */
				return NULL;
			if (opsize > length)
				return NULL;  /* don't parse partial options */
			if (opcode == TCPOPT_MPTCP &&
			    ((struct mptcp_option *)(ptr - 2))->sub == MPTCP_SUB_JOIN) {
				return (struct mp_join *)(ptr - 2);
			}
			ptr += opsize - 2;
			length -= opsize;
		}
	}
	return NULL;
}

int mptcp_lookup_join(struct sk_buff *skb, struct inet_timewait_sock *tw)
{
	struct sock *meta_sk;
	u32 token;
	bool meta_v4;
	struct mp_join *join_opt = mptcp_find_join(skb);
	if (!join_opt)
		return 0;

	/* MPTCP structures were not initialized, so return error */
	if (mptcp_init_failed)
		return -1;

	token = join_opt->u.syn.token;
	meta_sk = mptcp_hash_find(dev_net(skb_dst(skb)->dev), token);
	if (!meta_sk) {
		MPTCP_INC_STATS(dev_net(skb_dst(skb)->dev), MPTCP_MIB_JOINNOTOKEN);
		mptcp_debug("%s:mpcb not found:%x\n", __func__, token);
		return -1;
	}

	meta_v4 = meta_sk->sk_family == AF_INET;
	if (meta_v4) {
		if (skb->protocol == htons(ETH_P_IPV6)) {
			mptcp_debug("SYN+MP_JOIN with IPV6 address on pure IPV4 meta\n");
			sock_put(meta_sk); /* Taken by mptcp_hash_find */
			return -1;
		}
	} else if (skb->protocol == htons(ETH_P_IP) && meta_sk->sk_ipv6only) {
		mptcp_debug("SYN+MP_JOIN with IPV4 address on IPV6_V6ONLY meta\n");
		sock_put(meta_sk); /* Taken by mptcp_hash_find */
		return -1;
	}

	/* Coming from time-wait-sock processing in tcp_v4_rcv.
	 * We have to deschedule it before continuing, because otherwise
	 * mptcp_v4_do_rcv will hit again on it inside tcp_v4_hnd_req.
	 */
	if (tw)
		inet_twsk_deschedule_put(tw);

	/* OK, this is a new syn/join, let's create a new open request and
	 * send syn+ack
	 */
	if (skb->protocol == htons(ETH_P_IP)) {
		tcp_v4_do_rcv(meta_sk, skb);
#if IS_ENABLED(CONFIG_IPV6)
	} else {
		tcp_v6_do_rcv(meta_sk, skb);
#endif /* CONFIG_IPV6 */
	}
	sock_put(meta_sk); /* Taken by mptcp_hash_find */
	return 1;
}

int mptcp_do_join_short(struct sk_buff *skb,
			const struct mptcp_options_received *mopt,
			struct net *net)
{
	struct sock *meta_sk;
	u32 token;
	bool meta_v4;

	token = mopt->mptcp_rem_token;
	meta_sk = mptcp_hash_find(net, token);
	if (!meta_sk) {
		MPTCP_INC_STATS(dev_net(skb_dst(skb)->dev), MPTCP_MIB_JOINNOTOKEN);
		mptcp_debug("%s:mpcb not found:%x\n", __func__, token);
		return -1;
	}

	meta_v4 = meta_sk->sk_family == AF_INET;
	if (meta_v4) {
		if (skb->protocol == htons(ETH_P_IPV6)) {
			mptcp_debug("SYN+MP_JOIN with IPV6 address on pure IPV4 meta\n");
			sock_put(meta_sk); /* Taken by mptcp_hash_find */
			return -1;
		}
	} else if (skb->protocol == htons(ETH_P_IP) && meta_sk->sk_ipv6only) {
		mptcp_debug("SYN+MP_JOIN with IPV4 address on IPV6_V6ONLY meta\n");
		sock_put(meta_sk); /* Taken by mptcp_hash_find */
		return -1;
	}

	/* OK, this is a new syn/join, let's create a new open request and
	 * send syn+ack
	 */

	/* mptcp_v4_do_rcv tries to free the skb - we prevent this, as
	 * the skb will finally be freed by tcp_v4_do_rcv (where we are
	 * coming from)
	 */
	skb_get(skb);
	if (skb->protocol == htons(ETH_P_IP)) {
		tcp_v4_do_rcv(meta_sk, skb);
#if IS_ENABLED(CONFIG_IPV6)
	} else { /* IPv6 */
		tcp_v6_do_rcv(meta_sk, skb);
#endif /* CONFIG_IPV6 */
	}

	sock_put(meta_sk); /* Taken by mptcp_hash_find */
	return 0;
}

/**
 * Equivalent of tcp_fin() for MPTCP
 * Can be called only when the FIN is validly part
 * of the data seqnum space. Not before when we get holes.
 */
void mptcp_fin(struct sock *meta_sk)
{
	struct sock *sk = NULL;
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct mptcp_cb *mpcb = meta_tp->mpcb;
	struct mptcp_tcp_sock *mptcp;
	unsigned char state;

	mptcp_for_each_sub(mpcb, mptcp) {
		struct sock *sk_it = mptcp_to_sock(mptcp);

		if (tcp_sk(sk_it)->mptcp->path_index == mpcb->dfin_path_index) {
			sk = sk_it;
			break;
		}
	}

	if (!sk || sk->sk_state == TCP_CLOSE)
		sk = mptcp_select_ack_sock(meta_sk);

	inet_csk_schedule_ack(sk);

	if (!mpcb->in_time_wait) {
		meta_sk->sk_shutdown |= RCV_SHUTDOWN;
		sock_set_flag(meta_sk, SOCK_DONE);
		state = meta_sk->sk_state;
	} else {
		state = mpcb->mptw_state;
	}

	switch (state) {
	case TCP_SYN_RECV:
	case TCP_ESTABLISHED:
		/* Move to CLOSE_WAIT */
		tcp_set_state(meta_sk, TCP_CLOSE_WAIT);
		inet_csk(sk)->icsk_ack.pingpong = 1;
		break;

	case TCP_CLOSE_WAIT:
	case TCP_CLOSING:
		/* Received a retransmission of the FIN, do
		 * nothing.
		 */
		break;
	case TCP_LAST_ACK:
		/* RFC793: Remain in the LAST-ACK state. */
		break;

	case TCP_FIN_WAIT1:
		/* This case occurs when a simultaneous close
		 * happens, we must ack the received FIN and
		 * enter the CLOSING state.
		 */
		tcp_send_ack(sk);
		tcp_set_state(meta_sk, TCP_CLOSING);
		break;
	case TCP_FIN_WAIT2:
		/* Received a FIN -- send ACK and enter TIME_WAIT. */
		tcp_send_ack(sk);
		meta_tp->ops->time_wait(meta_sk, TCP_TIME_WAIT, 0);
		break;
	default:
		/* Only TCP_LISTEN and TCP_CLOSE are left, in these
		 * cases we should never reach this piece of code.
		 */
		pr_err("%s: Impossible, meta_sk->sk_state=%d\n", __func__,
		       meta_sk->sk_state);
		break;
	}

	/* It _is_ possible, that we have something out-of-order _after_ FIN.
	 * Probably, we should reset in this case. For now drop them.
	 */
	skb_rbtree_purge(&meta_tp->out_of_order_queue);
	sk_mem_reclaim(meta_sk);

	if (!sock_flag(meta_sk, SOCK_DEAD)) {
		meta_sk->sk_state_change(meta_sk);

		/* Do not send POLL_HUP for half duplex close. */
		if (meta_sk->sk_shutdown == SHUTDOWN_MASK ||
		    meta_sk->sk_state == TCP_CLOSE)
			sk_wake_async(meta_sk, SOCK_WAKE_WAITD, POLL_HUP);
		else
			sk_wake_async(meta_sk, SOCK_WAKE_WAITD, POLL_IN);
	}

	return;
}

/* Similar to tcp_xmit_retransmit_queue */
static void mptcp_xmit_retransmit_queue(struct sock *meta_sk)
{
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct sk_buff *skb, *rtx_head;

	if (!meta_tp->packets_out)
		return;

	skb = rtx_head = tcp_rtx_queue_head(meta_sk);
	skb_rbtree_walk_from(skb) {
		if (mptcp_retransmit_skb(meta_sk, skb))
			return;

		if (skb == rtx_head)
			inet_csk_reset_xmit_timer(meta_sk, ICSK_TIME_RETRANS,
						  inet_csk(meta_sk)->icsk_rto,
						  TCP_RTO_MAX);
	}
}

static void mptcp_snd_una_update(struct tcp_sock *meta_tp, u32 data_ack)
{
	u32 delta = data_ack - meta_tp->snd_una;

	sock_owned_by_me((struct sock *)meta_tp);
	meta_tp->bytes_acked += delta;
	meta_tp->snd_una = data_ack;
}

static void mptcp_stop_subflow_chronos(struct sock *meta_sk,
				       const enum tcp_chrono type)
{
	const struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
	struct mptcp_tcp_sock *mptcp;

	mptcp_for_each_sub(mpcb, mptcp) {
		struct sock *sk_it = mptcp_to_sock(mptcp);

		tcp_chrono_stop(sk_it, type);
	}
}

/* Return false if we can continue processing packets. True, otherwise */
static bool mptcp_process_data_ack(struct sock *sk, const struct sk_buff *skb)
{
	struct sock *meta_sk = mptcp_meta_sk(sk);
	struct tcp_sock *meta_tp = tcp_sk(meta_sk), *tp = tcp_sk(sk);
	struct tcp_skb_cb *tcb = TCP_SKB_CB(skb);
	u32 prior_snd_una = meta_tp->snd_una;
	int prior_packets;
	u32 nwin, data_ack, data_seq;
	u16 data_len = 0;

	/* A valid packet came in - subflow is operational again */
	tp->pf = 0;

	/* Even if there is no data-ack, we stop retransmitting.
	 * Except if this is a SYN/ACK. Then it is just a retransmission
	 */
	if (tp->mptcp->pre_established && !tcp_hdr(skb)->syn) {
		tp->mptcp->pre_established = 0;
		sk_stop_timer(sk, &tp->mptcp->mptcp_ack_timer);

		if (meta_tp->mpcb->pm_ops->established_subflow)
			meta_tp->mpcb->pm_ops->established_subflow(sk);
	}

	/* If we are in infinite mapping mode, rx_opt.data_ack has been
	 * set by mptcp_handle_ack_in_infinite.
	 */
	if (!(tcb->mptcp_flags & MPTCPHDR_ACK) && !tp->mpcb->infinite_mapping_snd)
		return false;

	if (unlikely(!tp->mptcp->fully_established) &&
	    tp->mptcp->snt_isn + 1 != TCP_SKB_CB(skb)->ack_seq)
		/* As soon as a subflow-data-ack (not acking syn, thus snt_isn + 1)
		 * includes a data-ack, we are fully established
		 */
		mptcp_become_fully_estab(sk);

	/* After we did the subflow-only processing (stopping timer and marking
	 * subflow as established), check if we can proceed with MPTCP-level
	 * processing.
	 */
	if (meta_sk->sk_state == TCP_CLOSE)
		return false;

	/* Get the data_seq */
	if (mptcp_is_data_seq(skb)) {
		data_seq = tp->mptcp->rx_opt.data_seq;
		data_len = tp->mptcp->rx_opt.data_len;
	} else {
		data_seq = meta_tp->snd_wl1;
	}

	data_ack = tp->mptcp->rx_opt.data_ack;

	/* If the ack is older than previous acks
	 * then we can probably ignore it.
	 */
	if (before(data_ack, prior_snd_una))
		goto exit;

	/* If the ack includes data we haven't sent yet, discard
	 * this segment (RFC793 Section 3.9).
	 */
	if (after(data_ack, meta_tp->snd_nxt))
		goto exit;

	/* First valid DATA_ACK, we can stop sending the special MP_CAPABLE */
	tp->mpcb->send_mptcpv1_mpcapable = 0;

	/*** Now, update the window  - inspired by tcp_ack_update_window ***/
	nwin = ntohs(tcp_hdr(skb)->window);

	if (likely(!tcp_hdr(skb)->syn))
		nwin <<= tp->rx_opt.snd_wscale;

	if (tcp_may_update_window(meta_tp, data_ack, data_seq, nwin)) {
		tcp_update_wl(meta_tp, data_seq);

		/* Draft v09, Section 3.3.5:
		 * [...] It should only update its local receive window values
		 * when the largest sequence number allowed (i.e.  DATA_ACK +
		 * receive window) increases. [...]
		 */
		if (meta_tp->snd_wnd != nwin &&
		    !before(data_ack + nwin, tcp_wnd_end(meta_tp))) {
			meta_tp->snd_wnd = nwin;

			if (nwin > meta_tp->max_window)
				meta_tp->max_window = nwin;
		}
	}
	/*** Done, update the window ***/

	/* We passed data and got it acked, remove any soft error
	 * log. Something worked...
	 */
	sk->sk_err_soft = 0;
	inet_csk(meta_sk)->icsk_probes_out = 0;
	meta_tp->rcv_tstamp = tcp_jiffies32;
	prior_packets = meta_tp->packets_out;
	if (!prior_packets)
		goto no_queue;

	mptcp_snd_una_update(meta_tp, data_ack);

	mptcp_clean_rtx_queue(meta_sk, prior_snd_una);

	/* We are in loss-state, and something got acked, retransmit the whole
	 * queue now!
	 */
	if (inet_csk(meta_sk)->icsk_ca_state == TCP_CA_Loss &&
	    after(data_ack, prior_snd_una)) {
		mptcp_xmit_retransmit_queue(meta_sk);
		inet_csk(meta_sk)->icsk_ca_state = TCP_CA_Open;
	}

	/* Simplified version of tcp_new_space, because the snd-buffer
	 * is handled by all the subflows.
	 */
	if (sock_flag(meta_sk, SOCK_QUEUE_SHRUNK)) {
		sock_reset_flag(meta_sk, SOCK_QUEUE_SHRUNK);
		if (meta_sk->sk_socket &&
		    test_bit(SOCK_NOSPACE, &meta_sk->sk_socket->flags))
			meta_sk->sk_write_space(meta_sk);

		if (meta_sk->sk_socket &&
		    !test_bit(SOCK_NOSPACE, &meta_sk->sk_socket->flags)) {
			tcp_chrono_stop(meta_sk, TCP_CHRONO_SNDBUF_LIMITED);
			mptcp_stop_subflow_chronos(meta_sk,
						   TCP_CHRONO_SNDBUF_LIMITED);
		}
	}

	if (meta_sk->sk_state != TCP_ESTABLISHED) {
		int ret = mptcp_rcv_state_process(meta_sk, sk, skb, data_seq, data_len);

		if (ret < 0)
			return true;
		else if (ret > 0)
			return false;
	}

exit:
	mptcp_push_pending_frames(meta_sk);

	return false;

no_queue:
	if (tcp_send_head(meta_sk))
		tcp_ack_probe(meta_sk);

	mptcp_push_pending_frames(meta_sk);

	return false;
}

/* Return false if we can continue processing packets. True, otherwise */
bool mptcp_handle_ack_in_infinite(struct sock *sk, const struct sk_buff *skb,
				  int flag)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct tcp_sock *meta_tp = mptcp_meta_tp(tp);
	struct mptcp_cb *mpcb = tp->mpcb;

	/* We are already in fallback-mode. Data is in-sequence and we know
	 * exactly what is being sent on this subflow belongs to the current
	 * meta-level sequence number space.
	 */
	if (mpcb->infinite_mapping_snd) {
		if (mpcb->infinite_send_una_ahead &&
		    !before(meta_tp->snd_una, tp->mptcp->last_end_data_seq - (tp->snd_nxt - tp->snd_una))) {
			tp->mptcp->rx_opt.data_ack = meta_tp->snd_una;
		} else {
			/* Remember that meta snd_una is no more ahead of the game */
			mpcb->infinite_send_una_ahead = 0;

			/* The difference between both write_seq's represents the offset between
			 * data-sequence and subflow-sequence. As we are infinite, this must
			 * match.
			 *
			 * Thus, from this difference we can infer the meta snd_una.
			 */
			tp->mptcp->rx_opt.data_ack = meta_tp->snd_nxt -
						     (tp->snd_nxt - tp->snd_una);
		}

		goto exit;
	}

	/* If data has been acknowleged on the meta-level, fully_established
	 * will have been set before and thus we will not fall back to infinite
	 * mapping.
	 */
	if (likely(tp->mptcp->fully_established))
		return false;

	if (!(flag & MPTCP_FLAG_DATA_ACKED))
		return false;

	pr_debug("%s %#x will fallback - pi %d, src %pI4:%u dst %pI4:%u rcv_nxt %u\n",
		 __func__, mpcb->mptcp_loc_token, tp->mptcp->path_index,
		 &inet_sk(sk)->inet_saddr, ntohs(inet_sk(sk)->inet_sport),
		 &inet_sk(sk)->inet_daddr, ntohs(inet_sk(sk)->inet_dport),
		 tp->rcv_nxt);
	if (!is_master_tp(tp)) {
		MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_FBACKSUB);
		return true;
	}

	/* We have sent more than what has ever been sent on the master subflow.
	 * This means, we won't be able to seamlessly fallback because there
	 * will now be a hole in the sequence space.
	 */
	if (before(tp->mptcp->last_end_data_seq, meta_tp->snd_una))
		return true;

	mpcb->infinite_mapping_snd = 1;
	mpcb->infinite_mapping_rcv = 1;
	mpcb->infinite_rcv_seq = mptcp_get_rcv_nxt_64(mptcp_meta_tp(tp));
	tp->mptcp->fully_established = 1;

	MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_FBACKINIT);

	if (mptcp_fallback_close(mpcb, sk))
		return true;

	mptcp_restart_sending(tp->meta_sk, tp->mptcp->last_end_data_seq);

	/* The acknowledged data-seq at the subflow-level is:
	 * last_end_data_seq - (tp->snd_nxt - tp->snd_una)
	 *
	 * If this is less than meta->snd_una, then we ignore it. Otherwise,
	 * this becomes our data_ack.
	 */
	if (after(meta_tp->snd_una, tp->mptcp->last_end_data_seq - (tp->snd_nxt - tp->snd_una))) {
		/* Remmeber that meta snd_una is ahead of the game */
		mpcb->infinite_send_una_ahead = 1;
		tp->mptcp->rx_opt.data_ack = meta_tp->snd_una;
	} else {
		tp->mptcp->rx_opt.data_ack = tp->mptcp->last_end_data_seq -
			(tp->snd_nxt - tp->snd_una);
	}

exit:

	return mptcp_process_data_ack(sk, skb);
}

/**** static functions used by mptcp_parse_options */

static void mptcp_send_reset_rem_id(const struct mptcp_cb *mpcb, u8 rem_id)
{
	struct mptcp_tcp_sock *mptcp;
	struct hlist_node *tmp;

	mptcp_for_each_sub_safe(mpcb, mptcp, tmp) {
		struct sock *sk_it = mptcp_to_sock(mptcp);

		if (tcp_sk(sk_it)->mptcp->rem_id == rem_id) {
			mptcp_reinject_data(sk_it, 0);
			mptcp_send_reset(sk_it);
		}
	}
}

static inline bool is_valid_addropt_opsize(u8 mptcp_ver,
					   struct mp_add_addr *mpadd,
					   int opsize)
{
#if IS_ENABLED(CONFIG_IPV6)
	if (mptcp_ver < MPTCP_VERSION_1 && mpadd->u_bit.v0.ipver == 6) {
		return opsize == MPTCP_SUB_LEN_ADD_ADDR6 ||
		       opsize == MPTCP_SUB_LEN_ADD_ADDR6 + 2;
	}
	if (mptcp_ver >= MPTCP_VERSION_1)
		return opsize == MPTCP_SUB_LEN_ADD_ADDR6_VER1 ||
		       opsize == MPTCP_SUB_LEN_ADD_ADDR6_VER1 + 2 ||
		       opsize == MPTCP_SUB_LEN_ADD_ADDR4_VER1 ||
		       opsize == MPTCP_SUB_LEN_ADD_ADDR4_VER1 + 2;
#endif
	if (mptcp_ver < MPTCP_VERSION_1 && mpadd->u_bit.v0.ipver == 4) {
		return opsize == MPTCP_SUB_LEN_ADD_ADDR4 ||
		       opsize == MPTCP_SUB_LEN_ADD_ADDR4 + 2;
	}
	if (mptcp_ver >= MPTCP_VERSION_1) {
		return opsize == MPTCP_SUB_LEN_ADD_ADDR4_VER1 ||
		       opsize == MPTCP_SUB_LEN_ADD_ADDR4_VER1 + 2;
	}
	return false;
}

void mptcp_parse_options(const uint8_t *ptr, int opsize,
			 struct mptcp_options_received *mopt,
			 const struct sk_buff *skb,
			 struct tcp_sock *tp)
{
	const struct mptcp_option *mp_opt = (struct mptcp_option *)ptr;
	const struct tcphdr *th = tcp_hdr(skb);

	/* If the socket is mp-capable we would have a mopt. */
	if (!mopt)
		return;

	switch (mp_opt->sub) {
	case MPTCP_SUB_CAPABLE:
	{
		const struct mp_capable *mpcapable = (struct mp_capable *)ptr;

		if (mpcapable->ver == MPTCP_VERSION_0 &&
		    ((th->syn && opsize != MPTCP_SUB_LEN_CAPABLE_SYN) ||
		     (!th->syn && th->ack && opsize != MPTCP_SUB_LEN_CAPABLE_ACK))) {
			mptcp_debug("%s: mp_capable v0: bad option size %d\n",
				    __func__, opsize);
			break;
		}

		if (mpcapable->ver == MPTCP_VERSION_1 &&
		    ((th->syn && !th->ack && opsize != MPTCPV1_SUB_LEN_CAPABLE_SYN) ||
		     (th->syn && th->ack && opsize != MPTCPV1_SUB_LEN_CAPABLE_SYNACK) ||
		     (!th->syn && th->ack && opsize != MPTCPV1_SUB_LEN_CAPABLE_ACK &&
		      opsize != MPTCPV1_SUB_LEN_CAPABLE_DATA &&
		      opsize != MPTCPV1_SUB_LEN_CAPABLE_DATA_CSUM))) {
			mptcp_debug("%s: mp_capable v1: bad option size %d\n",
				    __func__, opsize);
			break;
		}

		/* MPTCP-RFC 6824:
		 * "If receiving a message with the 'B' flag set to 1, and this
		 * is not understood, then this SYN MUST be silently ignored;
		 */
		if (mpcapable->b) {
			mopt->drop_me = 1;
			break;
		}

		/* MPTCP-RFC 6824:
		 * "An implementation that only supports this method MUST set
		 *  bit "H" to 1, and bits "C" through "G" to 0."
		 */
		if (!mpcapable->h)
			break;

		mopt->saw_mpc = 1;
		mopt->dss_csum = sysctl_mptcp_checksum || mpcapable->a;

		if (mpcapable->ver == MPTCP_VERSION_0) {
			if (opsize == MPTCP_SUB_LEN_CAPABLE_SYN)
				mopt->mptcp_sender_key = mpcapable->sender_key;

			if (opsize == MPTCP_SUB_LEN_CAPABLE_ACK) {
				mopt->mptcp_sender_key = mpcapable->sender_key;
				mopt->mptcp_receiver_key = mpcapable->receiver_key;
			}
		} else if (mpcapable->ver == MPTCP_VERSION_1) {
			if (opsize == MPTCPV1_SUB_LEN_CAPABLE_SYNACK)
				mopt->mptcp_sender_key = mpcapable->sender_key;

			if (opsize == MPTCPV1_SUB_LEN_CAPABLE_ACK) {
				mopt->mptcp_sender_key = mpcapable->sender_key;
				mopt->mptcp_receiver_key = mpcapable->receiver_key;
			}

			if (opsize == MPTCPV1_SUB_LEN_CAPABLE_DATA ||
			    opsize == MPTCPV1_SUB_LEN_CAPABLE_DATA_CSUM) {
				mopt->mptcp_sender_key = mpcapable->sender_key;
				mopt->mptcp_receiver_key = mpcapable->receiver_key;

				TCP_SKB_CB(skb)->mptcp_flags |= MPTCPHDR_MPC_DATA;

				ptr += sizeof(struct mp_capable);
				TCP_SKB_CB(skb)->dss_off = (ptr - skb_transport_header(skb));

				/* Is a check-sum present? */
				if (opsize == MPTCPV1_SUB_LEN_CAPABLE_DATA_CSUM)
					TCP_SKB_CB(skb)->mptcp_flags |= MPTCPHDR_DSS_CSUM;
			}
		}

		mopt->mptcp_ver = mpcapable->ver;
		break;
	}
	case MPTCP_SUB_JOIN:
	{
		const struct mp_join *mpjoin = (struct mp_join *)ptr;

		if (opsize != MPTCP_SUB_LEN_JOIN_SYN &&
		    opsize != MPTCP_SUB_LEN_JOIN_SYNACK &&
		    opsize != MPTCP_SUB_LEN_JOIN_ACK) {
			mptcp_debug("%s: mp_join: bad option size %d\n",
				    __func__, opsize);
			break;
		}

		/* saw_mpc must be set, because in tcp_check_req we assume that
		 * it is set to support falling back to reg. TCP if a rexmitted
		 * SYN has no MP_CAPABLE or MP_JOIN
		 */
		switch (opsize) {
		case MPTCP_SUB_LEN_JOIN_SYN:
			mopt->is_mp_join = 1;
			mopt->saw_mpc = 1;
			mopt->low_prio = mpjoin->b;
			mopt->rem_id = mpjoin->addr_id;
			mopt->mptcp_rem_token = mpjoin->u.syn.token;
			mopt->mptcp_recv_nonce = mpjoin->u.syn.nonce;
			break;
		case MPTCP_SUB_LEN_JOIN_SYNACK:
			mopt->saw_mpc = 1;
			mopt->low_prio = mpjoin->b;
			mopt->rem_id = mpjoin->addr_id;
			mopt->mptcp_recv_tmac = mpjoin->u.synack.mac;
			mopt->mptcp_recv_nonce = mpjoin->u.synack.nonce;
			break;
		case MPTCP_SUB_LEN_JOIN_ACK:
			mopt->saw_mpc = 1;
			mopt->join_ack = 1;
			memcpy(mopt->mptcp_recv_mac, mpjoin->u.ack.mac, 20);
			break;
		}
		break;
	}
	case MPTCP_SUB_DSS:
	{
		const struct mp_dss *mdss = (struct mp_dss *)ptr;
		struct tcp_skb_cb *tcb = TCP_SKB_CB(skb);

		/* We check opsize for the csum and non-csum case. We do this,
		 * because the draft says that the csum SHOULD be ignored if
		 * it has not been negotiated in the MP_CAPABLE but still is
		 * present in the data.
		 *
		 * It will get ignored later in mptcp_queue_skb.
		 */
		if (opsize != mptcp_sub_len_dss(mdss, 0) &&
		    opsize != mptcp_sub_len_dss(mdss, 1)) {
			mptcp_debug("%s: mp_dss: bad option size %d\n",
				    __func__, opsize);
			break;
		}

		ptr += 4;

		if (mdss->A) {
			tcb->mptcp_flags |= MPTCPHDR_ACK;

			if (mdss->a) {
				mopt->data_ack = (u32) get_unaligned_be64(ptr);
				ptr += MPTCP_SUB_LEN_ACK_64;
			} else {
				mopt->data_ack = get_unaligned_be32(ptr);
				ptr += MPTCP_SUB_LEN_ACK;
			}
		}

		tcb->dss_off = (ptr - skb_transport_header(skb));

		if (mdss->M) {
			if (mdss->m) {
				u64 data_seq64 = get_unaligned_be64(ptr);

				tcb->mptcp_flags |= MPTCPHDR_SEQ64_SET;
				mopt->data_seq = (u32) data_seq64;

				ptr += 12; /* 64-bit dseq + subseq */
			} else {
				mopt->data_seq = get_unaligned_be32(ptr);
				ptr += 8; /* 32-bit dseq + subseq */
			}
			mopt->data_len = get_unaligned_be16(ptr);

			tcb->mptcp_flags |= MPTCPHDR_SEQ;

			/* Is a check-sum present? */
			if (opsize == mptcp_sub_len_dss(mdss, 1))
				tcb->mptcp_flags |= MPTCPHDR_DSS_CSUM;

			/* DATA_FIN only possible with DSS-mapping */
			if (mdss->F)
				tcb->mptcp_flags |= MPTCPHDR_FIN;
		}

		break;
	}
	case MPTCP_SUB_ADD_ADDR:
	{
		struct mp_add_addr *mpadd = (struct mp_add_addr *)ptr;

		/* If tcp_sock is not available, MPTCP version can't be
		 * retrieved and ADD_ADDR opsize validation is not possible.
		 */
		if (!tp || !tp->mpcb)
			break;

		if (!is_valid_addropt_opsize(tp->mpcb->mptcp_ver,
					     mpadd, opsize)) {
			mptcp_debug("%s: mp_add_addr: bad option size %d\n",
				    __func__, opsize);
			break;
		}

		/* We have to manually parse the options if we got two of them. */
		if (mopt->saw_add_addr) {
			mopt->more_add_addr = 1;
			break;
		}
		mopt->saw_add_addr = 1;
		mopt->add_addr_ptr = ptr;
		break;
	}
	case MPTCP_SUB_REMOVE_ADDR:
		if ((opsize - MPTCP_SUB_LEN_REMOVE_ADDR) < 0) {
			mptcp_debug("%s: mp_remove_addr: bad option size %d\n",
				    __func__, opsize);
			break;
		}

		if (mopt->saw_rem_addr) {
			mopt->more_rem_addr = 1;
			break;
		}
		mopt->saw_rem_addr = 1;
		mopt->rem_addr_ptr = ptr;
		break;
	case MPTCP_SUB_PRIO:
	{
		const struct mp_prio *mpprio = (struct mp_prio *)ptr;

		if (opsize != MPTCP_SUB_LEN_PRIO &&
		    opsize != MPTCP_SUB_LEN_PRIO_ADDR) {
			mptcp_debug("%s: mp_prio: bad option size %d\n",
				    __func__, opsize);
			break;
		}

		mopt->saw_low_prio = 1;
		mopt->low_prio = mpprio->b;

		if (opsize == MPTCP_SUB_LEN_PRIO_ADDR) {
			mopt->saw_low_prio = 2;
			mopt->prio_addr_id = mpprio->addr_id;
		}
		break;
	}
	case MPTCP_SUB_FAIL:
		if (opsize != MPTCP_SUB_LEN_FAIL) {
			mptcp_debug("%s: mp_fail: bad option size %d\n",
				    __func__, opsize);
			break;
		}
		mopt->mp_fail = 1;
		break;
	case MPTCP_SUB_FCLOSE:
		if (opsize != MPTCP_SUB_LEN_FCLOSE) {
			mptcp_debug("%s: mp_fclose: bad option size %d\n",
				    __func__, opsize);
			break;
		}

		mopt->mp_fclose = 1;
		mopt->mptcp_sender_key = ((struct mp_fclose *)ptr)->key;

		break;
	default:
		mptcp_debug("%s: Received unkown subtype: %d\n",
			    __func__, mp_opt->sub);
		break;
	}
}

/** Parse only MPTCP options */
void tcp_parse_mptcp_options(const struct sk_buff *skb,
			     struct mptcp_options_received *mopt)
{
	const struct tcphdr *th = tcp_hdr(skb);
	int length = (th->doff * 4) - sizeof(struct tcphdr);
	const unsigned char *ptr = (const unsigned char *)(th + 1);

	while (length > 0) {
		int opcode = *ptr++;
		int opsize;

		switch (opcode) {
		case TCPOPT_EOL:
			return;
		case TCPOPT_NOP:	/* Ref: RFC 793 section 3.1 */
			length--;
			continue;
		default:
			opsize = *ptr++;
			if (opsize < 2)	/* "silly options" */
				return;
			if (opsize > length)
				return;	/* don't parse partial options */
			if (opcode == TCPOPT_MPTCP)
				mptcp_parse_options(ptr - 2, opsize, mopt, skb, NULL);
		}
		ptr += opsize - 2;
		length -= opsize;
	}
}

bool mptcp_check_rtt(const struct tcp_sock *tp, int time)
{
	struct mptcp_cb *mpcb = tp->mpcb;
	struct mptcp_tcp_sock *mptcp;
	u32 rtt_max = 0;

	/* In MPTCP, we take the max delay across all flows,
	 * in order to take into account meta-reordering buffers.
	 */
	mptcp_for_each_sub(mpcb, mptcp) {
		struct sock *sk = mptcp_to_sock(mptcp);

		if (!mptcp_sk_can_recv(sk))
			continue;

		if (rtt_max < tcp_sk(sk)->rcv_rtt_est.rtt_us)
			rtt_max = tcp_sk(sk)->rcv_rtt_est.rtt_us;
	}
	if (time < (rtt_max >> 3) || !rtt_max)
		return true;

	return false;
}

static void mptcp_handle_add_addr(const unsigned char *ptr, struct sock *sk)
{
	struct mp_add_addr *mpadd = (struct mp_add_addr *)ptr;
	struct mptcp_cb *mpcb = tcp_sk(sk)->mpcb;
	union inet_addr addr;
	sa_family_t family;
	__be16 port = 0;
	bool is_v4;

	if (mpcb->mptcp_ver < MPTCP_VERSION_1) {
		is_v4 = mpadd->u_bit.v0.ipver == 4;
	} else {
		is_v4 = mpadd->len == MPTCP_SUB_LEN_ADD_ADDR4_VER1 ||
			mpadd->len == MPTCP_SUB_LEN_ADD_ADDR4_VER1 + 2;

		/* TODO: support ADD_ADDRv1 retransmissions */
		if (mpadd->u_bit.v1.echo)
			return;
	}

	if (is_v4) {
		u8 hash_mac_check[SHA256_DIGEST_SIZE];
		__be16 hmacport = 0;
		char *recv_hmac;

		if (mpcb->mptcp_ver < MPTCP_VERSION_1)
			goto skip_hmac_v4;

		recv_hmac = (char *)mpadd->u.v4.mac;
		if (mpadd->len == MPTCP_SUB_LEN_ADD_ADDR4_VER1) {
			recv_hmac -= sizeof(mpadd->u.v4.port);
		} else if (mpadd->len == MPTCP_SUB_LEN_ADD_ADDR4_VER1 + 2) {
			hmacport = mpadd->u.v4.port;
		}
		mptcp_hmac(mpcb->mptcp_ver, (u8 *)&mpcb->mptcp_rem_key,
			   (u8 *)&mpcb->mptcp_loc_key, hash_mac_check, 3,
			   1, (u8 *)&mpadd->addr_id,
			   4, (u8 *)&mpadd->u.v4.addr.s_addr,
			   2, (u8 *)&hmacport);
		if (memcmp(&hash_mac_check[SHA256_DIGEST_SIZE - sizeof(u64)], recv_hmac, 8) != 0)
			/* ADD_ADDR2 discarded */
			return;
skip_hmac_v4:
		if ((mpcb->mptcp_ver == MPTCP_VERSION_0 &&
		     mpadd->len == MPTCP_SUB_LEN_ADD_ADDR4 + 2) ||
		     (mpcb->mptcp_ver == MPTCP_VERSION_1 &&
		     mpadd->len == MPTCP_SUB_LEN_ADD_ADDR4_VER1 + 2))
			port  = mpadd->u.v4.port;
		family = AF_INET;
		addr.in = mpadd->u.v4.addr;
#if IS_ENABLED(CONFIG_IPV6)
	} else {
		u8 hash_mac_check[SHA256_DIGEST_SIZE];
		__be16 hmacport = 0;
		char *recv_hmac;

		if (mpcb->mptcp_ver < MPTCP_VERSION_1)
			goto skip_hmac_v6;

		recv_hmac = (char *)mpadd->u.v6.mac;
		if (mpadd->len == MPTCP_SUB_LEN_ADD_ADDR6_VER1) {
			recv_hmac -= sizeof(mpadd->u.v6.port);
		} else if (mpadd->len == MPTCP_SUB_LEN_ADD_ADDR6_VER1 + 2) {
			hmacport = mpadd->u.v6.port;
		}
		mptcp_hmac(mpcb->mptcp_ver, (u8 *)&mpcb->mptcp_rem_key,
			   (u8 *)&mpcb->mptcp_loc_key, hash_mac_check, 3,
			   1, (u8 *)&mpadd->addr_id,
			   16, (u8 *)&mpadd->u.v6.addr.s6_addr,
			   2, (u8 *)&hmacport);
		if (memcmp(&hash_mac_check[SHA256_DIGEST_SIZE - sizeof(u64)], recv_hmac, 8) != 0)
			/* ADD_ADDR2 discarded */
			return;
skip_hmac_v6:
		if ((mpcb->mptcp_ver == MPTCP_VERSION_0 &&
		     mpadd->len == MPTCP_SUB_LEN_ADD_ADDR6 + 2) ||
		     (mpcb->mptcp_ver == MPTCP_VERSION_1 &&
		     mpadd->len == MPTCP_SUB_LEN_ADD_ADDR6_VER1 + 2))
			port  = mpadd->u.v6.port;
		family = AF_INET6;
		addr.in6 = mpadd->u.v6.addr;
#endif /* CONFIG_IPV6 */
	}

	if (mpcb->pm_ops->add_raddr)
		mpcb->pm_ops->add_raddr(mpcb, &addr, family, port, mpadd->addr_id);

	MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_ADDADDRRX);
}

static void mptcp_handle_rem_addr(const unsigned char *ptr, struct sock *sk)
{
	struct mp_remove_addr *mprem = (struct mp_remove_addr *)ptr;
	int i;
	u8 rem_id;
	struct mptcp_cb *mpcb = tcp_sk(sk)->mpcb;

	for (i = 0; i <= mprem->len - MPTCP_SUB_LEN_REMOVE_ADDR; i++) {
		rem_id = (&mprem->addrs_id)[i];

		if (mpcb->pm_ops->rem_raddr)
			mpcb->pm_ops->rem_raddr(mpcb, rem_id);
		mptcp_send_reset_rem_id(mpcb, rem_id);

		MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_REMADDRSUB);
	}

	MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_REMADDRRX);
}

static void mptcp_parse_addropt(const struct sk_buff *skb, struct sock *sk)
{
	struct tcphdr *th = tcp_hdr(skb);
	unsigned char *ptr;
	int length = (th->doff * 4) - sizeof(struct tcphdr);

	/* Jump through the options to check whether ADD_ADDR is there */
	ptr = (unsigned char *)(th + 1);
	while (length > 0) {
		int opcode = *ptr++;
		int opsize;

		switch (opcode) {
		case TCPOPT_EOL:
			return;
		case TCPOPT_NOP:
			length--;
			continue;
		default:
			opsize = *ptr++;
			if (opsize < 2)
				return;
			if (opsize > length)
				return;  /* don't parse partial options */
			if (opcode == TCPOPT_MPTCP &&
			    ((struct mptcp_option *)ptr)->sub == MPTCP_SUB_ADD_ADDR) {
				u8 mptcp_ver = tcp_sk(sk)->mpcb->mptcp_ver;
				struct mp_add_addr *mpadd = (struct mp_add_addr *)ptr;

				if (!is_valid_addropt_opsize(mptcp_ver, mpadd,
							     opsize))
					goto cont;

				mptcp_handle_add_addr(ptr, sk);
			}
			if (opcode == TCPOPT_MPTCP &&
			    ((struct mptcp_option *)ptr)->sub == MPTCP_SUB_REMOVE_ADDR) {
				if ((opsize - MPTCP_SUB_LEN_REMOVE_ADDR) < 0)
					goto cont;

				mptcp_handle_rem_addr(ptr, sk);
			}
cont:
			ptr += opsize - 2;
			length -= opsize;
		}
	}
	return;
}

static bool mptcp_mp_fastclose_rcvd(struct sock *sk)
{
	struct mptcp_tcp_sock *mptcp = tcp_sk(sk)->mptcp;
	struct mptcp_cb *mpcb = tcp_sk(sk)->mpcb;

	if (likely(!mptcp->rx_opt.mp_fclose))
		return false;

	MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_FASTCLOSERX);
	mptcp->rx_opt.mp_fclose = 0;
	if (mptcp->rx_opt.mptcp_sender_key != mpcb->mptcp_loc_key)
		return false;

	mptcp_sub_force_close_all(mpcb, NULL);

	tcp_reset(mptcp_meta_sk(sk));

	return true;
}

/* Returns true if we should stop processing NOW */
static bool mptcp_mp_fail_rcvd(struct sock *sk, const struct tcphdr *th)
{
	struct mptcp_tcp_sock *mptcp = tcp_sk(sk)->mptcp;
	struct sock *meta_sk = mptcp_meta_sk(sk);
	struct mptcp_cb *mpcb = tcp_sk(sk)->mpcb;

	MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_MPFAILRX);
	mptcp->rx_opt.mp_fail = 0;

	if (!th->rst && !mpcb->infinite_mapping_snd) {
		mpcb->send_infinite_mapping = 1;

		mptcp_restart_sending(meta_sk, tcp_sk(meta_sk)->snd_una);

		return mptcp_fallback_close(mpcb, sk);
	}

	return false;
}

static inline void mptcp_path_array_check(struct sock *meta_sk)
{
	struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;

	if (unlikely(mpcb->list_rcvd)) {
		mpcb->list_rcvd = 0;
		if (mpcb->pm_ops->new_remote_address)
			mpcb->pm_ops->new_remote_address(meta_sk);
	}
}

bool mptcp_handle_options(struct sock *sk, const struct tcphdr *th,
			  const struct sk_buff *skb)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct mptcp_options_received *mopt = &tp->mptcp->rx_opt;
	struct mptcp_cb *mpcb = tp->mpcb;

	if (tp->mpcb->infinite_mapping_rcv || tp->mpcb->infinite_mapping_snd)
		return false;

	if (mptcp_mp_fastclose_rcvd(sk))
		return true;

	if (sk->sk_state == TCP_RST_WAIT && !th->rst)
		return true;

	if (mopt->saw_mpc && !tp->mpcb->rem_key_set)
		mptcp_initialize_recv_vars(mptcp_meta_tp(tp), tp->mpcb,
					   mopt->mptcp_sender_key);

	if (unlikely(mopt->mp_fail) && mptcp_mp_fail_rcvd(sk, th))
		return true;

	/* RFC 6824, Section 3.3:
	 * If a checksum is not present when its use has been negotiated, the
	 * receiver MUST close the subflow with a RST as it is considered broken.
	 */
	if ((mptcp_is_data_seq(skb) || mptcp_is_data_mpcapable(skb)) &&
	    tp->mpcb->dss_csum &&
	    !(TCP_SKB_CB(skb)->mptcp_flags & MPTCPHDR_DSS_CSUM)) {
		mptcp_send_reset(sk);
		return true;
	}

	/* We have to acknowledge retransmissions of the third
	 * ack.
	 */
	if (mopt->join_ack) {
		tcp_send_delayed_ack(sk);
		mopt->join_ack = 0;
	}

	if (mopt->saw_add_addr || mopt->saw_rem_addr) {
		if (mopt->more_add_addr || mopt->more_rem_addr) {
			mptcp_parse_addropt(skb, sk);
		} else {
			if (mopt->saw_add_addr)
				mptcp_handle_add_addr(mopt->add_addr_ptr, sk);
			if (mopt->saw_rem_addr)
				mptcp_handle_rem_addr(mopt->rem_addr_ptr, sk);
		}

		mopt->more_add_addr = 0;
		mopt->saw_add_addr = 0;
		mopt->more_rem_addr = 0;
		mopt->saw_rem_addr = 0;
	}
	if (mopt->saw_low_prio) {
		if (mopt->saw_low_prio == 1) {
			tp->mptcp->rcv_low_prio = mopt->low_prio;
			if (mpcb->pm_ops->prio_changed)
				mpcb->pm_ops->prio_changed(sk, mopt->low_prio);
		} else {
			struct mptcp_tcp_sock *mptcp;

			mptcp_for_each_sub(tp->mpcb, mptcp) {
				if (mptcp->rem_id == mopt->prio_addr_id) {
					mptcp->rcv_low_prio = mopt->low_prio;
					if (mpcb->pm_ops->prio_changed)
						mpcb->pm_ops->prio_changed(sk,
									   mopt->low_prio);
				}
			}
		}
		mopt->saw_low_prio = 0;
	}

	if (mptcp_process_data_ack(sk, skb))
		return true;

	mptcp_path_array_check(mptcp_meta_sk(sk));
	/* Socket may have been mp_killed by a REMOVE_ADDR */
	if (tp->mp_killed)
		return true;

	return false;
}

static void _mptcp_rcv_synsent_fastopen(struct sock *meta_sk,
					struct sk_buff *skb, bool rtx_queue)
{
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct tcp_sock *master_tp = tcp_sk(meta_tp->mpcb->master_sk);
	u32 new_mapping = meta_tp->write_seq - master_tp->snd_una;

	/* If the server only acknowledges partially the data sent in
	 * the SYN, we need to trim the acknowledged part because
	 * we don't want to retransmit this already received data.
	 * When we reach this point, tcp_ack() has already cleaned up
	 * fully acked segments. However, tcp trims partially acked
	 * segments only when retransmitting. Since MPTCP comes into
	 * play only now, we will fake an initial transmit, and
	 * retransmit_skb() will not be called. The following fragment
	 * comes from __tcp_retransmit_skb().
	 */
	if (before(TCP_SKB_CB(skb)->seq, master_tp->snd_una)) {
		BUG_ON(before(TCP_SKB_CB(skb)->end_seq, master_tp->snd_una));
		/* tcp_trim_head can only returns ENOMEM if skb is
		 * cloned. It is not the case here (see
		 * tcp_send_syn_data).
		 */
		BUG_ON(tcp_trim_head(meta_sk, skb, master_tp->snd_una -
				     TCP_SKB_CB(skb)->seq));
	}

	TCP_SKB_CB(skb)->seq += new_mapping;
	TCP_SKB_CB(skb)->end_seq += new_mapping;
	TCP_SKB_CB(skb)->sacked = 0;

	list_del(&skb->tcp_tsorted_anchor);

	if (rtx_queue)
		tcp_rtx_queue_unlink(skb, meta_sk);

	INIT_LIST_HEAD(&skb->tcp_tsorted_anchor);

	if (rtx_queue)
		tcp_add_write_queue_tail(meta_sk, skb);
}

/* In case of fastopen, some data can already be in the write queue.
 * We need to update the sequence number of the segments as they
 * were initially TCP sequence numbers.
 */
static void mptcp_rcv_synsent_fastopen(struct sock *meta_sk)
{
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct tcp_sock *master_tp = tcp_sk(meta_tp->mpcb->master_sk);
	struct sk_buff *skb_write_head, *skb_rtx_head, *tmp;

	skb_write_head = tcp_write_queue_head(meta_sk);
	skb_rtx_head = tcp_rtx_queue_head(meta_sk);

	if (!(skb_write_head || skb_rtx_head))
		return;

	/* There should only be one skb in {write, rtx} queue: the data not
	 * acknowledged in the SYN+ACK. In this case, we need to map
	 * this data to data sequence numbers.
	 */

	BUG_ON(skb_write_head && skb_rtx_head);

	if (skb_write_head) {
		skb_queue_walk_from_safe(&meta_sk->sk_write_queue,
					 skb_write_head, tmp) {
			_mptcp_rcv_synsent_fastopen(meta_sk, skb_write_head,
						    false);
		}
	}

	if (skb_rtx_head) {
		skb_rbtree_walk_from_safe(skb_rtx_head, tmp) {
			_mptcp_rcv_synsent_fastopen(meta_sk, skb_rtx_head,
						    true);
		}
	}

	/* We can advance write_seq by the number of bytes unacknowledged
	 * and that were mapped in the previous loop.
	 */
	meta_tp->write_seq += master_tp->write_seq - master_tp->snd_una;

	/* The packets from the master_sk will be entailed to it later
	 * Until that time, its write queue is empty, and
	 * write_seq must align with snd_una
	 */
	master_tp->snd_nxt = master_tp->write_seq = master_tp->snd_una;
	master_tp->packets_out = 0;
	tcp_clear_retrans(meta_tp);
	tcp_clear_retrans(master_tp);
	tcp_set_ca_state(meta_tp->mpcb->master_sk, TCP_CA_Open);
	tcp_set_ca_state(meta_sk, TCP_CA_Open);
}

/* The skptr is needed, because if we become MPTCP-capable, we have to switch
 * from meta-socket to master-socket.
 *
 * @return: 1 - we want to reset this connection
 *	    2 - we want to discard the received syn/ack
 *	    0 - everything is fine - continue
 */
int mptcp_rcv_synsent_state_process(struct sock *sk, struct sock **skptr,
				    const struct sk_buff *skb,
				    const struct mptcp_options_received *mopt)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (mptcp(tp)) {
		u8 hash_mac_check[SHA256_DIGEST_SIZE];
		struct mptcp_cb *mpcb = tp->mpcb;

		mptcp_hmac(mpcb->mptcp_ver, (u8 *)&mpcb->mptcp_rem_key,
			   (u8 *)&mpcb->mptcp_loc_key, hash_mac_check, 2,
			   4, (u8 *)&tp->mptcp->rx_opt.mptcp_recv_nonce,
			   4, (u8 *)&tp->mptcp->mptcp_loc_nonce);
		if (memcmp(hash_mac_check,
			   (char *)&tp->mptcp->rx_opt.mptcp_recv_tmac, 8)) {
			MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_JOINSYNACKMAC);
			mptcp_sub_force_close(sk);
			return 1;
		}

		/* Set this flag in order to postpone data sending
		 * until the 4th ack arrives.
		 */
		tp->mptcp->pre_established = 1;
		tp->mptcp->rcv_low_prio = tp->mptcp->rx_opt.low_prio;

		mptcp_hmac(mpcb->mptcp_ver, (u8 *)&mpcb->mptcp_loc_key,
			   (u8 *)&mpcb->mptcp_rem_key,
			   tp->mptcp->sender_mac, 2,
			   4, (u8 *)&tp->mptcp->mptcp_loc_nonce,
			   4, (u8 *)&tp->mptcp->rx_opt.mptcp_recv_nonce);

		MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_JOINSYNACKRX);
	} else if (mopt->saw_mpc) {
		struct sock *meta_sk = sk;

		MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_MPCAPABLEACTIVEACK);
		if (mopt->mptcp_ver > tcp_sk(sk)->mptcp_ver)
			/* TODO Consider adding new MPTCP_INC_STATS entry */
			goto fallback;
		if (tcp_sk(sk)->mptcp_ver == MPTCP_VERSION_1 &&
		    mopt->mptcp_ver < MPTCP_VERSION_1)
			/* TODO Consider adding new MPTCP_INC_STATS entry */
			/* TODO - record this in the cache - use v0 next time */
			goto fallback;

		if (mptcp_create_master_sk(sk, mopt->mptcp_sender_key, 1,
					   mopt->mptcp_ver,
					   ntohs(tcp_hdr(skb)->window)))
			return 2;

		sk = tcp_sk(sk)->mpcb->master_sk;
		*skptr = sk;
		tp = tcp_sk(sk);

		/* If fastopen was used data might be in the send queue. We
		 * need to update their sequence number to MPTCP-level seqno.
		 * Note that it can happen in rare cases that fastopen_req is
		 * NULL and syn_data is 0 but fastopen indeed occurred and
		 * data has been queued in the write queue (but not sent).
		 * Example of such rare cases: connect is non-blocking and
		 * TFO is configured to work without cookies.
		 */
		mptcp_rcv_synsent_fastopen(meta_sk);

		/* -1, because the SYN consumed 1 byte. In case of TFO, we
		 * start the subflow-sequence number as if the data of the SYN
		 * is not part of any mapping.
		 */
		tp->mptcp->snt_isn = tp->snd_una - 1;
		tp->mpcb->dss_csum = mopt->dss_csum;
		if (tp->mpcb->dss_csum)
			MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_CSUMENABLED);

		if (tp->mpcb->mptcp_ver >= MPTCP_VERSION_1)
			tp->mpcb->send_mptcpv1_mpcapable = 1;

		tp->mptcp->include_mpc = 1;

		sk_set_socket(sk, meta_sk->sk_socket);
		sk->sk_wq = meta_sk->sk_wq;

		bh_unlock_sock(sk);
		 /* hold in sk_clone_lock due to initialization to 2 */
		sock_put(sk);
	} else {
		MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_MPCAPABLEACTIVEFALLBACK);
fallback:
		tp->request_mptcp = 0;

		if (tp->inside_tk_table)
			mptcp_hash_remove_bh(tp);
	}

	if (mptcp(tp))
		tp->mptcp->rcv_isn = TCP_SKB_CB(skb)->seq;

	return 0;
}

/* Similar to tcp_should_expand_sndbuf */
bool mptcp_should_expand_sndbuf(const struct sock *sk)
{
	const struct sock *meta_sk = mptcp_meta_sk(sk);
	const struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	const struct mptcp_tcp_sock *mptcp;

	/* We circumvent this check in tcp_check_space, because we want to
	 * always call sk_write_space. So, we reproduce the check here.
	 */
	if (!meta_sk->sk_socket ||
	    !test_bit(SOCK_NOSPACE, &meta_sk->sk_socket->flags))
		return false;

	/* If the user specified a specific send buffer setting, do
	 * not modify it.
	 */
	if (meta_sk->sk_userlocks & SOCK_SNDBUF_LOCK)
		return false;

	/* If we are under global TCP memory pressure, do not expand.  */
	if (tcp_under_memory_pressure(meta_sk))
		return false;

	/* If we are under soft global TCP memory pressure, do not expand.  */
	if (sk_memory_allocated(meta_sk) >= sk_prot_mem_limits(meta_sk, 0))
		return false;

	/* For MPTCP we look for a subsocket that could send data.
	 * If we found one, then we update the send-buffer.
	 */
	mptcp_for_each_sub(meta_tp->mpcb, mptcp) {
		const struct sock *sk_it = mptcp_to_sock(mptcp);
		const struct tcp_sock *tp_it = tcp_sk(sk_it);

		if (!mptcp_sk_can_send(sk_it))
			continue;

		if (tcp_packets_in_flight(tp_it) < tp_it->snd_cwnd)
			return true;
	}

	return false;
}

void mptcp_tcp_set_rto(struct sock *sk)
{
	tcp_set_rto(sk);
	mptcp_set_rto(sk);
}
