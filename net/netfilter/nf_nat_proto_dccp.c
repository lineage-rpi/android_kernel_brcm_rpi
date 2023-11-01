/*
 * DCCP NAT protocol helper
 *
 * Copyright (c) 2005, 2006, 2008 Patrick McHardy <kaber@trash.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/dccp.h>

#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_nat.h>
#include <net/netfilter/nf_nat_l3proto.h>
#include <net/netfilter/nf_nat_l4proto.h>

static void
dccp_unique_tuple(const struct nf_nat_l3proto *l3proto,
		  struct nf_conntrack_tuple *tuple,
		  const struct nf_nat_range2 *range,
		  enum nf_nat_manip_type maniptype,
		  const struct nf_conn *ct)
{
	nf_nat_l4proto_unique_tuple(l3proto, tuple, range, maniptype, ct);
}

static bool
dccp_manip_pkt(struct sk_buff *skb,
	       const struct nf_nat_l3proto *l3proto,
	       unsigned int iphdroff, unsigned int hdroff,
	       const struct nf_conntrack_tuple *tuple,
	       enum nf_nat_manip_type maniptype)
{
	struct dccp_hdr *hdr;
	__be16 *portptr, oldport, newport;
	int hdrsize = 8; /* DCCP connection tracking guarantees this much */

	if (skb->len >= hdroff + sizeof(struct dccp_hdr))
		hdrsize = sizeof(struct dccp_hdr);

	if (!skb_make_writable(skb, hdroff + hdrsize))
		return false;

	hdr = (struct dccp_hdr *)(skb->data + hdroff);

	if (maniptype == NF_NAT_MANIP_SRC) {
		newport = tuple->src.u.dccp.port;
		portptr = &hdr->dccph_sport;
	} else {
		newport = tuple->dst.u.dccp.port;
		portptr = &hdr->dccph_dport;
	}

	oldport = *portptr;
	*portptr = newport;

	if (hdrsize < sizeof(*hdr))
		return true;

	l3proto->csum_update(skb, iphdroff, &hdr->dccph_checksum,
			     tuple, maniptype);
	inet_proto_csum_replace2(&hdr->dccph_checksum, skb, oldport, newport,
				 false);
	return true;
}

const struct nf_nat_l4proto nf_nat_l4proto_dccp = {
	.l4proto		= IPPROTO_DCCP,
	.manip_pkt		= dccp_manip_pkt,
	.in_range		= nf_nat_l4proto_in_range,
	.unique_tuple		= dccp_unique_tuple,
#if IS_ENABLED(CONFIG_NF_CT_NETLINK)
	.nlattr_to_range	= nf_nat_l4proto_nlattr_to_range,
#endif
};
