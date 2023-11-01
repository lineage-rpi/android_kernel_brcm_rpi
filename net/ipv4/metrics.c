#include <linux/netlink.h>
#include <linux/nospec.h>
#include <linux/rtnetlink.h>
#include <linux/types.h>
#include <net/ip.h>
#include <net/net_namespace.h>
#include <net/tcp.h>

int ip_metrics_convert(struct net *net, struct nlattr *fc_mx, int fc_mx_len,
		       u32 *metrics)
{
	bool ecn_ca = false;
	struct nlattr *nla;
	int remaining;

	if (!fc_mx)
		return 0;

	nla_for_each_attr(nla, fc_mx, fc_mx_len, remaining) {
		int type = nla_type(nla);
		u32 val;

		if (!type)
			continue;
		if (type > RTAX_MAX)
			return -EINVAL;

		type = array_index_nospec(type, RTAX_MAX + 1);
		if (type == RTAX_CC_ALGO) {
			char tmp[TCP_CA_NAME_MAX];

			nla_strlcpy(tmp, nla, sizeof(tmp));
			val = tcp_ca_get_key_by_name(net, tmp, &ecn_ca);
			if (val == TCP_CA_UNSPEC)
				return -EINVAL;
		} else {
			if (nla_len(nla) != sizeof(u32))
				return -EINVAL;
			val = nla_get_u32(nla);
		}
		if (type == RTAX_ADVMSS && val > 65535 - 40)
			val = 65535 - 40;
		if (type == RTAX_MTU && val > 65535 - 15)
			val = 65535 - 15;
		if (type == RTAX_HOPLIMIT && val > 255)
			val = 255;
		if (type == RTAX_FEATURES && (val & ~RTAX_FEATURE_MASK))
			return -EINVAL;
		metrics[type - 1] = val;
	}

	if (ecn_ca)
		metrics[RTAX_FEATURES - 1] |= DST_FEATURE_ECN_CA;

	return 0;
}
EXPORT_SYMBOL_GPL(ip_metrics_convert);
