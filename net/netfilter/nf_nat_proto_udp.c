/* (C) 1999-2001 Paul `Rusty' Russell
 * (C) 2002-2006 Netfilter Core Team <coreteam@netfilter.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/types.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/udp.h>

#include <linux/netfilter.h>
#include <net/netfilter/nf_nat.h>
#include <net/netfilter/nf_nat_core.h>
#include <net/netfilter/nf_nat_l3proto.h>
#include <net/netfilter/nf_nat_l4proto.h>

static void
udp_unique_tuple(const struct nf_nat_l3proto *l3proto,
		 struct nf_conntrack_tuple *tuple,
		 const struct nf_nat_range2 *range,
		 enum nf_nat_manip_type maniptype,
		 const struct nf_conn *ct)
{
	nf_nat_l4proto_unique_tuple(l3proto, tuple, range, maniptype, ct);
}

static void
__udp_manip_pkt(struct sk_buff *skb,
	        const struct nf_nat_l3proto *l3proto,
	        unsigned int iphdroff, struct udphdr *hdr,
	        const struct nf_conntrack_tuple *tuple,
	        enum nf_nat_manip_type maniptype, bool do_csum)
{
	__be16 *portptr, newport;

	if (maniptype == NF_NAT_MANIP_SRC) {
		/* Get rid of src port */
		newport = tuple->src.u.udp.port;
		portptr = &hdr->source;
	} else {
		/* Get rid of dst port */
		newport = tuple->dst.u.udp.port;
		portptr = &hdr->dest;
	}
	if (do_csum) {
		l3proto->csum_update(skb, iphdroff, &hdr->check,
				     tuple, maniptype);
		inet_proto_csum_replace2(&hdr->check, skb, *portptr, newport,
					 false);
		if (!hdr->check)
			hdr->check = CSUM_MANGLED_0;
	}
	*portptr = newport;
}

static bool udp_manip_pkt(struct sk_buff *skb,
			  const struct nf_nat_l3proto *l3proto,
			  unsigned int iphdroff, unsigned int hdroff,
			  const struct nf_conntrack_tuple *tuple,
			  enum nf_nat_manip_type maniptype)
{
	struct udphdr *hdr;

	if (!skb_make_writable(skb, hdroff + sizeof(*hdr)))
		return false;

	hdr = (struct udphdr *)(skb->data + hdroff);
	__udp_manip_pkt(skb, l3proto, iphdroff, hdr, tuple, maniptype,
			!!hdr->check);

	return true;
}

#ifdef CONFIG_NF_NAT_PROTO_UDPLITE
static bool udplite_manip_pkt(struct sk_buff *skb,
			      const struct nf_nat_l3proto *l3proto,
			      unsigned int iphdroff, unsigned int hdroff,
			      const struct nf_conntrack_tuple *tuple,
			      enum nf_nat_manip_type maniptype)
{
	struct udphdr *hdr;

	if (!skb_make_writable(skb, hdroff + sizeof(*hdr)))
		return false;

	hdr = (struct udphdr *)(skb->data + hdroff);
	__udp_manip_pkt(skb, l3proto, iphdroff, hdr, tuple, maniptype, true);
	return true;
}

static void
udplite_unique_tuple(const struct nf_nat_l3proto *l3proto,
		     struct nf_conntrack_tuple *tuple,
		     const struct nf_nat_range2 *range,
		     enum nf_nat_manip_type maniptype,
		     const struct nf_conn *ct)
{
	nf_nat_l4proto_unique_tuple(l3proto, tuple, range, maniptype, ct);
}

const struct nf_nat_l4proto nf_nat_l4proto_udplite = {
	.l4proto		= IPPROTO_UDPLITE,
	.manip_pkt		= udplite_manip_pkt,
	.in_range		= nf_nat_l4proto_in_range,
	.unique_tuple		= udplite_unique_tuple,
#if IS_ENABLED(CONFIG_NF_CT_NETLINK)
	.nlattr_to_range	= nf_nat_l4proto_nlattr_to_range,
#endif
};
#endif /* CONFIG_NF_NAT_PROTO_UDPLITE */

const struct nf_nat_l4proto nf_nat_l4proto_udp = {
	.l4proto		= IPPROTO_UDP,
	.manip_pkt		= udp_manip_pkt,
	.in_range		= nf_nat_l4proto_in_range,
	.unique_tuple		= udp_unique_tuple,
#if IS_ENABLED(CONFIG_NF_CT_NETLINK)
	.nlattr_to_range	= nf_nat_l4proto_nlattr_to_range,
#endif
};
