/*
 * Copyright (c) 2001-2003, Raphael Manfredi
 * Copyright (c) 2005, Christian Biere
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @ingroup lib
 * @file
 *
 * Host address functions.
 *
 * @author Christian Biere
 * @date 2005
 */

#ifndef _host_addr_h_
#define _host_addr_h_

#include "common.h"

#include "endian.h"
#include "glib-missing.h"		/* For g_carp() */
#include "misc.h"

/**
 * @note AF_UNIX/AF_LOCAL (unix domain) sockets are not fully supported. These
 * are only used for "anonymous" incoming connections, not for outgoing
 * connections and a potential pathname is ignored.
 */

enum net_type {
	NET_TYPE_NONE	= 0,
	NET_TYPE_LOCAL	= 1,
	NET_TYPE_IPV4	= 4,
	NET_TYPE_IPV6	= 6
};

static inline int
net_type_to_pf(enum net_type net)
{
	switch (net) {
	case NET_TYPE_NONE:  return PF_UNSPEC;
	case NET_TYPE_LOCAL: return PF_LOCAL;
	case NET_TYPE_IPV4:  return PF_INET;
	case NET_TYPE_IPV6:
#ifdef HAS_IPV6
		return PF_INET6;
#else
		return PF_UNSPEC;
#endif /* HAS_IPV6 */
	}
	g_assert_not_reached();
	return PF_UNSPEC;
}

static inline int
net_type_to_af(enum net_type net)
{
	switch (net) {
	case NET_TYPE_NONE:  return AF_UNSPEC;
	case NET_TYPE_LOCAL: return AF_LOCAL;
	case NET_TYPE_IPV4:  return AF_INET;
	case NET_TYPE_IPV6:
#ifdef HAS_IPV6
		return AF_INET6;
#else
		return AF_UNSPEC;
#endif /* HAS_IPV6 */
	}
	g_assert_not_reached();
	return AF_UNSPEC;
}

/**
 * Structure used to hold any IPv4 or IPv6 address whilst keeping track
 * of which type it is.
 */
typedef struct host_addr {
	guint32 net;	/**< The address network type */
	union {
		guint8	ipv6[16];	/**< This is valid if "net == NET_TYPE_IPV6" */
		guint32 ipv4;		/**< @attention: Always in host byte order! */
	} addr;
} host_addr_t;

/**
 * Serialized host address.
 */
struct packed_host_addr {
	guchar net;
	guchar addr[sizeof ((host_addr_t *) 0)->addr];
};

/**
 * Serialized host (IP:port).
 */
struct packed_host {
	guchar port[sizeof (guint16)];
	struct packed_host_addr ha;
};

typedef union socket_addr {
	/*
	 * Both structures in the union start with an sa_family_t field
	 * (either sin_family or sin6_family) so we can discriminate the
	 * union correctly by probing inet4.sin_family and inet6.sin6_family.
	 */
	struct sockaddr_in inet4;
#ifdef HAS_IPV6
	struct sockaddr_in6 inet6;
#endif /* HAS_IPV6 */
} socket_addr_t;

static const host_addr_t ipv4_unspecified = {	/* 0.0.0.0/32 */
	NET_TYPE_IPV4,
	{ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
};

static const host_addr_t ipv4_loopback = {	/* 127.0.0.1/32 */
	NET_TYPE_IPV4,
#if G_BYTE_ORDER == G_BIG_ENDIAN
	{ { 0x7f, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
#else
	{ { 0x01, 0x00, 0x00, 0x7f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
#endif
};

static const host_addr_t ipv6_unspecified = {	/* ::/128 */
	NET_TYPE_IPV6,
	{ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
};

static const host_addr_t ipv6_loopback = {	/* ::1/128 */
	NET_TYPE_IPV6,
	{ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } },
};

static const host_addr_t ipv6_ipv4_mapped = {	/* ::ffff:0:0/96 */
	NET_TYPE_IPV6,
	{ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0 } },
};

static const host_addr_t ipv6_multicast = {		/* ff00::/8 */
	NET_TYPE_IPV6,
	{ { 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
};

static const host_addr_t ipv6_link_local = {	/* fe80::/10 */
	NET_TYPE_IPV6,
	{ { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
};

static const host_addr_t ipv6_site_local = {	/* fec0::/10 */
	NET_TYPE_IPV6,
	{ { 0xfe, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
};

static const host_addr_t ipv6_6to4 = {			/* 2002::/16 */
	NET_TYPE_IPV6,
	{ { 0x20, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
};


static const host_addr_t local_host_addr = {
	NET_TYPE_LOCAL,
	{ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
};

static const host_addr_t zero_host_addr;

static inline size_t
packed_host_length(const struct packed_host *ph)
{
	switch (ph->ha.net) {
	case NET_TYPE_IPV4:
		return ptr_diff(&ph->ha.addr[4], ph);
	case NET_TYPE_IPV6:
	case NET_TYPE_LOCAL:
		return ptr_diff(&ph->ha.addr[16], ph);
	case NET_TYPE_NONE:
		return ptr_diff(&ph->ha.addr[0], ph);
	}
	/*
	 * Because this routine can be used through gnet_host_length() to compute
	 * the length of keys from a DBMW file, we cannot abort the execution when
	 * faced with an invalid structure (coming from the disk).
	 */
	g_carp("corrupted packed host: unknown net address type %d", ph->ha.net);
	return ptr_diff(&ph->ha.addr[0], ph);
}

gboolean host_addr_convert(const host_addr_t from, host_addr_t *to,
	enum net_type to_net);
gboolean host_addr_can_convert(const host_addr_t from, enum net_type to_net);
gboolean host_addr_tunnel_client(const host_addr_t from, host_addr_t *to);


static inline gboolean
host_addr_initialized(const host_addr_t ha)
{
	switch (ha.net) {
	case NET_TYPE_IPV4:
	case NET_TYPE_IPV6:
	case NET_TYPE_LOCAL:
		return TRUE;
	case NET_TYPE_NONE:
		return FALSE;
	}
	g_assert_not_reached();
	return FALSE;
}

static inline const char *
net_type_to_string(enum net_type net)
{
	switch (net) {
	case NET_TYPE_IPV4:  return "IPv4";
	case NET_TYPE_IPV6:  return "IPv6";
	case NET_TYPE_LOCAL: return "<local>";
	case NET_TYPE_NONE:  return "<none>";
	}
	g_assert_not_reached();
	return NULL;
}

static inline G_GNUC_CONST enum net_type 
host_addr_net(const host_addr_t ha)
{
	return ha.net;
}

static inline G_GNUC_CONST gboolean
host_addr_is_ipv4(const host_addr_t ha)
{
	return NET_TYPE_IPV4 == ha.net;
}

static inline G_GNUC_CONST gboolean
host_addr_is_ipv6(const host_addr_t ha)
{
	return NET_TYPE_IPV6 == ha.net;
}

static inline G_GNUC_CONST guint32
host_addr_ipv4(const host_addr_t ha)
{
	return NET_TYPE_IPV4 == ha.net ? ha.addr.ipv4 : 0;
}

static inline const guint8 *
host_addr_ipv6(const host_addr_t *ha)
{
	return NET_TYPE_IPV6 == ha->net ? ha->addr.ipv6 : NULL;
}

static inline G_GNUC_CONST host_addr_t
host_addr_get_ipv4(guint32 ip)
{
	host_addr_t ha;

	ha.net = NET_TYPE_IPV4;
	ha.addr.ipv4 = ip;
	return ha;
}

static inline G_GNUC_PURE host_addr_t
host_addr_peek_ipv4(const void *ipv4)
{
	return host_addr_get_ipv4(peek_be32(ipv4));
}

static inline G_GNUC_PURE host_addr_t
host_addr_peek_ipv6(const void *ipv6)
{
	host_addr_t ha;
	
	ha.net = NET_TYPE_IPV6;
	memcpy(ha.addr.ipv6, ipv6, 16);
	return ha;
}

static inline gboolean
host_addr_equal(const host_addr_t a, const host_addr_t b)
{
	if (a.net == b.net) {
		switch (a.net) {
		case NET_TYPE_IPV4:
			return host_addr_ipv4(a) == host_addr_ipv4(b);
		case NET_TYPE_IPV6:
			if (0 != memcmp(a.addr.ipv6, b.addr.ipv6, sizeof a.addr.ipv6)) {
				host_addr_t a_ipv4, b_ipv4;

				return host_addr_convert(a, &a_ipv4, NET_TYPE_IPV4) &&
					host_addr_convert(b, &b_ipv4, NET_TYPE_IPV4) &&
					host_addr_ipv4(a_ipv4) == host_addr_ipv4(b_ipv4);
			}
			return TRUE;

		case NET_TYPE_LOCAL:
		case NET_TYPE_NONE:
			return TRUE;
		}
		g_assert_not_reached();
	} else {
		host_addr_t to;

		return host_addr_convert(a, &to, b.net) && host_addr_equal(to, b);
	}
	return FALSE;
}

static inline int
host_addr_cmp(host_addr_t a, host_addr_t b)
{
	int r;

	r = CMP(a.net, b.net);
	if (0 != r) {
		host_addr_t to;

		if (!host_addr_convert(b, &to, a.net))
			return r;
		b = to;
	}

	switch (a.net) {
	case NET_TYPE_IPV4:
		return CMP(host_addr_ipv4(a), host_addr_ipv4(b));
	case NET_TYPE_IPV6:
		{
			guint i;

			for (i = 0; i < G_N_ELEMENTS(a.addr.ipv6); i++) {
				r = CMP(a.addr.ipv6[i], b.addr.ipv6[i]);
				if (0 != r)
					break;
			}
		}
		return r;
	case NET_TYPE_LOCAL:
	case NET_TYPE_NONE:
		return 0;
	}
	g_assert_not_reached();
	return 0;
}

static inline gboolean
host_addr_matches(const host_addr_t a, const host_addr_t b, guint8 bits)
{
	host_addr_t to;
	guint8 shift;

	if (!host_addr_convert(b, &to, a.net))
		return FALSE;

	switch (a.net) {
	case NET_TYPE_IPV4:
		shift = bits < 32 ? 32 - bits : 0;
		return host_addr_ipv4(a) >> shift == host_addr_ipv4(to) >> shift;

	case NET_TYPE_IPV6:
		{
			int i;

			bits = MIN(128, bits);
			for (i = 0; bits >= 8; i++, bits -= 8) {
				if (a.addr.ipv6[i] != to.addr.ipv6[i])
					return FALSE;
			}

			if (bits > 0) {
				shift = 8 - bits;
				return (a.addr.ipv6[i] >> shift) == (to.addr.ipv6[i] >> shift);
			}

		}
		return TRUE;

	case NET_TYPE_LOCAL:
	case NET_TYPE_NONE:
		return TRUE;
	}

	g_assert_not_reached();
	return FALSE;
}


static inline G_GNUC_PURE gboolean
is_host_addr(const host_addr_t ha)
{
	switch (host_addr_net(ha)) {
	case NET_TYPE_IPV4:
		return 0 != host_addr_ipv4(ha);
	case NET_TYPE_IPV6:
		return 0 != memcmp(ha.addr.ipv6, zero_host_addr.addr.ipv6,
						sizeof ha.addr.ipv6);
	case NET_TYPE_LOCAL:
	case NET_TYPE_NONE:
		return FALSE;
	}
	g_assert_not_reached();
	return FALSE;
}

static inline guint32
host_addr_hash(host_addr_t ha)
{
	switch (ha.net) {
	case NET_TYPE_IPV6:
		{
			host_addr_t ha_ipv4;

			if (!host_addr_convert(ha, &ha_ipv4, NET_TYPE_IPV4)) {
				guint32 h = ha.net ^ ha.addr.ipv6[15];
				guint i;

				for (i = 0; i < sizeof ha.addr.ipv6; i++)
					h ^= (guint32) ha.addr.ipv6[i] << (i * 2);

				return h;
			}
			ha = ha_ipv4;
		}
		/* FALL THROUGH */
	case NET_TYPE_IPV4:
		return ha.net ^ host_addr_ipv4(ha);
	case NET_TYPE_LOCAL:
	case NET_TYPE_NONE:
		return ha.net;
	}
	g_assert_not_reached();
	return (guint32) -1;
}

/**
 * Retrieves the address from a socket_addr_t.
 *
 * @param addr a pointer to an initialized socket_addr_t
 * @return the address.
 */
static inline host_addr_t
socket_addr_get_addr(const socket_addr_t *addr)
{
	host_addr_t ha;

	g_assert(addr);

	if (AF_INET == addr->inet4.sin_family) {
		ha = host_addr_peek_ipv4(&addr->inet4.sin_addr.s_addr);
#if defined(HAS_IPV6)
	} else if (AF_INET6 == addr->inet6.sin6_family) {
		ha = host_addr_peek_ipv6(addr->inet6.sin6_addr.s6_addr);
#endif /* HAS_IPV6 */
	} else {
		ha = zero_host_addr;
	}

	return ha;
}

/**
 * Retrieves the port number from a socket_addr_t.
 *
 * @param addr a pointer to an initialized socket_addr_t
 * @return the port number in host byte order
 */
static inline guint16
socket_addr_get_port(const socket_addr_t *addr)
{
	g_assert(addr != NULL);

	if (AF_INET == addr->inet4.sin_family) {
		return ntohs(addr->inet4.sin_port);
#if defined(HAS_IPV6)
	} else if (AF_INET6 == addr->inet6.sin6_family) {
		return ntohs(addr->inet6.sin6_port);
#endif /* HAS_IPV6 */
	}

	return 0;
}

static inline socklen_t
socket_addr_get_len(const socket_addr_t *addr)
{
	g_assert(addr != NULL);

	if (AF_INET == addr->inet4.sin_family) {
		return sizeof addr->inet4;
#if defined(HAS_IPV6)
	} else if (AF_INET6 == addr->inet6.sin6_family) {
		return sizeof addr->inet6;
#endif /* HAS_IPV6 */
	}

	return 0;
}

static inline const struct sockaddr *
socket_addr_get_const_sockaddr(const socket_addr_t *addr)
{
	g_assert(addr != NULL);

	if (AF_INET == addr->inet4.sin_family) {
		return cast_to_gconstpointer(&addr->inet4);
#if defined(HAS_IPV6)
	} else if (AF_INET6 == addr->inet6.sin6_family) {
		return cast_to_gconstpointer(&addr->inet6);
#endif /* HAS_IPV6 */
	}

	return NULL;
}

static inline struct sockaddr *
socket_addr_get_sockaddr(socket_addr_t *addr)
{
	return (struct sockaddr *) socket_addr_get_const_sockaddr(addr);
}

static inline int
socket_addr_get_family(const socket_addr_t *addr)
{
	g_assert(addr != NULL);

	if (AF_INET == addr->inet4.sin_family) {
		return AF_INET;
#if defined(HAS_IPV6)
	} else if (AF_INET6 == addr->inet6.sin6_family) {
		return AF_INET6;
#endif /* HAS_IPV6 */
	}

	return 0;
}

socklen_t socket_addr_set(socket_addr_t *sa_ptr,
			const host_addr_t addr, guint16 port);
socklen_t socket_addr_init(socket_addr_t *sa_ptr, enum net_type net);
int socket_addr_getpeername(socket_addr_t *p_addr, int fd);
int socket_addr_getsockname(socket_addr_t *p_addr, int fd);

guint host_addr_hash_func(gconstpointer key);
gboolean host_addr_eq_func(gconstpointer p, gconstpointer q);
void wfree_host_addr1(void *key);
void wfree_host_addr(gpointer key, gpointer unused_data);

int host_addr_family(const host_addr_t ha);
gboolean is_private_addr(const host_addr_t addr);
gboolean host_addr_is_routable(const host_addr_t addr);
gboolean host_addr_is_loopback(const host_addr_t addr);
gboolean host_addr_is_unspecified(const host_addr_t addr);

static inline gboolean
host_addr_is_ipv4_mapped(const host_addr_t addr)
{
	return host_addr_is_ipv6(addr) && 
		host_addr_matches(addr, ipv6_ipv4_mapped, 96);
}

const char *host_addr_to_string(const host_addr_t ha);
const char *host_addr_to_string2(const host_addr_t ha);
size_t host_addr_to_string_buf(const host_addr_t addr, char *, size_t);
gboolean string_to_host_addr(const char *s, const char **endptr,
	host_addr_t *addr_ptr);
const char *host_addr_port_to_string(const host_addr_t addr, guint16 port);
const char *host_addr_port_to_string2(const host_addr_t addr, guint16 port);
size_t host_addr_port_to_string_buf(const host_addr_t addr,
				guint16 port, char *, size_t);
gboolean string_to_host_addr_port(const char *str, const char **endptr,
	host_addr_t *addr_ptr, guint16 *port_ptr);
gboolean
string_to_port_host_addr(const char *str, const char **endptr,
	guint16 *port_ptr, host_addr_t *addr_ptr);
const char *host_port_to_string(const char *hostname,
				host_addr_t addr, guint16 port);
const char *port_host_addr_to_string(guint16 port, const host_addr_t ha);
size_t host_port_addr_to_string_buf(guint16 port, const host_addr_t ha,
	char *dst, size_t size);

GSList *name_to_host_addr(const char *host, enum net_type net);
void host_addr_free_list(GSList **sl_ptr);

host_addr_t name_to_single_host_addr(const char *host, enum net_type net);
#ifdef HAS_GETADDRINFO
struct addrinfo;
host_addr_t addrinfo_to_addr(const struct addrinfo *ai);
#endif

const char *host_addr_to_name(const host_addr_t addr);
gboolean string_to_host_or_addr(const char *s, const char **endptr,
		host_addr_t *ha);

GSList *host_addr_get_interface_addrs(enum net_type net);
void host_addr_free_interface_addrs(GSList **sl_ptr);

guint packed_host_addr_size(const struct packed_host_addr paddr);
struct packed_host_addr host_addr_pack(const host_addr_t addr);
host_addr_t packed_host_addr_unpack(const struct packed_host_addr paddr);

guint packed_host_size(const struct packed_host paddr);
struct packed_host host_pack(const host_addr_t addr, guint16 port);
void packed_host_unpack_addr(const struct packed_host *phost,
	host_addr_t *addr_ptr);
guint packed_host_hash_func(gconstpointer key);
gboolean packed_host_eq_func(gconstpointer p, gconstpointer q);
gpointer walloc_packed_host(const host_addr_t addr, guint16 port);
void wfree_packed_host(gpointer key, gpointer unused_data);

#endif /* _host_addr_h_ */

/* vi: set ts=4 sw=4 cindent: */
