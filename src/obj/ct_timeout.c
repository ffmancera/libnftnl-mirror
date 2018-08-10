/*
 * (C) 2018 by Harsha Sharma <harshasharmaiitr@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>

#include <linux/netfilter/nf_tables.h>

#include "internal.h"
#include <libmnl/libmnl.h>
#include <libnftnl/object.h>
#include <libnftnl/cttimeout.h>

#include "obj.h"

static const char *const tcp_state_to_name[] = {
	[NFTNL_CTTIMEOUT_TCP_SYN_SENT]		= "SYN_SENT",
	[NFTNL_CTTIMEOUT_TCP_SYN_RECV]		= "SYN_RECV",
	[NFTNL_CTTIMEOUT_TCP_ESTABLISHED]	= "ESTABLISHED",
	[NFTNL_CTTIMEOUT_TCP_FIN_WAIT]		= "FIN_WAIT",
	[NFTNL_CTTIMEOUT_TCP_CLOSE_WAIT]	= "CLOSE_WAIT",
	[NFTNL_CTTIMEOUT_TCP_LAST_ACK]		= "LAST_ACK",
	[NFTNL_CTTIMEOUT_TCP_TIME_WAIT]		= "TIME_WAIT",
	[NFTNL_CTTIMEOUT_TCP_CLOSE]		= "CLOSE",
	[NFTNL_CTTIMEOUT_TCP_SYN_SENT2]		= "SYN_SENT2",
	[NFTNL_CTTIMEOUT_TCP_RETRANS]		= "RETRANS",
	[NFTNL_CTTIMEOUT_TCP_UNACK]		= "UNACKNOWLEDGED",
};

static uint32_t tcp_dflt_timeout[] = {
	[NFTNL_CTTIMEOUT_TCP_SYN_SENT]		= 120,
	[NFTNL_CTTIMEOUT_TCP_SYN_RECV]		= 60,
	[NFTNL_CTTIMEOUT_TCP_ESTABLISHED]	= 432000,
	[NFTNL_CTTIMEOUT_TCP_FIN_WAIT]		= 120,
	[NFTNL_CTTIMEOUT_TCP_CLOSE_WAIT]	= 60,
	[NFTNL_CTTIMEOUT_TCP_LAST_ACK]		= 30,
	[NFTNL_CTTIMEOUT_TCP_TIME_WAIT]		= 120,
	[NFTNL_CTTIMEOUT_TCP_CLOSE]		= 10,
	[NFTNL_CTTIMEOUT_TCP_SYN_SENT2] 	= 120,
	[NFTNL_CTTIMEOUT_TCP_RETRANS]		= 300,
	[NFTNL_CTTIMEOUT_TCP_UNACK]		= 300,
};

static const char *const udp_state_to_name[] = {
	[NFTNL_CTTIMEOUT_UDP_UNREPLIED]		= "UNREPLIED",
	[NFTNL_CTTIMEOUT_UDP_REPLIED]		= "REPLIED",
};

static uint32_t udp_dflt_timeout[] = {
	[NFTNL_CTTIMEOUT_UDP_UNREPLIED]		= 30,
	[NFTNL_CTTIMEOUT_UDP_REPLIED]		= 180,
};

static struct {
	uint32_t attr_max;
	const char *const *state_to_name;
	uint32_t *dflt_timeout;
} timeout_protocol[IPPROTO_MAX] = {
	[IPPROTO_TCP]	= {
		.attr_max	= NFTNL_CTTIMEOUT_TCP_MAX,
		.state_to_name	= tcp_state_to_name,
		.dflt_timeout	= tcp_dflt_timeout,
	},
	[IPPROTO_UDP]	= {
		.attr_max	= NFTNL_CTTIMEOUT_UDP_MAX,
		.state_to_name	= udp_state_to_name,
		.dflt_timeout	= udp_dflt_timeout,
	},
};

struct _container_policy_cb {
	unsigned int nlattr_max;
	void *tb;
};

static int
nftnl_timeout_policy_attr_set_u32(struct nftnl_obj *e,
				 uint32_t type, uint32_t data)
{
	struct nftnl_obj_ct_timeout *t = nftnl_obj_data(e);
	size_t timeout_array_size;

	/* Layer 4 protocol needs to be already set. */
	if (!(e->flags & (1 << NFTNL_OBJ_CT_TIMEOUT_L4PROTO)))
		return -1;
	if (t->timeout == NULL) {
		/* if not supported, default to generic protocol tracker. */
		if (timeout_protocol[t->l4proto].attr_max != 0) {
			timeout_array_size = sizeof(uint32_t) *
					timeout_protocol[t->l4proto].attr_max;
		} else {
			timeout_array_size = sizeof(uint32_t) *
					timeout_protocol[IPPROTO_RAW].attr_max;
		}
		t->timeout = calloc(1, timeout_array_size);
		if (t->timeout == NULL)
			return -1;
	}

	/* this state does not exists in this protocol tracker.*/
	if (type > timeout_protocol[t->l4proto].attr_max)
		return -1;

	t->timeout[type] = data;

	if (!(e->flags & (1 << NFTNL_OBJ_CT_TIMEOUT_ARRAY)))
		e->flags |= (1 << NFTNL_OBJ_CT_TIMEOUT_ARRAY);

	return 0;
}

static int
parse_timeout_attr_policy_cb(const struct nlattr *attr, void *data)
{
	struct _container_policy_cb *data_cb = data;
	const struct nlattr **tb = data_cb->tb;
	uint16_t type = mnl_attr_get_type(attr);

	if (mnl_attr_type_valid(attr, data_cb->nlattr_max) < 0)
		return MNL_CB_OK;

	if (type <= data_cb->nlattr_max) {
		if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
			abi_breakage();
		tb[type] = attr;
	}
	return MNL_CB_OK;
}

static void
timeout_parse_attr_data(struct nftnl_obj *e,
			const struct nlattr *nest)
{
	struct nftnl_obj_ct_timeout *t = nftnl_obj_data(e);
	unsigned int attr_max = timeout_protocol[t->l4proto].attr_max;
	struct nlattr *tb[attr_max];
	struct _container_policy_cb cnt = {
		.nlattr_max = attr_max,
		.tb = tb,
	};
	unsigned int i;

	memset(tb, 0, sizeof(struct nlattr *) * attr_max);

	mnl_attr_parse_nested(nest, parse_timeout_attr_policy_cb, &cnt);

	for (i = 1; i <= attr_max; i++) {
		if (tb[i]) {
			nftnl_timeout_policy_attr_set_u32(e, i-1,
				ntohl(mnl_attr_get_u32(tb[i])));
		}
	}
}

static int nftnl_obj_ct_timeout_set(struct nftnl_obj *e, uint16_t type,
				   const void *data, uint32_t data_len)
{
	struct nftnl_obj_ct_timeout *timeout = nftnl_obj_data(e);

	switch (type) {
	case NFTNL_OBJ_CT_TIMEOUT_L3PROTO:
		timeout->l3proto = *((uint16_t *)data);
		break;
	case NFTNL_OBJ_CT_TIMEOUT_L4PROTO:
		timeout->l4proto = *((uint8_t *)data);
		break;
	case NFTNL_OBJ_CT_TIMEOUT_ARRAY:
		timeout->timeout = ((uint32_t *)data);
		break;
	default:
		return -1;
		}
	return 0;
}

static const void *nftnl_obj_ct_timeout_get(const struct nftnl_obj *e,
					   uint16_t type, uint32_t *data_len)
{
	struct nftnl_obj_ct_timeout *timeout = nftnl_obj_data(e);

	switch (type) {
	case NFTNL_OBJ_CT_TIMEOUT_L3PROTO:
		*data_len = sizeof(timeout->l3proto);
		return &timeout->l3proto;
	case NFTNL_OBJ_CT_TIMEOUT_L4PROTO:
		*data_len = sizeof(timeout->l4proto);
		return &timeout->l4proto;
	case NFTNL_OBJ_CT_TIMEOUT_ARRAY:
		*data_len = sizeof(timeout->timeout);
		return timeout->timeout;
	}
	return NULL;
}

static int nftnl_obj_ct_timeout_cb(const struct nlattr *attr, void *data)
{
	int type = mnl_attr_get_type(attr);
	const struct nlattr **tb = data;

	if (mnl_attr_type_valid(attr, NFTA_CT_TIMEOUT_MAX) < 0)
		return MNL_CB_OK;

	switch (type) {
	case NFTA_CT_TIMEOUT_L3PROTO:
		if (mnl_attr_validate(attr, MNL_TYPE_U16) < 0)
			abi_breakage();
		break;
	case NFTA_CT_TIMEOUT_L4PROTO:
		if (mnl_attr_validate(attr, MNL_TYPE_U8) < 0)
			abi_breakage();
		break;
	case NFTA_CT_TIMEOUT_DATA:
		if (mnl_attr_validate(attr, MNL_TYPE_NESTED) < 0)
			abi_breakage();
		break;
	}

	tb[type] = attr;
	return MNL_CB_OK;
}

static void
nftnl_obj_ct_timeout_build(struct nlmsghdr *nlh, const struct nftnl_obj *e)
{
	struct nftnl_obj_ct_timeout *timeout = nftnl_obj_data(e);
	struct nlattr *nest;

	if (e->flags & (1 << NFTNL_OBJ_CT_TIMEOUT_L3PROTO))
		mnl_attr_put_u16(nlh, NFTA_CT_TIMEOUT_L3PROTO, htons(timeout->l3proto));
	if (e->flags & (1 << NFTNL_OBJ_CT_TIMEOUT_L4PROTO))
		mnl_attr_put_u8(nlh, NFTA_CT_TIMEOUT_L4PROTO, timeout->l4proto);
	if (e->flags & (1 << NFTNL_OBJ_CT_TIMEOUT_ARRAY)) {
		nest = mnl_attr_nest_start(nlh, NFTA_CT_TIMEOUT_DATA);
		for (int i = 0; i < timeout_protocol[timeout->l4proto].attr_max; i++) {
			if (timeout->timeout[i])
				mnl_attr_put_u32(nlh, i+1, htonl(timeout->timeout[i]));
		}
		mnl_attr_nest_end(nlh, nest);
	}
}

static int
nftnl_obj_ct_timeout_parse(struct nftnl_obj *e, struct nlattr *attr)
{
	struct nftnl_obj_ct_timeout *timeout = nftnl_obj_data(e);
	struct nlattr *tb[NFTA_CT_TIMEOUT_MAX + 1] = {};

	if (mnl_attr_parse_nested(attr, nftnl_obj_ct_timeout_cb, tb) < 0)
		return -1;

	if (tb[NFTA_CT_TIMEOUT_L3PROTO]) {
		timeout->l3proto = ntohs(mnl_attr_get_u16(tb[NFTA_CT_TIMEOUT_L3PROTO]));
		e->flags |= (1 << NFTNL_OBJ_CT_TIMEOUT_L3PROTO);
	}
	if (tb[NFTA_CT_TIMEOUT_L4PROTO]) {
		timeout->l4proto = mnl_attr_get_u8(tb[NFTA_CT_TIMEOUT_L4PROTO]);
		e->flags |= (1 << NFTNL_OBJ_CT_TIMEOUT_L4PROTO);
	}
	if (tb[NFTA_CT_TIMEOUT_DATA]) {
		timeout_parse_attr_data(e, tb[NFTA_CT_TIMEOUT_DATA]);
		e->flags |= (1 << NFTNL_OBJ_CT_TIMEOUT_ARRAY);
	}
	return 0;
}

static int nftnl_obj_ct_timeout_export(char *buf, size_t size,
				   const struct nftnl_obj *e, int type)
{
	struct nftnl_obj_ct_timeout *timeout = nftnl_obj_data(e);

	NFTNL_BUF_INIT(b, buf, size);

	if (e->flags & (1 << NFTNL_OBJ_CT_TIMEOUT_L3PROTO))
		nftnl_buf_u32(&b, type, timeout->l3proto, FAMILY);
	if (e->flags & (1 << NFTNL_OBJ_CT_TIMEOUT_L4PROTO))
		nftnl_buf_u32(&b, type, timeout->l4proto, "service");

	return nftnl_buf_done(&b);
}

static int nftnl_obj_ct_timeout_snprintf_default(char *buf, size_t len,
					       const struct nftnl_obj *e)
{
	int ret = 0;
	int offset = 0, remain = len;

	struct nftnl_obj_ct_timeout *timeout = nftnl_obj_data(e);

	if (e->flags & (1 << NFTNL_OBJ_CT_TIMEOUT_L3PROTO)) {
		ret = snprintf(buf + offset, len, "family %d ",
			       timeout->l3proto);
		SNPRINTF_BUFFER_SIZE(ret, remain, offset);
	}
	if (e->flags & (1 << NFTNL_OBJ_CT_TIMEOUT_L4PROTO)) {
		ret = snprintf(buf + offset, len, "protocol %d ",
				timeout->l4proto);
		SNPRINTF_BUFFER_SIZE(ret, remain, offset);
	}
	if (e->flags & (1 << NFTNL_OBJ_CT_TIMEOUT_ARRAY)) {
		uint8_t l4num = timeout->l4proto;
		int i;

		/* default to generic protocol tracker. */
		if (timeout_protocol[timeout->l4proto].attr_max == 0)
			l4num = IPPROTO_RAW;

		ret = snprintf(buf + offset, len, "policy = {");
		SNPRINTF_BUFFER_SIZE(ret, remain, offset);

		for (i = 0; i < timeout_protocol[l4num].attr_max; i++) {
			const char *state_name =
				timeout_protocol[l4num].state_to_name[i][0] ?
				timeout_protocol[l4num].state_to_name[i] :
				"UNKNOWN";

			if (timeout->timeout[i] != timeout_protocol[l4num].dflt_timeout[i]) {
				ret = snprintf(buf + offset, len,
					"%s = %u,", state_name, timeout->timeout[i]);
				SNPRINTF_BUFFER_SIZE(ret, remain, offset);
			}
		}

		ret = snprintf(buf + offset, len, "}");
		SNPRINTF_BUFFER_SIZE(ret, remain, offset);
	}
	buf[offset] = '\0';

	return offset;
}

static int nftnl_obj_ct_timeout_snprintf(char *buf, size_t len, uint32_t type,
				       uint32_t flags,
				       const struct nftnl_obj *e)
{
	if (len)
		buf[0] = '\0';

	switch (type) {
	case NFTNL_OUTPUT_DEFAULT:
		return nftnl_obj_ct_timeout_snprintf_default(buf, len, e);
	case NFTNL_OUTPUT_JSON:
		return nftnl_obj_ct_timeout_export(buf, len, e, type);
	default:
		break;
	}
	return -1;
}

struct obj_ops obj_ops_ct_timeout = {
	.name		= "ct_timeout",
	.type		= NFT_OBJECT_CT_TIMEOUT,
	.alloc_len	= sizeof(struct nftnl_obj_ct_timeout),
	.max_attr	= NFTA_CT_TIMEOUT_MAX,
	.set		= nftnl_obj_ct_timeout_set,
	.get		= nftnl_obj_ct_timeout_get,
	.parse		= nftnl_obj_ct_timeout_parse,
	.build		= nftnl_obj_ct_timeout_build,
	.snprintf	= nftnl_obj_ct_timeout_snprintf,
};