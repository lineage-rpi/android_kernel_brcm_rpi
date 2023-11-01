/*
 * net/sched/cls_flower.c		Flower classifier
 *
 * Copyright (c) 2015 Jiri Pirko <jiri@resnulli.us>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/rhashtable.h>
#include <linux/workqueue.h>

#include <linux/if_ether.h>
#include <linux/in6.h>
#include <linux/ip.h>
#include <linux/mpls.h>

#include <net/sch_generic.h>
#include <net/pkt_cls.h>
#include <net/ip.h>
#include <net/flow_dissector.h>
#include <net/geneve.h>

#include <net/dst.h>
#include <net/dst_metadata.h>

struct fl_flow_key {
	int	indev_ifindex;
	struct flow_dissector_key_control control;
	struct flow_dissector_key_control enc_control;
	struct flow_dissector_key_basic basic;
	struct flow_dissector_key_eth_addrs eth;
	struct flow_dissector_key_vlan vlan;
	struct flow_dissector_key_vlan cvlan;
	union {
		struct flow_dissector_key_ipv4_addrs ipv4;
		struct flow_dissector_key_ipv6_addrs ipv6;
	};
	struct flow_dissector_key_ports tp;
	struct flow_dissector_key_icmp icmp;
	struct flow_dissector_key_arp arp;
	struct flow_dissector_key_keyid enc_key_id;
	union {
		struct flow_dissector_key_ipv4_addrs enc_ipv4;
		struct flow_dissector_key_ipv6_addrs enc_ipv6;
	};
	struct flow_dissector_key_ports enc_tp;
	struct flow_dissector_key_mpls mpls;
	struct flow_dissector_key_tcp tcp;
	struct flow_dissector_key_ip ip;
	struct flow_dissector_key_ip enc_ip;
	struct flow_dissector_key_enc_opts enc_opts;
} __aligned(BITS_PER_LONG / 8); /* Ensure that we can do comparisons as longs. */

struct fl_flow_mask_range {
	unsigned short int start;
	unsigned short int end;
};

struct fl_flow_mask {
	struct fl_flow_key key;
	struct fl_flow_mask_range range;
	struct rhash_head ht_node;
	struct rhashtable ht;
	struct rhashtable_params filter_ht_params;
	struct flow_dissector dissector;
	struct list_head filters;
	struct rcu_work rwork;
	struct list_head list;
};

struct fl_flow_tmplt {
	struct fl_flow_key dummy_key;
	struct fl_flow_key mask;
	struct flow_dissector dissector;
	struct tcf_chain *chain;
};

struct cls_fl_head {
	struct rhashtable ht;
	struct list_head masks;
	struct rcu_work rwork;
	struct idr handle_idr;
};

struct cls_fl_filter {
	struct fl_flow_mask *mask;
	struct rhash_head ht_node;
	struct fl_flow_key mkey;
	struct tcf_exts exts;
	struct tcf_result res;
	struct fl_flow_key key;
	struct list_head list;
	u32 handle;
	u32 flags;
	unsigned int in_hw_count;
	struct rcu_work rwork;
	struct net_device *hw_dev;
};

static const struct rhashtable_params mask_ht_params = {
	.key_offset = offsetof(struct fl_flow_mask, key),
	.key_len = sizeof(struct fl_flow_key),
	.head_offset = offsetof(struct fl_flow_mask, ht_node),
	.automatic_shrinking = true,
};

static unsigned short int fl_mask_range(const struct fl_flow_mask *mask)
{
	return mask->range.end - mask->range.start;
}

static void fl_mask_update_range(struct fl_flow_mask *mask)
{
	const u8 *bytes = (const u8 *) &mask->key;
	size_t size = sizeof(mask->key);
	size_t i, first = 0, last;

	for (i = 0; i < size; i++) {
		if (bytes[i]) {
			first = i;
			break;
		}
	}
	last = first;
	for (i = size - 1; i != first; i--) {
		if (bytes[i]) {
			last = i;
			break;
		}
	}
	mask->range.start = rounddown(first, sizeof(long));
	mask->range.end = roundup(last + 1, sizeof(long));
}

static void *fl_key_get_start(struct fl_flow_key *key,
			      const struct fl_flow_mask *mask)
{
	return (u8 *) key + mask->range.start;
}

static void fl_set_masked_key(struct fl_flow_key *mkey, struct fl_flow_key *key,
			      struct fl_flow_mask *mask)
{
	const long *lkey = fl_key_get_start(key, mask);
	const long *lmask = fl_key_get_start(&mask->key, mask);
	long *lmkey = fl_key_get_start(mkey, mask);
	int i;

	for (i = 0; i < fl_mask_range(mask); i += sizeof(long))
		*lmkey++ = *lkey++ & *lmask++;
}

static bool fl_mask_fits_tmplt(struct fl_flow_tmplt *tmplt,
			       struct fl_flow_mask *mask)
{
	const long *lmask = fl_key_get_start(&mask->key, mask);
	const long *ltmplt;
	int i;

	if (!tmplt)
		return true;
	ltmplt = fl_key_get_start(&tmplt->mask, mask);
	for (i = 0; i < fl_mask_range(mask); i += sizeof(long)) {
		if (~*ltmplt++ & *lmask++)
			return false;
	}
	return true;
}

static void fl_clear_masked_range(struct fl_flow_key *key,
				  struct fl_flow_mask *mask)
{
	memset(fl_key_get_start(key, mask), 0, fl_mask_range(mask));
}

static struct cls_fl_filter *fl_lookup(struct fl_flow_mask *mask,
				       struct fl_flow_key *mkey)
{
	return rhashtable_lookup_fast(&mask->ht, fl_key_get_start(mkey, mask),
				      mask->filter_ht_params);
}

static int fl_classify(struct sk_buff *skb, const struct tcf_proto *tp,
		       struct tcf_result *res)
{
	struct cls_fl_head *head = rcu_dereference_bh(tp->root);
	struct cls_fl_filter *f;
	struct fl_flow_mask *mask;
	struct fl_flow_key skb_key;
	struct fl_flow_key skb_mkey;

	list_for_each_entry_rcu(mask, &head->masks, list) {
		flow_dissector_init_keys(&skb_key.control, &skb_key.basic);
		fl_clear_masked_range(&skb_key, mask);

		skb_key.indev_ifindex = skb->skb_iif;
		/* skb_flow_dissect() does not set n_proto in case an unknown
		 * protocol, so do it rather here.
		 */
		skb_key.basic.n_proto = skb_protocol(skb, false);
		skb_flow_dissect_tunnel_info(skb, &mask->dissector, &skb_key);
		skb_flow_dissect(skb, &mask->dissector, &skb_key, 0);

		fl_set_masked_key(&skb_mkey, &skb_key, mask);

		f = fl_lookup(mask, &skb_mkey);
		if (f && !tc_skip_sw(f->flags)) {
			*res = f->res;
			return tcf_exts_exec(skb, &f->exts, res);
		}
	}
	return -1;
}

static int fl_init(struct tcf_proto *tp)
{
	struct cls_fl_head *head;

	head = kzalloc(sizeof(*head), GFP_KERNEL);
	if (!head)
		return -ENOBUFS;

	INIT_LIST_HEAD_RCU(&head->masks);
	rcu_assign_pointer(tp->root, head);
	idr_init(&head->handle_idr);

	return rhashtable_init(&head->ht, &mask_ht_params);
}

static void fl_mask_free(struct fl_flow_mask *mask)
{
	rhashtable_destroy(&mask->ht);
	kfree(mask);
}

static void fl_mask_free_work(struct work_struct *work)
{
	struct fl_flow_mask *mask = container_of(to_rcu_work(work),
						 struct fl_flow_mask, rwork);

	fl_mask_free(mask);
}

static bool fl_mask_put(struct cls_fl_head *head, struct fl_flow_mask *mask,
			bool async)
{
	if (!list_empty(&mask->filters))
		return false;

	rhashtable_remove_fast(&head->ht, &mask->ht_node, mask_ht_params);
	list_del_rcu(&mask->list);
	if (async)
		tcf_queue_work(&mask->rwork, fl_mask_free_work);
	else
		fl_mask_free(mask);

	return true;
}

static void __fl_destroy_filter(struct cls_fl_filter *f)
{
	tcf_exts_destroy(&f->exts);
	tcf_exts_put_net(&f->exts);
	kfree(f);
}

static void fl_destroy_filter_work(struct work_struct *work)
{
	struct cls_fl_filter *f = container_of(to_rcu_work(work),
					struct cls_fl_filter, rwork);

	rtnl_lock();
	__fl_destroy_filter(f);
	rtnl_unlock();
}

static void fl_hw_destroy_filter(struct tcf_proto *tp, struct cls_fl_filter *f,
				 struct netlink_ext_ack *extack)
{
	struct tc_cls_flower_offload cls_flower = {};
	struct tcf_block *block = tp->chain->block;

	tc_cls_common_offload_init(&cls_flower.common, tp, f->flags, extack);
	cls_flower.command = TC_CLSFLOWER_DESTROY;
	cls_flower.cookie = (unsigned long) f;

	tc_setup_cb_call(block, &f->exts, TC_SETUP_CLSFLOWER,
			 &cls_flower, false);
	tcf_block_offload_dec(block, &f->flags);
}

static int fl_hw_replace_filter(struct tcf_proto *tp,
				struct cls_fl_filter *f,
				struct netlink_ext_ack *extack)
{
	struct tc_cls_flower_offload cls_flower = {};
	struct tcf_block *block = tp->chain->block;
	bool skip_sw = tc_skip_sw(f->flags);
	int err;

	tc_cls_common_offload_init(&cls_flower.common, tp, f->flags, extack);
	cls_flower.command = TC_CLSFLOWER_REPLACE;
	cls_flower.cookie = (unsigned long) f;
	cls_flower.dissector = &f->mask->dissector;
	cls_flower.mask = &f->mask->key;
	cls_flower.key = &f->mkey;
	cls_flower.exts = &f->exts;
	cls_flower.classid = f->res.classid;

	err = tc_setup_cb_call(block, &f->exts, TC_SETUP_CLSFLOWER,
			       &cls_flower, skip_sw);
	if (err < 0) {
		fl_hw_destroy_filter(tp, f, NULL);
		return err;
	} else if (err > 0) {
		f->in_hw_count = err;
		tcf_block_offload_inc(block, &f->flags);
	}

	if (skip_sw && !(f->flags & TCA_CLS_FLAGS_IN_HW))
		return -EINVAL;

	return 0;
}

static void fl_hw_update_stats(struct tcf_proto *tp, struct cls_fl_filter *f)
{
	struct tc_cls_flower_offload cls_flower = {};
	struct tcf_block *block = tp->chain->block;

	tc_cls_common_offload_init(&cls_flower.common, tp, f->flags, NULL);
	cls_flower.command = TC_CLSFLOWER_STATS;
	cls_flower.cookie = (unsigned long) f;
	cls_flower.exts = &f->exts;
	cls_flower.classid = f->res.classid;

	tc_setup_cb_call(block, &f->exts, TC_SETUP_CLSFLOWER,
			 &cls_flower, false);
}

static bool __fl_delete(struct tcf_proto *tp, struct cls_fl_filter *f,
			struct netlink_ext_ack *extack)
{
	struct cls_fl_head *head = rtnl_dereference(tp->root);
	bool async = tcf_exts_get_net(&f->exts);
	bool last;

	idr_remove(&head->handle_idr, f->handle);
	list_del_rcu(&f->list);
	last = fl_mask_put(head, f->mask, async);
	if (!tc_skip_hw(f->flags))
		fl_hw_destroy_filter(tp, f, extack);
	tcf_unbind_filter(tp, &f->res);
	if (async)
		tcf_queue_work(&f->rwork, fl_destroy_filter_work);
	else
		__fl_destroy_filter(f);

	return last;
}

static void fl_destroy_sleepable(struct work_struct *work)
{
	struct cls_fl_head *head = container_of(to_rcu_work(work),
						struct cls_fl_head,
						rwork);

	rhashtable_destroy(&head->ht);
	kfree(head);
	module_put(THIS_MODULE);
}

static void fl_destroy(struct tcf_proto *tp, struct netlink_ext_ack *extack)
{
	struct cls_fl_head *head = rtnl_dereference(tp->root);
	struct fl_flow_mask *mask, *next_mask;
	struct cls_fl_filter *f, *next;

	list_for_each_entry_safe(mask, next_mask, &head->masks, list) {
		list_for_each_entry_safe(f, next, &mask->filters, list) {
			if (__fl_delete(tp, f, extack))
				break;
		}
	}
	idr_destroy(&head->handle_idr);

	__module_get(THIS_MODULE);
	tcf_queue_work(&head->rwork, fl_destroy_sleepable);
}

static void *fl_get(struct tcf_proto *tp, u32 handle)
{
	struct cls_fl_head *head = rtnl_dereference(tp->root);

	return idr_find(&head->handle_idr, handle);
}

static const struct nla_policy fl_policy[TCA_FLOWER_MAX + 1] = {
	[TCA_FLOWER_UNSPEC]		= { .type = NLA_UNSPEC },
	[TCA_FLOWER_CLASSID]		= { .type = NLA_U32 },
	[TCA_FLOWER_INDEV]		= { .type = NLA_STRING,
					    .len = IFNAMSIZ },
	[TCA_FLOWER_KEY_ETH_DST]	= { .len = ETH_ALEN },
	[TCA_FLOWER_KEY_ETH_DST_MASK]	= { .len = ETH_ALEN },
	[TCA_FLOWER_KEY_ETH_SRC]	= { .len = ETH_ALEN },
	[TCA_FLOWER_KEY_ETH_SRC_MASK]	= { .len = ETH_ALEN },
	[TCA_FLOWER_KEY_ETH_TYPE]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_IP_PROTO]	= { .type = NLA_U8 },
	[TCA_FLOWER_KEY_IPV4_SRC]	= { .type = NLA_U32 },
	[TCA_FLOWER_KEY_IPV4_SRC_MASK]	= { .type = NLA_U32 },
	[TCA_FLOWER_KEY_IPV4_DST]	= { .type = NLA_U32 },
	[TCA_FLOWER_KEY_IPV4_DST_MASK]	= { .type = NLA_U32 },
	[TCA_FLOWER_KEY_IPV6_SRC]	= { .len = sizeof(struct in6_addr) },
	[TCA_FLOWER_KEY_IPV6_SRC_MASK]	= { .len = sizeof(struct in6_addr) },
	[TCA_FLOWER_KEY_IPV6_DST]	= { .len = sizeof(struct in6_addr) },
	[TCA_FLOWER_KEY_IPV6_DST_MASK]	= { .len = sizeof(struct in6_addr) },
	[TCA_FLOWER_KEY_TCP_SRC]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_TCP_DST]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_UDP_SRC]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_UDP_DST]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_VLAN_ID]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_VLAN_PRIO]	= { .type = NLA_U8 },
	[TCA_FLOWER_KEY_VLAN_ETH_TYPE]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_ENC_KEY_ID]	= { .type = NLA_U32 },
	[TCA_FLOWER_KEY_ENC_IPV4_SRC]	= { .type = NLA_U32 },
	[TCA_FLOWER_KEY_ENC_IPV4_SRC_MASK] = { .type = NLA_U32 },
	[TCA_FLOWER_KEY_ENC_IPV4_DST]	= { .type = NLA_U32 },
	[TCA_FLOWER_KEY_ENC_IPV4_DST_MASK] = { .type = NLA_U32 },
	[TCA_FLOWER_KEY_ENC_IPV6_SRC]	= { .len = sizeof(struct in6_addr) },
	[TCA_FLOWER_KEY_ENC_IPV6_SRC_MASK] = { .len = sizeof(struct in6_addr) },
	[TCA_FLOWER_KEY_ENC_IPV6_DST]	= { .len = sizeof(struct in6_addr) },
	[TCA_FLOWER_KEY_ENC_IPV6_DST_MASK] = { .len = sizeof(struct in6_addr) },
	[TCA_FLOWER_KEY_TCP_SRC_MASK]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_TCP_DST_MASK]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_UDP_SRC_MASK]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_UDP_DST_MASK]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_SCTP_SRC_MASK]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_SCTP_DST_MASK]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_SCTP_SRC]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_SCTP_DST]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_ENC_UDP_SRC_PORT]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_ENC_UDP_SRC_PORT_MASK]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_ENC_UDP_DST_PORT]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_ENC_UDP_DST_PORT_MASK]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_FLAGS]		= { .type = NLA_U32 },
	[TCA_FLOWER_KEY_FLAGS_MASK]	= { .type = NLA_U32 },
	[TCA_FLOWER_KEY_ICMPV4_TYPE]	= { .type = NLA_U8 },
	[TCA_FLOWER_KEY_ICMPV4_TYPE_MASK] = { .type = NLA_U8 },
	[TCA_FLOWER_KEY_ICMPV4_CODE]	= { .type = NLA_U8 },
	[TCA_FLOWER_KEY_ICMPV4_CODE_MASK] = { .type = NLA_U8 },
	[TCA_FLOWER_KEY_ICMPV6_TYPE]	= { .type = NLA_U8 },
	[TCA_FLOWER_KEY_ICMPV6_TYPE_MASK] = { .type = NLA_U8 },
	[TCA_FLOWER_KEY_ICMPV6_CODE]	= { .type = NLA_U8 },
	[TCA_FLOWER_KEY_ICMPV6_CODE_MASK] = { .type = NLA_U8 },
	[TCA_FLOWER_KEY_ARP_SIP]	= { .type = NLA_U32 },
	[TCA_FLOWER_KEY_ARP_SIP_MASK]	= { .type = NLA_U32 },
	[TCA_FLOWER_KEY_ARP_TIP]	= { .type = NLA_U32 },
	[TCA_FLOWER_KEY_ARP_TIP_MASK]	= { .type = NLA_U32 },
	[TCA_FLOWER_KEY_ARP_OP]		= { .type = NLA_U8 },
	[TCA_FLOWER_KEY_ARP_OP_MASK]	= { .type = NLA_U8 },
	[TCA_FLOWER_KEY_ARP_SHA]	= { .len = ETH_ALEN },
	[TCA_FLOWER_KEY_ARP_SHA_MASK]	= { .len = ETH_ALEN },
	[TCA_FLOWER_KEY_ARP_THA]	= { .len = ETH_ALEN },
	[TCA_FLOWER_KEY_ARP_THA_MASK]	= { .len = ETH_ALEN },
	[TCA_FLOWER_KEY_MPLS_TTL]	= { .type = NLA_U8 },
	[TCA_FLOWER_KEY_MPLS_BOS]	= { .type = NLA_U8 },
	[TCA_FLOWER_KEY_MPLS_TC]	= { .type = NLA_U8 },
	[TCA_FLOWER_KEY_MPLS_LABEL]	= { .type = NLA_U32 },
	[TCA_FLOWER_KEY_TCP_FLAGS]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_TCP_FLAGS_MASK]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_IP_TOS]		= { .type = NLA_U8 },
	[TCA_FLOWER_KEY_IP_TOS_MASK]	= { .type = NLA_U8 },
	[TCA_FLOWER_KEY_IP_TTL]		= { .type = NLA_U8 },
	[TCA_FLOWER_KEY_IP_TTL_MASK]	= { .type = NLA_U8 },
	[TCA_FLOWER_KEY_CVLAN_ID]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_CVLAN_PRIO]	= { .type = NLA_U8 },
	[TCA_FLOWER_KEY_CVLAN_ETH_TYPE]	= { .type = NLA_U16 },
	[TCA_FLOWER_KEY_ENC_IP_TOS]	= { .type = NLA_U8 },
	[TCA_FLOWER_KEY_ENC_IP_TOS_MASK] = { .type = NLA_U8 },
	[TCA_FLOWER_KEY_ENC_IP_TTL]	 = { .type = NLA_U8 },
	[TCA_FLOWER_KEY_ENC_IP_TTL_MASK] = { .type = NLA_U8 },
	[TCA_FLOWER_KEY_ENC_OPTS]	= { .type = NLA_NESTED },
	[TCA_FLOWER_KEY_ENC_OPTS_MASK]	= { .type = NLA_NESTED },
	[TCA_FLOWER_FLAGS]		= { .type = NLA_U32 },
};

static const struct nla_policy
enc_opts_policy[TCA_FLOWER_KEY_ENC_OPTS_MAX + 1] = {
	[TCA_FLOWER_KEY_ENC_OPTS_GENEVE]        = { .type = NLA_NESTED },
};

static const struct nla_policy
geneve_opt_policy[TCA_FLOWER_KEY_ENC_OPT_GENEVE_MAX + 1] = {
	[TCA_FLOWER_KEY_ENC_OPT_GENEVE_CLASS]      = { .type = NLA_U16 },
	[TCA_FLOWER_KEY_ENC_OPT_GENEVE_TYPE]       = { .type = NLA_U8 },
	[TCA_FLOWER_KEY_ENC_OPT_GENEVE_DATA]       = { .type = NLA_BINARY,
						       .len = 128 },
};

static void fl_set_key_val(struct nlattr **tb,
			   void *val, int val_type,
			   void *mask, int mask_type, int len)
{
	if (!tb[val_type])
		return;
	memcpy(val, nla_data(tb[val_type]), len);
	if (mask_type == TCA_FLOWER_UNSPEC || !tb[mask_type])
		memset(mask, 0xff, len);
	else
		memcpy(mask, nla_data(tb[mask_type]), len);
}

static int fl_set_key_mpls(struct nlattr **tb,
			   struct flow_dissector_key_mpls *key_val,
			   struct flow_dissector_key_mpls *key_mask)
{
	if (tb[TCA_FLOWER_KEY_MPLS_TTL]) {
		key_val->mpls_ttl = nla_get_u8(tb[TCA_FLOWER_KEY_MPLS_TTL]);
		key_mask->mpls_ttl = MPLS_TTL_MASK;
	}
	if (tb[TCA_FLOWER_KEY_MPLS_BOS]) {
		u8 bos = nla_get_u8(tb[TCA_FLOWER_KEY_MPLS_BOS]);

		if (bos & ~MPLS_BOS_MASK)
			return -EINVAL;
		key_val->mpls_bos = bos;
		key_mask->mpls_bos = MPLS_BOS_MASK;
	}
	if (tb[TCA_FLOWER_KEY_MPLS_TC]) {
		u8 tc = nla_get_u8(tb[TCA_FLOWER_KEY_MPLS_TC]);

		if (tc & ~MPLS_TC_MASK)
			return -EINVAL;
		key_val->mpls_tc = tc;
		key_mask->mpls_tc = MPLS_TC_MASK;
	}
	if (tb[TCA_FLOWER_KEY_MPLS_LABEL]) {
		u32 label = nla_get_u32(tb[TCA_FLOWER_KEY_MPLS_LABEL]);

		if (label & ~MPLS_LABEL_MASK)
			return -EINVAL;
		key_val->mpls_label = label;
		key_mask->mpls_label = MPLS_LABEL_MASK;
	}
	return 0;
}

static void fl_set_key_vlan(struct nlattr **tb,
			    __be16 ethertype,
			    int vlan_id_key, int vlan_prio_key,
			    int vlan_next_eth_type_key,
			    struct flow_dissector_key_vlan *key_val,
			    struct flow_dissector_key_vlan *key_mask)
{
#define VLAN_PRIORITY_MASK	0x7

	if (tb[vlan_id_key]) {
		key_val->vlan_id =
			nla_get_u16(tb[vlan_id_key]) & VLAN_VID_MASK;
		key_mask->vlan_id = VLAN_VID_MASK;
	}
	if (tb[vlan_prio_key]) {
		key_val->vlan_priority =
			nla_get_u8(tb[vlan_prio_key]) &
			VLAN_PRIORITY_MASK;
		key_mask->vlan_priority = VLAN_PRIORITY_MASK;
	}
	key_val->vlan_tpid = ethertype;
	key_mask->vlan_tpid = cpu_to_be16(~0);
	if (tb[vlan_next_eth_type_key]) {
		key_val->vlan_eth_type =
			nla_get_be16(tb[vlan_next_eth_type_key]);
		key_mask->vlan_eth_type = cpu_to_be16(~0);
	}
}

static void fl_set_key_flag(u32 flower_key, u32 flower_mask,
			    u32 *dissector_key, u32 *dissector_mask,
			    u32 flower_flag_bit, u32 dissector_flag_bit)
{
	if (flower_mask & flower_flag_bit) {
		*dissector_mask |= dissector_flag_bit;
		if (flower_key & flower_flag_bit)
			*dissector_key |= dissector_flag_bit;
	}
}

static int fl_set_key_flags(struct nlattr **tb,
			    u32 *flags_key, u32 *flags_mask)
{
	u32 key, mask;

	/* mask is mandatory for flags */
	if (!tb[TCA_FLOWER_KEY_FLAGS_MASK])
		return -EINVAL;

	key = be32_to_cpu(nla_get_u32(tb[TCA_FLOWER_KEY_FLAGS]));
	mask = be32_to_cpu(nla_get_u32(tb[TCA_FLOWER_KEY_FLAGS_MASK]));

	*flags_key  = 0;
	*flags_mask = 0;

	fl_set_key_flag(key, mask, flags_key, flags_mask,
			TCA_FLOWER_KEY_FLAGS_IS_FRAGMENT, FLOW_DIS_IS_FRAGMENT);
	fl_set_key_flag(key, mask, flags_key, flags_mask,
			TCA_FLOWER_KEY_FLAGS_FRAG_IS_FIRST,
			FLOW_DIS_FIRST_FRAG);

	return 0;
}

static void fl_set_key_ip(struct nlattr **tb, bool encap,
			  struct flow_dissector_key_ip *key,
			  struct flow_dissector_key_ip *mask)
{
	int tos_key = encap ? TCA_FLOWER_KEY_ENC_IP_TOS : TCA_FLOWER_KEY_IP_TOS;
	int ttl_key = encap ? TCA_FLOWER_KEY_ENC_IP_TTL : TCA_FLOWER_KEY_IP_TTL;
	int tos_mask = encap ? TCA_FLOWER_KEY_ENC_IP_TOS_MASK : TCA_FLOWER_KEY_IP_TOS_MASK;
	int ttl_mask = encap ? TCA_FLOWER_KEY_ENC_IP_TTL_MASK : TCA_FLOWER_KEY_IP_TTL_MASK;

	fl_set_key_val(tb, &key->tos, tos_key, &mask->tos, tos_mask, sizeof(key->tos));
	fl_set_key_val(tb, &key->ttl, ttl_key, &mask->ttl, ttl_mask, sizeof(key->ttl));
}

static int fl_set_geneve_opt(const struct nlattr *nla, struct fl_flow_key *key,
			     int depth, int option_len,
			     struct netlink_ext_ack *extack)
{
	struct nlattr *tb[TCA_FLOWER_KEY_ENC_OPT_GENEVE_MAX + 1];
	struct nlattr *class = NULL, *type = NULL, *data = NULL;
	struct geneve_opt *opt;
	int err, data_len = 0;

	if (option_len > sizeof(struct geneve_opt))
		data_len = option_len - sizeof(struct geneve_opt);

	opt = (struct geneve_opt *)&key->enc_opts.data[key->enc_opts.len];
	memset(opt, 0xff, option_len);
	opt->length = data_len / 4;
	opt->r1 = 0;
	opt->r2 = 0;
	opt->r3 = 0;

	/* If no mask has been prodived we assume an exact match. */
	if (!depth)
		return sizeof(struct geneve_opt) + data_len;

	if (nla_type(nla) != TCA_FLOWER_KEY_ENC_OPTS_GENEVE) {
		NL_SET_ERR_MSG(extack, "Non-geneve option type for mask");
		return -EINVAL;
	}

	err = nla_parse_nested(tb, TCA_FLOWER_KEY_ENC_OPT_GENEVE_MAX,
			       nla, geneve_opt_policy, extack);
	if (err < 0)
		return err;

	/* We are not allowed to omit any of CLASS, TYPE or DATA
	 * fields from the key.
	 */
	if (!option_len &&
	    (!tb[TCA_FLOWER_KEY_ENC_OPT_GENEVE_CLASS] ||
	     !tb[TCA_FLOWER_KEY_ENC_OPT_GENEVE_TYPE] ||
	     !tb[TCA_FLOWER_KEY_ENC_OPT_GENEVE_DATA])) {
		NL_SET_ERR_MSG(extack, "Missing tunnel key geneve option class, type or data");
		return -EINVAL;
	}

	/* Omitting any of CLASS, TYPE or DATA fields is allowed
	 * for the mask.
	 */
	if (tb[TCA_FLOWER_KEY_ENC_OPT_GENEVE_DATA]) {
		int new_len = key->enc_opts.len;

		data = tb[TCA_FLOWER_KEY_ENC_OPT_GENEVE_DATA];
		data_len = nla_len(data);
		if (data_len < 4) {
			NL_SET_ERR_MSG(extack, "Tunnel key geneve option data is less than 4 bytes long");
			return -ERANGE;
		}
		if (data_len % 4) {
			NL_SET_ERR_MSG(extack, "Tunnel key geneve option data is not a multiple of 4 bytes long");
			return -ERANGE;
		}

		new_len += sizeof(struct geneve_opt) + data_len;
		BUILD_BUG_ON(FLOW_DIS_TUN_OPTS_MAX != IP_TUNNEL_OPTS_MAX);
		if (new_len > FLOW_DIS_TUN_OPTS_MAX) {
			NL_SET_ERR_MSG(extack, "Tunnel options exceeds max size");
			return -ERANGE;
		}
		opt->length = data_len / 4;
		memcpy(opt->opt_data, nla_data(data), data_len);
	}

	if (tb[TCA_FLOWER_KEY_ENC_OPT_GENEVE_CLASS]) {
		class = tb[TCA_FLOWER_KEY_ENC_OPT_GENEVE_CLASS];
		opt->opt_class = nla_get_be16(class);
	}

	if (tb[TCA_FLOWER_KEY_ENC_OPT_GENEVE_TYPE]) {
		type = tb[TCA_FLOWER_KEY_ENC_OPT_GENEVE_TYPE];
		opt->type = nla_get_u8(type);
	}

	return sizeof(struct geneve_opt) + data_len;
}

static int fl_set_enc_opt(struct nlattr **tb, struct fl_flow_key *key,
			  struct fl_flow_key *mask,
			  struct netlink_ext_ack *extack)
{
	const struct nlattr *nla_enc_key, *nla_opt_key, *nla_opt_msk = NULL;
	int err, option_len, key_depth, msk_depth = 0;

	err = nla_validate_nested(tb[TCA_FLOWER_KEY_ENC_OPTS],
				  TCA_FLOWER_KEY_ENC_OPTS_MAX,
				  enc_opts_policy, extack);
	if (err)
		return err;

	nla_enc_key = nla_data(tb[TCA_FLOWER_KEY_ENC_OPTS]);

	if (tb[TCA_FLOWER_KEY_ENC_OPTS_MASK]) {
		err = nla_validate_nested(tb[TCA_FLOWER_KEY_ENC_OPTS_MASK],
					  TCA_FLOWER_KEY_ENC_OPTS_MAX,
					  enc_opts_policy, extack);
		if (err)
			return err;

		nla_opt_msk = nla_data(tb[TCA_FLOWER_KEY_ENC_OPTS_MASK]);
		msk_depth = nla_len(tb[TCA_FLOWER_KEY_ENC_OPTS_MASK]);
	}

	nla_for_each_attr(nla_opt_key, nla_enc_key,
			  nla_len(tb[TCA_FLOWER_KEY_ENC_OPTS]), key_depth) {
		switch (nla_type(nla_opt_key)) {
		case TCA_FLOWER_KEY_ENC_OPTS_GENEVE:
			option_len = 0;
			key->enc_opts.dst_opt_type = TUNNEL_GENEVE_OPT;
			option_len = fl_set_geneve_opt(nla_opt_key, key,
						       key_depth, option_len,
						       extack);
			if (option_len < 0)
				return option_len;

			key->enc_opts.len += option_len;
			/* At the same time we need to parse through the mask
			 * in order to verify exact and mask attribute lengths.
			 */
			mask->enc_opts.dst_opt_type = TUNNEL_GENEVE_OPT;
			option_len = fl_set_geneve_opt(nla_opt_msk, mask,
						       msk_depth, option_len,
						       extack);
			if (option_len < 0)
				return option_len;

			mask->enc_opts.len += option_len;
			if (key->enc_opts.len != mask->enc_opts.len) {
				NL_SET_ERR_MSG(extack, "Key and mask miss aligned");
				return -EINVAL;
			}

			if (msk_depth)
				nla_opt_msk = nla_next(nla_opt_msk, &msk_depth);
			break;
		default:
			NL_SET_ERR_MSG(extack, "Unknown tunnel option type");
			return -EINVAL;
		}
	}

	return 0;
}

static int fl_set_key(struct net *net, struct nlattr **tb,
		      struct fl_flow_key *key, struct fl_flow_key *mask,
		      struct netlink_ext_ack *extack)
{
	__be16 ethertype;
	int ret = 0;
#ifdef CONFIG_NET_CLS_IND
	if (tb[TCA_FLOWER_INDEV]) {
		int err = tcf_change_indev(net, tb[TCA_FLOWER_INDEV], extack);
		if (err < 0)
			return err;
		key->indev_ifindex = err;
		mask->indev_ifindex = 0xffffffff;
	}
#endif

	fl_set_key_val(tb, key->eth.dst, TCA_FLOWER_KEY_ETH_DST,
		       mask->eth.dst, TCA_FLOWER_KEY_ETH_DST_MASK,
		       sizeof(key->eth.dst));
	fl_set_key_val(tb, key->eth.src, TCA_FLOWER_KEY_ETH_SRC,
		       mask->eth.src, TCA_FLOWER_KEY_ETH_SRC_MASK,
		       sizeof(key->eth.src));

	if (tb[TCA_FLOWER_KEY_ETH_TYPE]) {
		ethertype = nla_get_be16(tb[TCA_FLOWER_KEY_ETH_TYPE]);

		if (eth_type_vlan(ethertype)) {
			fl_set_key_vlan(tb, ethertype, TCA_FLOWER_KEY_VLAN_ID,
					TCA_FLOWER_KEY_VLAN_PRIO,
					TCA_FLOWER_KEY_VLAN_ETH_TYPE,
					&key->vlan, &mask->vlan);

			if (tb[TCA_FLOWER_KEY_VLAN_ETH_TYPE]) {
				ethertype = nla_get_be16(tb[TCA_FLOWER_KEY_VLAN_ETH_TYPE]);
				if (eth_type_vlan(ethertype)) {
					fl_set_key_vlan(tb, ethertype,
							TCA_FLOWER_KEY_CVLAN_ID,
							TCA_FLOWER_KEY_CVLAN_PRIO,
							TCA_FLOWER_KEY_CVLAN_ETH_TYPE,
							&key->cvlan, &mask->cvlan);
					fl_set_key_val(tb, &key->basic.n_proto,
						       TCA_FLOWER_KEY_CVLAN_ETH_TYPE,
						       &mask->basic.n_proto,
						       TCA_FLOWER_UNSPEC,
						       sizeof(key->basic.n_proto));
				} else {
					key->basic.n_proto = ethertype;
					mask->basic.n_proto = cpu_to_be16(~0);
				}
			}
		} else {
			key->basic.n_proto = ethertype;
			mask->basic.n_proto = cpu_to_be16(~0);
		}
	}

	if (key->basic.n_proto == htons(ETH_P_IP) ||
	    key->basic.n_proto == htons(ETH_P_IPV6)) {
		fl_set_key_val(tb, &key->basic.ip_proto, TCA_FLOWER_KEY_IP_PROTO,
			       &mask->basic.ip_proto, TCA_FLOWER_UNSPEC,
			       sizeof(key->basic.ip_proto));
		fl_set_key_ip(tb, false, &key->ip, &mask->ip);
	}

	if (tb[TCA_FLOWER_KEY_IPV4_SRC] || tb[TCA_FLOWER_KEY_IPV4_DST]) {
		key->control.addr_type = FLOW_DISSECTOR_KEY_IPV4_ADDRS;
		mask->control.addr_type = ~0;
		fl_set_key_val(tb, &key->ipv4.src, TCA_FLOWER_KEY_IPV4_SRC,
			       &mask->ipv4.src, TCA_FLOWER_KEY_IPV4_SRC_MASK,
			       sizeof(key->ipv4.src));
		fl_set_key_val(tb, &key->ipv4.dst, TCA_FLOWER_KEY_IPV4_DST,
			       &mask->ipv4.dst, TCA_FLOWER_KEY_IPV4_DST_MASK,
			       sizeof(key->ipv4.dst));
	} else if (tb[TCA_FLOWER_KEY_IPV6_SRC] || tb[TCA_FLOWER_KEY_IPV6_DST]) {
		key->control.addr_type = FLOW_DISSECTOR_KEY_IPV6_ADDRS;
		mask->control.addr_type = ~0;
		fl_set_key_val(tb, &key->ipv6.src, TCA_FLOWER_KEY_IPV6_SRC,
			       &mask->ipv6.src, TCA_FLOWER_KEY_IPV6_SRC_MASK,
			       sizeof(key->ipv6.src));
		fl_set_key_val(tb, &key->ipv6.dst, TCA_FLOWER_KEY_IPV6_DST,
			       &mask->ipv6.dst, TCA_FLOWER_KEY_IPV6_DST_MASK,
			       sizeof(key->ipv6.dst));
	}

	if (key->basic.ip_proto == IPPROTO_TCP) {
		fl_set_key_val(tb, &key->tp.src, TCA_FLOWER_KEY_TCP_SRC,
			       &mask->tp.src, TCA_FLOWER_KEY_TCP_SRC_MASK,
			       sizeof(key->tp.src));
		fl_set_key_val(tb, &key->tp.dst, TCA_FLOWER_KEY_TCP_DST,
			       &mask->tp.dst, TCA_FLOWER_KEY_TCP_DST_MASK,
			       sizeof(key->tp.dst));
		fl_set_key_val(tb, &key->tcp.flags, TCA_FLOWER_KEY_TCP_FLAGS,
			       &mask->tcp.flags, TCA_FLOWER_KEY_TCP_FLAGS_MASK,
			       sizeof(key->tcp.flags));
	} else if (key->basic.ip_proto == IPPROTO_UDP) {
		fl_set_key_val(tb, &key->tp.src, TCA_FLOWER_KEY_UDP_SRC,
			       &mask->tp.src, TCA_FLOWER_KEY_UDP_SRC_MASK,
			       sizeof(key->tp.src));
		fl_set_key_val(tb, &key->tp.dst, TCA_FLOWER_KEY_UDP_DST,
			       &mask->tp.dst, TCA_FLOWER_KEY_UDP_DST_MASK,
			       sizeof(key->tp.dst));
	} else if (key->basic.ip_proto == IPPROTO_SCTP) {
		fl_set_key_val(tb, &key->tp.src, TCA_FLOWER_KEY_SCTP_SRC,
			       &mask->tp.src, TCA_FLOWER_KEY_SCTP_SRC_MASK,
			       sizeof(key->tp.src));
		fl_set_key_val(tb, &key->tp.dst, TCA_FLOWER_KEY_SCTP_DST,
			       &mask->tp.dst, TCA_FLOWER_KEY_SCTP_DST_MASK,
			       sizeof(key->tp.dst));
	} else if (key->basic.n_proto == htons(ETH_P_IP) &&
		   key->basic.ip_proto == IPPROTO_ICMP) {
		fl_set_key_val(tb, &key->icmp.type, TCA_FLOWER_KEY_ICMPV4_TYPE,
			       &mask->icmp.type,
			       TCA_FLOWER_KEY_ICMPV4_TYPE_MASK,
			       sizeof(key->icmp.type));
		fl_set_key_val(tb, &key->icmp.code, TCA_FLOWER_KEY_ICMPV4_CODE,
			       &mask->icmp.code,
			       TCA_FLOWER_KEY_ICMPV4_CODE_MASK,
			       sizeof(key->icmp.code));
	} else if (key->basic.n_proto == htons(ETH_P_IPV6) &&
		   key->basic.ip_proto == IPPROTO_ICMPV6) {
		fl_set_key_val(tb, &key->icmp.type, TCA_FLOWER_KEY_ICMPV6_TYPE,
			       &mask->icmp.type,
			       TCA_FLOWER_KEY_ICMPV6_TYPE_MASK,
			       sizeof(key->icmp.type));
		fl_set_key_val(tb, &key->icmp.code, TCA_FLOWER_KEY_ICMPV6_CODE,
			       &mask->icmp.code,
			       TCA_FLOWER_KEY_ICMPV6_CODE_MASK,
			       sizeof(key->icmp.code));
	} else if (key->basic.n_proto == htons(ETH_P_MPLS_UC) ||
		   key->basic.n_proto == htons(ETH_P_MPLS_MC)) {
		ret = fl_set_key_mpls(tb, &key->mpls, &mask->mpls);
		if (ret)
			return ret;
	} else if (key->basic.n_proto == htons(ETH_P_ARP) ||
		   key->basic.n_proto == htons(ETH_P_RARP)) {
		fl_set_key_val(tb, &key->arp.sip, TCA_FLOWER_KEY_ARP_SIP,
			       &mask->arp.sip, TCA_FLOWER_KEY_ARP_SIP_MASK,
			       sizeof(key->arp.sip));
		fl_set_key_val(tb, &key->arp.tip, TCA_FLOWER_KEY_ARP_TIP,
			       &mask->arp.tip, TCA_FLOWER_KEY_ARP_TIP_MASK,
			       sizeof(key->arp.tip));
		fl_set_key_val(tb, &key->arp.op, TCA_FLOWER_KEY_ARP_OP,
			       &mask->arp.op, TCA_FLOWER_KEY_ARP_OP_MASK,
			       sizeof(key->arp.op));
		fl_set_key_val(tb, key->arp.sha, TCA_FLOWER_KEY_ARP_SHA,
			       mask->arp.sha, TCA_FLOWER_KEY_ARP_SHA_MASK,
			       sizeof(key->arp.sha));
		fl_set_key_val(tb, key->arp.tha, TCA_FLOWER_KEY_ARP_THA,
			       mask->arp.tha, TCA_FLOWER_KEY_ARP_THA_MASK,
			       sizeof(key->arp.tha));
	}

	if (tb[TCA_FLOWER_KEY_ENC_IPV4_SRC] ||
	    tb[TCA_FLOWER_KEY_ENC_IPV4_DST]) {
		key->enc_control.addr_type = FLOW_DISSECTOR_KEY_IPV4_ADDRS;
		mask->enc_control.addr_type = ~0;
		fl_set_key_val(tb, &key->enc_ipv4.src,
			       TCA_FLOWER_KEY_ENC_IPV4_SRC,
			       &mask->enc_ipv4.src,
			       TCA_FLOWER_KEY_ENC_IPV4_SRC_MASK,
			       sizeof(key->enc_ipv4.src));
		fl_set_key_val(tb, &key->enc_ipv4.dst,
			       TCA_FLOWER_KEY_ENC_IPV4_DST,
			       &mask->enc_ipv4.dst,
			       TCA_FLOWER_KEY_ENC_IPV4_DST_MASK,
			       sizeof(key->enc_ipv4.dst));
	}

	if (tb[TCA_FLOWER_KEY_ENC_IPV6_SRC] ||
	    tb[TCA_FLOWER_KEY_ENC_IPV6_DST]) {
		key->enc_control.addr_type = FLOW_DISSECTOR_KEY_IPV6_ADDRS;
		mask->enc_control.addr_type = ~0;
		fl_set_key_val(tb, &key->enc_ipv6.src,
			       TCA_FLOWER_KEY_ENC_IPV6_SRC,
			       &mask->enc_ipv6.src,
			       TCA_FLOWER_KEY_ENC_IPV6_SRC_MASK,
			       sizeof(key->enc_ipv6.src));
		fl_set_key_val(tb, &key->enc_ipv6.dst,
			       TCA_FLOWER_KEY_ENC_IPV6_DST,
			       &mask->enc_ipv6.dst,
			       TCA_FLOWER_KEY_ENC_IPV6_DST_MASK,
			       sizeof(key->enc_ipv6.dst));
	}

	fl_set_key_val(tb, &key->enc_key_id.keyid, TCA_FLOWER_KEY_ENC_KEY_ID,
		       &mask->enc_key_id.keyid, TCA_FLOWER_UNSPEC,
		       sizeof(key->enc_key_id.keyid));

	fl_set_key_val(tb, &key->enc_tp.src, TCA_FLOWER_KEY_ENC_UDP_SRC_PORT,
		       &mask->enc_tp.src, TCA_FLOWER_KEY_ENC_UDP_SRC_PORT_MASK,
		       sizeof(key->enc_tp.src));

	fl_set_key_val(tb, &key->enc_tp.dst, TCA_FLOWER_KEY_ENC_UDP_DST_PORT,
		       &mask->enc_tp.dst, TCA_FLOWER_KEY_ENC_UDP_DST_PORT_MASK,
		       sizeof(key->enc_tp.dst));

	fl_set_key_ip(tb, true, &key->enc_ip, &mask->enc_ip);

	if (tb[TCA_FLOWER_KEY_ENC_OPTS]) {
		ret = fl_set_enc_opt(tb, key, mask, extack);
		if (ret)
			return ret;
	}

	if (tb[TCA_FLOWER_KEY_FLAGS])
		ret = fl_set_key_flags(tb, &key->control.flags, &mask->control.flags);

	return ret;
}

static void fl_mask_copy(struct fl_flow_mask *dst,
			 struct fl_flow_mask *src)
{
	const void *psrc = fl_key_get_start(&src->key, src);
	void *pdst = fl_key_get_start(&dst->key, src);

	memcpy(pdst, psrc, fl_mask_range(src));
	dst->range = src->range;
}

static const struct rhashtable_params fl_ht_params = {
	.key_offset = offsetof(struct cls_fl_filter, mkey), /* base offset */
	.head_offset = offsetof(struct cls_fl_filter, ht_node),
	.automatic_shrinking = true,
};

static int fl_init_mask_hashtable(struct fl_flow_mask *mask)
{
	mask->filter_ht_params = fl_ht_params;
	mask->filter_ht_params.key_len = fl_mask_range(mask);
	mask->filter_ht_params.key_offset += mask->range.start;

	return rhashtable_init(&mask->ht, &mask->filter_ht_params);
}

#define FL_KEY_MEMBER_OFFSET(member) offsetof(struct fl_flow_key, member)
#define FL_KEY_MEMBER_SIZE(member) (sizeof(((struct fl_flow_key *) 0)->member))

#define FL_KEY_IS_MASKED(mask, member)						\
	memchr_inv(((char *)mask) + FL_KEY_MEMBER_OFFSET(member),		\
		   0, FL_KEY_MEMBER_SIZE(member))				\

#define FL_KEY_SET(keys, cnt, id, member)					\
	do {									\
		keys[cnt].key_id = id;						\
		keys[cnt].offset = FL_KEY_MEMBER_OFFSET(member);		\
		cnt++;								\
	} while(0);

#define FL_KEY_SET_IF_MASKED(mask, keys, cnt, id, member)			\
	do {									\
		if (FL_KEY_IS_MASKED(mask, member))				\
			FL_KEY_SET(keys, cnt, id, member);			\
	} while(0);

static void fl_init_dissector(struct flow_dissector *dissector,
			      struct fl_flow_key *mask)
{
	struct flow_dissector_key keys[FLOW_DISSECTOR_KEY_MAX];
	size_t cnt = 0;

	FL_KEY_SET(keys, cnt, FLOW_DISSECTOR_KEY_CONTROL, control);
	FL_KEY_SET(keys, cnt, FLOW_DISSECTOR_KEY_BASIC, basic);
	FL_KEY_SET_IF_MASKED(mask, keys, cnt,
			     FLOW_DISSECTOR_KEY_ETH_ADDRS, eth);
	FL_KEY_SET_IF_MASKED(mask, keys, cnt,
			     FLOW_DISSECTOR_KEY_IPV4_ADDRS, ipv4);
	FL_KEY_SET_IF_MASKED(mask, keys, cnt,
			     FLOW_DISSECTOR_KEY_IPV6_ADDRS, ipv6);
	FL_KEY_SET_IF_MASKED(mask, keys, cnt,
			     FLOW_DISSECTOR_KEY_PORTS, tp);
	FL_KEY_SET_IF_MASKED(mask, keys, cnt,
			     FLOW_DISSECTOR_KEY_IP, ip);
	FL_KEY_SET_IF_MASKED(mask, keys, cnt,
			     FLOW_DISSECTOR_KEY_TCP, tcp);
	FL_KEY_SET_IF_MASKED(mask, keys, cnt,
			     FLOW_DISSECTOR_KEY_ICMP, icmp);
	FL_KEY_SET_IF_MASKED(mask, keys, cnt,
			     FLOW_DISSECTOR_KEY_ARP, arp);
	FL_KEY_SET_IF_MASKED(mask, keys, cnt,
			     FLOW_DISSECTOR_KEY_MPLS, mpls);
	FL_KEY_SET_IF_MASKED(mask, keys, cnt,
			     FLOW_DISSECTOR_KEY_VLAN, vlan);
	FL_KEY_SET_IF_MASKED(mask, keys, cnt,
			     FLOW_DISSECTOR_KEY_CVLAN, cvlan);
	FL_KEY_SET_IF_MASKED(mask, keys, cnt,
			     FLOW_DISSECTOR_KEY_ENC_KEYID, enc_key_id);
	FL_KEY_SET_IF_MASKED(mask, keys, cnt,
			     FLOW_DISSECTOR_KEY_ENC_IPV4_ADDRS, enc_ipv4);
	FL_KEY_SET_IF_MASKED(mask, keys, cnt,
			     FLOW_DISSECTOR_KEY_ENC_IPV6_ADDRS, enc_ipv6);
	if (FL_KEY_IS_MASKED(mask, enc_ipv4) ||
	    FL_KEY_IS_MASKED(mask, enc_ipv6))
		FL_KEY_SET(keys, cnt, FLOW_DISSECTOR_KEY_ENC_CONTROL,
			   enc_control);
	FL_KEY_SET_IF_MASKED(mask, keys, cnt,
			     FLOW_DISSECTOR_KEY_ENC_PORTS, enc_tp);
	FL_KEY_SET_IF_MASKED(mask, keys, cnt,
			     FLOW_DISSECTOR_KEY_ENC_IP, enc_ip);
	FL_KEY_SET_IF_MASKED(mask, keys, cnt,
			     FLOW_DISSECTOR_KEY_ENC_OPTS, enc_opts);

	skb_flow_dissector_init(dissector, keys, cnt);
}

static struct fl_flow_mask *fl_create_new_mask(struct cls_fl_head *head,
					       struct fl_flow_mask *mask)
{
	struct fl_flow_mask *newmask;
	int err;

	newmask = kzalloc(sizeof(*newmask), GFP_KERNEL);
	if (!newmask)
		return ERR_PTR(-ENOMEM);

	fl_mask_copy(newmask, mask);

	err = fl_init_mask_hashtable(newmask);
	if (err)
		goto errout_free;

	fl_init_dissector(&newmask->dissector, &newmask->key);

	INIT_LIST_HEAD_RCU(&newmask->filters);

	err = rhashtable_insert_fast(&head->ht, &newmask->ht_node,
				     mask_ht_params);
	if (err)
		goto errout_destroy;

	list_add_tail_rcu(&newmask->list, &head->masks);

	return newmask;

errout_destroy:
	rhashtable_destroy(&newmask->ht);
errout_free:
	kfree(newmask);

	return ERR_PTR(err);
}

static int fl_check_assign_mask(struct cls_fl_head *head,
				struct cls_fl_filter *fnew,
				struct cls_fl_filter *fold,
				struct fl_flow_mask *mask)
{
	struct fl_flow_mask *newmask;

	fnew->mask = rhashtable_lookup_fast(&head->ht, mask, mask_ht_params);
	if (!fnew->mask) {
		if (fold)
			return -EINVAL;

		newmask = fl_create_new_mask(head, mask);
		if (IS_ERR(newmask))
			return PTR_ERR(newmask);

		fnew->mask = newmask;
	} else if (fold && fold->mask != fnew->mask) {
		return -EINVAL;
	}

	return 0;
}

static int fl_set_parms(struct net *net, struct tcf_proto *tp,
			struct cls_fl_filter *f, struct fl_flow_mask *mask,
			unsigned long base, struct nlattr **tb,
			struct nlattr *est, bool ovr,
			struct fl_flow_tmplt *tmplt,
			struct netlink_ext_ack *extack)
{
	int err;

	err = tcf_exts_validate(net, tp, tb, est, &f->exts, ovr, extack);
	if (err < 0)
		return err;

	if (tb[TCA_FLOWER_CLASSID]) {
		f->res.classid = nla_get_u32(tb[TCA_FLOWER_CLASSID]);
		tcf_bind_filter(tp, &f->res, base);
	}

	err = fl_set_key(net, tb, &f->key, &mask->key, extack);
	if (err)
		return err;

	fl_mask_update_range(mask);
	fl_set_masked_key(&f->mkey, &f->key, mask);

	if (!fl_mask_fits_tmplt(tmplt, mask)) {
		NL_SET_ERR_MSG_MOD(extack, "Mask does not fit the template");
		return -EINVAL;
	}

	return 0;
}

static int fl_change(struct net *net, struct sk_buff *in_skb,
		     struct tcf_proto *tp, unsigned long base,
		     u32 handle, struct nlattr **tca,
		     void **arg, bool ovr, struct netlink_ext_ack *extack)
{
	struct cls_fl_head *head = rtnl_dereference(tp->root);
	struct cls_fl_filter *fold = *arg;
	struct cls_fl_filter *fnew;
	struct fl_flow_mask *mask;
	struct nlattr **tb;
	int err;

	if (!tca[TCA_OPTIONS])
		return -EINVAL;

	mask = kzalloc(sizeof(struct fl_flow_mask), GFP_KERNEL);
	if (!mask)
		return -ENOBUFS;

	tb = kcalloc(TCA_FLOWER_MAX + 1, sizeof(struct nlattr *), GFP_KERNEL);
	if (!tb) {
		err = -ENOBUFS;
		goto errout_mask_alloc;
	}

	err = nla_parse_nested(tb, TCA_FLOWER_MAX, tca[TCA_OPTIONS],
			       fl_policy, NULL);
	if (err < 0)
		goto errout_tb;

	if (fold && handle && fold->handle != handle) {
		err = -EINVAL;
		goto errout_tb;
	}

	fnew = kzalloc(sizeof(*fnew), GFP_KERNEL);
	if (!fnew) {
		err = -ENOBUFS;
		goto errout_tb;
	}

	err = tcf_exts_init(&fnew->exts, TCA_FLOWER_ACT, 0);
	if (err < 0)
		goto errout;

	if (tb[TCA_FLOWER_FLAGS]) {
		fnew->flags = nla_get_u32(tb[TCA_FLOWER_FLAGS]);

		if (!tc_flags_valid(fnew->flags)) {
			err = -EINVAL;
			goto errout;
		}
	}

	err = fl_set_parms(net, tp, fnew, mask, base, tb, tca[TCA_RATE], ovr,
			   tp->chain->tmplt_priv, extack);
	if (err)
		goto errout;

	err = fl_check_assign_mask(head, fnew, fold, mask);
	if (err)
		goto errout;

	if (!handle) {
		handle = 1;
		err = idr_alloc_u32(&head->handle_idr, fnew, &handle,
				    INT_MAX, GFP_KERNEL);
	} else if (!fold) {
		/* user specifies a handle and it doesn't exist */
		err = idr_alloc_u32(&head->handle_idr, fnew, &handle,
				    handle, GFP_KERNEL);
	}
	if (err)
		goto errout_mask;
	fnew->handle = handle;

	if (!tc_skip_sw(fnew->flags)) {
		if (!fold && fl_lookup(fnew->mask, &fnew->mkey)) {
			err = -EEXIST;
			goto errout_idr;
		}

		err = rhashtable_insert_fast(&fnew->mask->ht, &fnew->ht_node,
					     fnew->mask->filter_ht_params);
		if (err)
			goto errout_idr;
	}

	if (!tc_skip_hw(fnew->flags)) {
		err = fl_hw_replace_filter(tp, fnew, extack);
		if (err)
			goto errout_mask;
	}

	if (!tc_in_hw(fnew->flags))
		fnew->flags |= TCA_CLS_FLAGS_NOT_IN_HW;

	if (fold) {
		if (!tc_skip_sw(fold->flags))
			rhashtable_remove_fast(&fold->mask->ht,
					       &fold->ht_node,
					       fold->mask->filter_ht_params);
		if (!tc_skip_hw(fold->flags))
			fl_hw_destroy_filter(tp, fold, NULL);
	}

	*arg = fnew;

	if (fold) {
		idr_replace(&head->handle_idr, fnew, fnew->handle);
		list_replace_rcu(&fold->list, &fnew->list);
		tcf_unbind_filter(tp, &fold->res);
		tcf_exts_get_net(&fold->exts);
		tcf_queue_work(&fold->rwork, fl_destroy_filter_work);
	} else {
		list_add_tail_rcu(&fnew->list, &fnew->mask->filters);
	}

	kfree(tb);
	kfree(mask);
	return 0;

errout_idr:
	if (!fold)
		idr_remove(&head->handle_idr, fnew->handle);

errout_mask:
	fl_mask_put(head, fnew->mask, false);

errout:
	tcf_exts_destroy(&fnew->exts);
	kfree(fnew);
errout_tb:
	kfree(tb);
errout_mask_alloc:
	kfree(mask);
	return err;
}

static int fl_delete(struct tcf_proto *tp, void *arg, bool *last,
		     struct netlink_ext_ack *extack)
{
	struct cls_fl_head *head = rtnl_dereference(tp->root);
	struct cls_fl_filter *f = arg;

	if (!tc_skip_sw(f->flags))
		rhashtable_remove_fast(&f->mask->ht, &f->ht_node,
				       f->mask->filter_ht_params);
	__fl_delete(tp, f, extack);
	*last = list_empty(&head->masks);
	return 0;
}

static void fl_walk(struct tcf_proto *tp, struct tcf_walker *arg)
{
	struct cls_fl_head *head = rtnl_dereference(tp->root);
	struct cls_fl_filter *f;

	arg->count = arg->skip;

	while ((f = idr_get_next_ul(&head->handle_idr,
				    &arg->cookie)) != NULL) {
		if (arg->fn(tp, f, arg) < 0) {
			arg->stop = 1;
			break;
		}
		arg->cookie = f->handle + 1;
		arg->count++;
	}
}

static int fl_reoffload(struct tcf_proto *tp, bool add, tc_setup_cb_t *cb,
			void *cb_priv, struct netlink_ext_ack *extack)
{
	struct cls_fl_head *head = rtnl_dereference(tp->root);
	struct tc_cls_flower_offload cls_flower = {};
	struct tcf_block *block = tp->chain->block;
	struct fl_flow_mask *mask;
	struct cls_fl_filter *f;
	int err;

	list_for_each_entry(mask, &head->masks, list) {
		list_for_each_entry(f, &mask->filters, list) {
			if (tc_skip_hw(f->flags))
				continue;

			tc_cls_common_offload_init(&cls_flower.common, tp,
						   f->flags, extack);
			cls_flower.command = add ?
				TC_CLSFLOWER_REPLACE : TC_CLSFLOWER_DESTROY;
			cls_flower.cookie = (unsigned long)f;
			cls_flower.dissector = &mask->dissector;
			cls_flower.mask = &mask->key;
			cls_flower.key = &f->mkey;
			cls_flower.exts = &f->exts;
			cls_flower.classid = f->res.classid;

			err = cb(TC_SETUP_CLSFLOWER, &cls_flower, cb_priv);
			if (err) {
				if (add && tc_skip_sw(f->flags))
					return err;
				continue;
			}

			tc_cls_offload_cnt_update(block, &f->in_hw_count,
						  &f->flags, add);
		}
	}

	return 0;
}

static void fl_hw_create_tmplt(struct tcf_chain *chain,
			       struct fl_flow_tmplt *tmplt)
{
	struct tc_cls_flower_offload cls_flower = {};
	struct tcf_block *block = chain->block;
	struct tcf_exts dummy_exts = { 0, };

	cls_flower.common.chain_index = chain->index;
	cls_flower.command = TC_CLSFLOWER_TMPLT_CREATE;
	cls_flower.cookie = (unsigned long) tmplt;
	cls_flower.dissector = &tmplt->dissector;
	cls_flower.mask = &tmplt->mask;
	cls_flower.key = &tmplt->dummy_key;
	cls_flower.exts = &dummy_exts;

	/* We don't care if driver (any of them) fails to handle this
	 * call. It serves just as a hint for it.
	 */
	tc_setup_cb_call(block, NULL, TC_SETUP_CLSFLOWER,
			 &cls_flower, false);
}

static void fl_hw_destroy_tmplt(struct tcf_chain *chain,
				struct fl_flow_tmplt *tmplt)
{
	struct tc_cls_flower_offload cls_flower = {};
	struct tcf_block *block = chain->block;

	cls_flower.common.chain_index = chain->index;
	cls_flower.command = TC_CLSFLOWER_TMPLT_DESTROY;
	cls_flower.cookie = (unsigned long) tmplt;

	tc_setup_cb_call(block, NULL, TC_SETUP_CLSFLOWER,
			 &cls_flower, false);
}

static void *fl_tmplt_create(struct net *net, struct tcf_chain *chain,
			     struct nlattr **tca,
			     struct netlink_ext_ack *extack)
{
	struct fl_flow_tmplt *tmplt;
	struct nlattr **tb;
	int err;

	if (!tca[TCA_OPTIONS])
		return ERR_PTR(-EINVAL);

	tb = kcalloc(TCA_FLOWER_MAX + 1, sizeof(struct nlattr *), GFP_KERNEL);
	if (!tb)
		return ERR_PTR(-ENOBUFS);
	err = nla_parse_nested(tb, TCA_FLOWER_MAX, tca[TCA_OPTIONS],
			       fl_policy, NULL);
	if (err)
		goto errout_tb;

	tmplt = kzalloc(sizeof(*tmplt), GFP_KERNEL);
	if (!tmplt) {
		err = -ENOMEM;
		goto errout_tb;
	}
	tmplt->chain = chain;
	err = fl_set_key(net, tb, &tmplt->dummy_key, &tmplt->mask, extack);
	if (err)
		goto errout_tmplt;
	kfree(tb);

	fl_init_dissector(&tmplt->dissector, &tmplt->mask);

	fl_hw_create_tmplt(chain, tmplt);

	return tmplt;

errout_tmplt:
	kfree(tmplt);
errout_tb:
	kfree(tb);
	return ERR_PTR(err);
}

static void fl_tmplt_destroy(void *tmplt_priv)
{
	struct fl_flow_tmplt *tmplt = tmplt_priv;

	fl_hw_destroy_tmplt(tmplt->chain, tmplt);
	kfree(tmplt);
}

static int fl_dump_key_val(struct sk_buff *skb,
			   void *val, int val_type,
			   void *mask, int mask_type, int len)
{
	int err;

	if (!memchr_inv(mask, 0, len))
		return 0;
	err = nla_put(skb, val_type, len, val);
	if (err)
		return err;
	if (mask_type != TCA_FLOWER_UNSPEC) {
		err = nla_put(skb, mask_type, len, mask);
		if (err)
			return err;
	}
	return 0;
}

static int fl_dump_key_mpls(struct sk_buff *skb,
			    struct flow_dissector_key_mpls *mpls_key,
			    struct flow_dissector_key_mpls *mpls_mask)
{
	int err;

	if (!memchr_inv(mpls_mask, 0, sizeof(*mpls_mask)))
		return 0;
	if (mpls_mask->mpls_ttl) {
		err = nla_put_u8(skb, TCA_FLOWER_KEY_MPLS_TTL,
				 mpls_key->mpls_ttl);
		if (err)
			return err;
	}
	if (mpls_mask->mpls_tc) {
		err = nla_put_u8(skb, TCA_FLOWER_KEY_MPLS_TC,
				 mpls_key->mpls_tc);
		if (err)
			return err;
	}
	if (mpls_mask->mpls_label) {
		err = nla_put_u32(skb, TCA_FLOWER_KEY_MPLS_LABEL,
				  mpls_key->mpls_label);
		if (err)
			return err;
	}
	if (mpls_mask->mpls_bos) {
		err = nla_put_u8(skb, TCA_FLOWER_KEY_MPLS_BOS,
				 mpls_key->mpls_bos);
		if (err)
			return err;
	}
	return 0;
}

static int fl_dump_key_ip(struct sk_buff *skb, bool encap,
			  struct flow_dissector_key_ip *key,
			  struct flow_dissector_key_ip *mask)
{
	int tos_key = encap ? TCA_FLOWER_KEY_ENC_IP_TOS : TCA_FLOWER_KEY_IP_TOS;
	int ttl_key = encap ? TCA_FLOWER_KEY_ENC_IP_TTL : TCA_FLOWER_KEY_IP_TTL;
	int tos_mask = encap ? TCA_FLOWER_KEY_ENC_IP_TOS_MASK : TCA_FLOWER_KEY_IP_TOS_MASK;
	int ttl_mask = encap ? TCA_FLOWER_KEY_ENC_IP_TTL_MASK : TCA_FLOWER_KEY_IP_TTL_MASK;

	if (fl_dump_key_val(skb, &key->tos, tos_key, &mask->tos, tos_mask, sizeof(key->tos)) ||
	    fl_dump_key_val(skb, &key->ttl, ttl_key, &mask->ttl, ttl_mask, sizeof(key->ttl)))
		return -1;

	return 0;
}

static int fl_dump_key_vlan(struct sk_buff *skb,
			    int vlan_id_key, int vlan_prio_key,
			    struct flow_dissector_key_vlan *vlan_key,
			    struct flow_dissector_key_vlan *vlan_mask)
{
	int err;

	if (!memchr_inv(vlan_mask, 0, sizeof(*vlan_mask)))
		return 0;
	if (vlan_mask->vlan_id) {
		err = nla_put_u16(skb, vlan_id_key,
				  vlan_key->vlan_id);
		if (err)
			return err;
	}
	if (vlan_mask->vlan_priority) {
		err = nla_put_u8(skb, vlan_prio_key,
				 vlan_key->vlan_priority);
		if (err)
			return err;
	}
	return 0;
}

static void fl_get_key_flag(u32 dissector_key, u32 dissector_mask,
			    u32 *flower_key, u32 *flower_mask,
			    u32 flower_flag_bit, u32 dissector_flag_bit)
{
	if (dissector_mask & dissector_flag_bit) {
		*flower_mask |= flower_flag_bit;
		if (dissector_key & dissector_flag_bit)
			*flower_key |= flower_flag_bit;
	}
}

static int fl_dump_key_flags(struct sk_buff *skb, u32 flags_key, u32 flags_mask)
{
	u32 key, mask;
	__be32 _key, _mask;
	int err;

	if (!memchr_inv(&flags_mask, 0, sizeof(flags_mask)))
		return 0;

	key = 0;
	mask = 0;

	fl_get_key_flag(flags_key, flags_mask, &key, &mask,
			TCA_FLOWER_KEY_FLAGS_IS_FRAGMENT, FLOW_DIS_IS_FRAGMENT);
	fl_get_key_flag(flags_key, flags_mask, &key, &mask,
			TCA_FLOWER_KEY_FLAGS_FRAG_IS_FIRST,
			FLOW_DIS_FIRST_FRAG);

	_key = cpu_to_be32(key);
	_mask = cpu_to_be32(mask);

	err = nla_put(skb, TCA_FLOWER_KEY_FLAGS, 4, &_key);
	if (err)
		return err;

	return nla_put(skb, TCA_FLOWER_KEY_FLAGS_MASK, 4, &_mask);
}

static int fl_dump_key_geneve_opt(struct sk_buff *skb,
				  struct flow_dissector_key_enc_opts *enc_opts)
{
	struct geneve_opt *opt;
	struct nlattr *nest;
	int opt_off = 0;

	nest = nla_nest_start(skb, TCA_FLOWER_KEY_ENC_OPTS_GENEVE);
	if (!nest)
		goto nla_put_failure;

	while (enc_opts->len > opt_off) {
		opt = (struct geneve_opt *)&enc_opts->data[opt_off];

		if (nla_put_be16(skb, TCA_FLOWER_KEY_ENC_OPT_GENEVE_CLASS,
				 opt->opt_class))
			goto nla_put_failure;
		if (nla_put_u8(skb, TCA_FLOWER_KEY_ENC_OPT_GENEVE_TYPE,
			       opt->type))
			goto nla_put_failure;
		if (nla_put(skb, TCA_FLOWER_KEY_ENC_OPT_GENEVE_DATA,
			    opt->length * 4, opt->opt_data))
			goto nla_put_failure;

		opt_off += sizeof(struct geneve_opt) + opt->length * 4;
	}
	nla_nest_end(skb, nest);
	return 0;

nla_put_failure:
	nla_nest_cancel(skb, nest);
	return -EMSGSIZE;
}

static int fl_dump_key_options(struct sk_buff *skb, int enc_opt_type,
			       struct flow_dissector_key_enc_opts *enc_opts)
{
	struct nlattr *nest;
	int err;

	if (!enc_opts->len)
		return 0;

	nest = nla_nest_start(skb, enc_opt_type);
	if (!nest)
		goto nla_put_failure;

	switch (enc_opts->dst_opt_type) {
	case TUNNEL_GENEVE_OPT:
		err = fl_dump_key_geneve_opt(skb, enc_opts);
		if (err)
			goto nla_put_failure;
		break;
	default:
		goto nla_put_failure;
	}
	nla_nest_end(skb, nest);
	return 0;

nla_put_failure:
	nla_nest_cancel(skb, nest);
	return -EMSGSIZE;
}

static int fl_dump_key_enc_opt(struct sk_buff *skb,
			       struct flow_dissector_key_enc_opts *key_opts,
			       struct flow_dissector_key_enc_opts *msk_opts)
{
	int err;

	err = fl_dump_key_options(skb, TCA_FLOWER_KEY_ENC_OPTS, key_opts);
	if (err)
		return err;

	return fl_dump_key_options(skb, TCA_FLOWER_KEY_ENC_OPTS_MASK, msk_opts);
}

static int fl_dump_key(struct sk_buff *skb, struct net *net,
		       struct fl_flow_key *key, struct fl_flow_key *mask)
{
	if (mask->indev_ifindex) {
		struct net_device *dev;

		dev = __dev_get_by_index(net, key->indev_ifindex);
		if (dev && nla_put_string(skb, TCA_FLOWER_INDEV, dev->name))
			goto nla_put_failure;
	}

	if (fl_dump_key_val(skb, key->eth.dst, TCA_FLOWER_KEY_ETH_DST,
			    mask->eth.dst, TCA_FLOWER_KEY_ETH_DST_MASK,
			    sizeof(key->eth.dst)) ||
	    fl_dump_key_val(skb, key->eth.src, TCA_FLOWER_KEY_ETH_SRC,
			    mask->eth.src, TCA_FLOWER_KEY_ETH_SRC_MASK,
			    sizeof(key->eth.src)) ||
	    fl_dump_key_val(skb, &key->basic.n_proto, TCA_FLOWER_KEY_ETH_TYPE,
			    &mask->basic.n_proto, TCA_FLOWER_UNSPEC,
			    sizeof(key->basic.n_proto)))
		goto nla_put_failure;

	if (fl_dump_key_mpls(skb, &key->mpls, &mask->mpls))
		goto nla_put_failure;

	if (fl_dump_key_vlan(skb, TCA_FLOWER_KEY_VLAN_ID,
			     TCA_FLOWER_KEY_VLAN_PRIO, &key->vlan, &mask->vlan))
		goto nla_put_failure;

	if (fl_dump_key_vlan(skb, TCA_FLOWER_KEY_CVLAN_ID,
			     TCA_FLOWER_KEY_CVLAN_PRIO,
			     &key->cvlan, &mask->cvlan) ||
	    (mask->cvlan.vlan_tpid &&
	     nla_put_be16(skb, TCA_FLOWER_KEY_VLAN_ETH_TYPE,
			  key->cvlan.vlan_tpid)))
		goto nla_put_failure;

	if (mask->basic.n_proto) {
		if (mask->cvlan.vlan_eth_type) {
			if (nla_put_be16(skb, TCA_FLOWER_KEY_CVLAN_ETH_TYPE,
					 key->basic.n_proto))
				goto nla_put_failure;
		} else if (mask->vlan.vlan_eth_type) {
			if (nla_put_be16(skb, TCA_FLOWER_KEY_VLAN_ETH_TYPE,
					 key->vlan.vlan_eth_type))
				goto nla_put_failure;
		}
	}

	if ((key->basic.n_proto == htons(ETH_P_IP) ||
	     key->basic.n_proto == htons(ETH_P_IPV6)) &&
	    (fl_dump_key_val(skb, &key->basic.ip_proto, TCA_FLOWER_KEY_IP_PROTO,
			    &mask->basic.ip_proto, TCA_FLOWER_UNSPEC,
			    sizeof(key->basic.ip_proto)) ||
	    fl_dump_key_ip(skb, false, &key->ip, &mask->ip)))
		goto nla_put_failure;

	if (key->control.addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS &&
	    (fl_dump_key_val(skb, &key->ipv4.src, TCA_FLOWER_KEY_IPV4_SRC,
			     &mask->ipv4.src, TCA_FLOWER_KEY_IPV4_SRC_MASK,
			     sizeof(key->ipv4.src)) ||
	     fl_dump_key_val(skb, &key->ipv4.dst, TCA_FLOWER_KEY_IPV4_DST,
			     &mask->ipv4.dst, TCA_FLOWER_KEY_IPV4_DST_MASK,
			     sizeof(key->ipv4.dst))))
		goto nla_put_failure;
	else if (key->control.addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS &&
		 (fl_dump_key_val(skb, &key->ipv6.src, TCA_FLOWER_KEY_IPV6_SRC,
				  &mask->ipv6.src, TCA_FLOWER_KEY_IPV6_SRC_MASK,
				  sizeof(key->ipv6.src)) ||
		  fl_dump_key_val(skb, &key->ipv6.dst, TCA_FLOWER_KEY_IPV6_DST,
				  &mask->ipv6.dst, TCA_FLOWER_KEY_IPV6_DST_MASK,
				  sizeof(key->ipv6.dst))))
		goto nla_put_failure;

	if (key->basic.ip_proto == IPPROTO_TCP &&
	    (fl_dump_key_val(skb, &key->tp.src, TCA_FLOWER_KEY_TCP_SRC,
			     &mask->tp.src, TCA_FLOWER_KEY_TCP_SRC_MASK,
			     sizeof(key->tp.src)) ||
	     fl_dump_key_val(skb, &key->tp.dst, TCA_FLOWER_KEY_TCP_DST,
			     &mask->tp.dst, TCA_FLOWER_KEY_TCP_DST_MASK,
			     sizeof(key->tp.dst)) ||
	     fl_dump_key_val(skb, &key->tcp.flags, TCA_FLOWER_KEY_TCP_FLAGS,
			     &mask->tcp.flags, TCA_FLOWER_KEY_TCP_FLAGS_MASK,
			     sizeof(key->tcp.flags))))
		goto nla_put_failure;
	else if (key->basic.ip_proto == IPPROTO_UDP &&
		 (fl_dump_key_val(skb, &key->tp.src, TCA_FLOWER_KEY_UDP_SRC,
				  &mask->tp.src, TCA_FLOWER_KEY_UDP_SRC_MASK,
				  sizeof(key->tp.src)) ||
		  fl_dump_key_val(skb, &key->tp.dst, TCA_FLOWER_KEY_UDP_DST,
				  &mask->tp.dst, TCA_FLOWER_KEY_UDP_DST_MASK,
				  sizeof(key->tp.dst))))
		goto nla_put_failure;
	else if (key->basic.ip_proto == IPPROTO_SCTP &&
		 (fl_dump_key_val(skb, &key->tp.src, TCA_FLOWER_KEY_SCTP_SRC,
				  &mask->tp.src, TCA_FLOWER_KEY_SCTP_SRC_MASK,
				  sizeof(key->tp.src)) ||
		  fl_dump_key_val(skb, &key->tp.dst, TCA_FLOWER_KEY_SCTP_DST,
				  &mask->tp.dst, TCA_FLOWER_KEY_SCTP_DST_MASK,
				  sizeof(key->tp.dst))))
		goto nla_put_failure;
	else if (key->basic.n_proto == htons(ETH_P_IP) &&
		 key->basic.ip_proto == IPPROTO_ICMP &&
		 (fl_dump_key_val(skb, &key->icmp.type,
				  TCA_FLOWER_KEY_ICMPV4_TYPE, &mask->icmp.type,
				  TCA_FLOWER_KEY_ICMPV4_TYPE_MASK,
				  sizeof(key->icmp.type)) ||
		  fl_dump_key_val(skb, &key->icmp.code,
				  TCA_FLOWER_KEY_ICMPV4_CODE, &mask->icmp.code,
				  TCA_FLOWER_KEY_ICMPV4_CODE_MASK,
				  sizeof(key->icmp.code))))
		goto nla_put_failure;
	else if (key->basic.n_proto == htons(ETH_P_IPV6) &&
		 key->basic.ip_proto == IPPROTO_ICMPV6 &&
		 (fl_dump_key_val(skb, &key->icmp.type,
				  TCA_FLOWER_KEY_ICMPV6_TYPE, &mask->icmp.type,
				  TCA_FLOWER_KEY_ICMPV6_TYPE_MASK,
				  sizeof(key->icmp.type)) ||
		  fl_dump_key_val(skb, &key->icmp.code,
				  TCA_FLOWER_KEY_ICMPV6_CODE, &mask->icmp.code,
				  TCA_FLOWER_KEY_ICMPV6_CODE_MASK,
				  sizeof(key->icmp.code))))
		goto nla_put_failure;
	else if ((key->basic.n_proto == htons(ETH_P_ARP) ||
		  key->basic.n_proto == htons(ETH_P_RARP)) &&
		 (fl_dump_key_val(skb, &key->arp.sip,
				  TCA_FLOWER_KEY_ARP_SIP, &mask->arp.sip,
				  TCA_FLOWER_KEY_ARP_SIP_MASK,
				  sizeof(key->arp.sip)) ||
		  fl_dump_key_val(skb, &key->arp.tip,
				  TCA_FLOWER_KEY_ARP_TIP, &mask->arp.tip,
				  TCA_FLOWER_KEY_ARP_TIP_MASK,
				  sizeof(key->arp.tip)) ||
		  fl_dump_key_val(skb, &key->arp.op,
				  TCA_FLOWER_KEY_ARP_OP, &mask->arp.op,
				  TCA_FLOWER_KEY_ARP_OP_MASK,
				  sizeof(key->arp.op)) ||
		  fl_dump_key_val(skb, key->arp.sha, TCA_FLOWER_KEY_ARP_SHA,
				  mask->arp.sha, TCA_FLOWER_KEY_ARP_SHA_MASK,
				  sizeof(key->arp.sha)) ||
		  fl_dump_key_val(skb, key->arp.tha, TCA_FLOWER_KEY_ARP_THA,
				  mask->arp.tha, TCA_FLOWER_KEY_ARP_THA_MASK,
				  sizeof(key->arp.tha))))
		goto nla_put_failure;

	if (key->enc_control.addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS &&
	    (fl_dump_key_val(skb, &key->enc_ipv4.src,
			    TCA_FLOWER_KEY_ENC_IPV4_SRC, &mask->enc_ipv4.src,
			    TCA_FLOWER_KEY_ENC_IPV4_SRC_MASK,
			    sizeof(key->enc_ipv4.src)) ||
	     fl_dump_key_val(skb, &key->enc_ipv4.dst,
			     TCA_FLOWER_KEY_ENC_IPV4_DST, &mask->enc_ipv4.dst,
			     TCA_FLOWER_KEY_ENC_IPV4_DST_MASK,
			     sizeof(key->enc_ipv4.dst))))
		goto nla_put_failure;
	else if (key->enc_control.addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS &&
		 (fl_dump_key_val(skb, &key->enc_ipv6.src,
			    TCA_FLOWER_KEY_ENC_IPV6_SRC, &mask->enc_ipv6.src,
			    TCA_FLOWER_KEY_ENC_IPV6_SRC_MASK,
			    sizeof(key->enc_ipv6.src)) ||
		 fl_dump_key_val(skb, &key->enc_ipv6.dst,
				 TCA_FLOWER_KEY_ENC_IPV6_DST,
				 &mask->enc_ipv6.dst,
				 TCA_FLOWER_KEY_ENC_IPV6_DST_MASK,
			    sizeof(key->enc_ipv6.dst))))
		goto nla_put_failure;

	if (fl_dump_key_val(skb, &key->enc_key_id, TCA_FLOWER_KEY_ENC_KEY_ID,
			    &mask->enc_key_id, TCA_FLOWER_UNSPEC,
			    sizeof(key->enc_key_id)) ||
	    fl_dump_key_val(skb, &key->enc_tp.src,
			    TCA_FLOWER_KEY_ENC_UDP_SRC_PORT,
			    &mask->enc_tp.src,
			    TCA_FLOWER_KEY_ENC_UDP_SRC_PORT_MASK,
			    sizeof(key->enc_tp.src)) ||
	    fl_dump_key_val(skb, &key->enc_tp.dst,
			    TCA_FLOWER_KEY_ENC_UDP_DST_PORT,
			    &mask->enc_tp.dst,
			    TCA_FLOWER_KEY_ENC_UDP_DST_PORT_MASK,
			    sizeof(key->enc_tp.dst)) ||
	    fl_dump_key_ip(skb, true, &key->enc_ip, &mask->enc_ip) ||
	    fl_dump_key_enc_opt(skb, &key->enc_opts, &mask->enc_opts))
		goto nla_put_failure;

	if (fl_dump_key_flags(skb, key->control.flags, mask->control.flags))
		goto nla_put_failure;

	return 0;

nla_put_failure:
	return -EMSGSIZE;
}

static int fl_dump(struct net *net, struct tcf_proto *tp, void *fh,
		   struct sk_buff *skb, struct tcmsg *t)
{
	struct cls_fl_filter *f = fh;
	struct nlattr *nest;
	struct fl_flow_key *key, *mask;

	if (!f)
		return skb->len;

	t->tcm_handle = f->handle;

	nest = nla_nest_start(skb, TCA_OPTIONS);
	if (!nest)
		goto nla_put_failure;

	if (f->res.classid &&
	    nla_put_u32(skb, TCA_FLOWER_CLASSID, f->res.classid))
		goto nla_put_failure;

	key = &f->key;
	mask = &f->mask->key;

	if (fl_dump_key(skb, net, key, mask))
		goto nla_put_failure;

	if (!tc_skip_hw(f->flags))
		fl_hw_update_stats(tp, f);

	if (f->flags && nla_put_u32(skb, TCA_FLOWER_FLAGS, f->flags))
		goto nla_put_failure;

	if (tcf_exts_dump(skb, &f->exts))
		goto nla_put_failure;

	nla_nest_end(skb, nest);

	if (tcf_exts_dump_stats(skb, &f->exts) < 0)
		goto nla_put_failure;

	return skb->len;

nla_put_failure:
	nla_nest_cancel(skb, nest);
	return -1;
}

static int fl_tmplt_dump(struct sk_buff *skb, struct net *net, void *tmplt_priv)
{
	struct fl_flow_tmplt *tmplt = tmplt_priv;
	struct fl_flow_key *key, *mask;
	struct nlattr *nest;

	nest = nla_nest_start(skb, TCA_OPTIONS);
	if (!nest)
		goto nla_put_failure;

	key = &tmplt->dummy_key;
	mask = &tmplt->mask;

	if (fl_dump_key(skb, net, key, mask))
		goto nla_put_failure;

	nla_nest_end(skb, nest);

	return skb->len;

nla_put_failure:
	nla_nest_cancel(skb, nest);
	return -EMSGSIZE;
}

static void fl_bind_class(void *fh, u32 classid, unsigned long cl, void *q,
			  unsigned long base)
{
	struct cls_fl_filter *f = fh;

	if (f && f->res.classid == classid) {
		if (cl)
			__tcf_bind_filter(q, &f->res, base);
		else
			__tcf_unbind_filter(q, &f->res);
	}
}

static struct tcf_proto_ops cls_fl_ops __read_mostly = {
	.kind		= "flower",
	.classify	= fl_classify,
	.init		= fl_init,
	.destroy	= fl_destroy,
	.get		= fl_get,
	.change		= fl_change,
	.delete		= fl_delete,
	.walk		= fl_walk,
	.reoffload	= fl_reoffload,
	.dump		= fl_dump,
	.bind_class	= fl_bind_class,
	.tmplt_create	= fl_tmplt_create,
	.tmplt_destroy	= fl_tmplt_destroy,
	.tmplt_dump	= fl_tmplt_dump,
	.owner		= THIS_MODULE,
};

static int __init cls_fl_init(void)
{
	return register_tcf_proto_ops(&cls_fl_ops);
}

static void __exit cls_fl_exit(void)
{
	unregister_tcf_proto_ops(&cls_fl_ops);
}

module_init(cls_fl_init);
module_exit(cls_fl_exit);

MODULE_AUTHOR("Jiri Pirko <jiri@resnulli.us>");
MODULE_DESCRIPTION("Flower classifier");
MODULE_LICENSE("GPL v2");
