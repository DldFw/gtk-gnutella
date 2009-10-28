/*
 * $Id$
 *
 * Copyright (c) 2009, Raphael Manfredi
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
 * @ingroup dht
 * @file
 *
 * Stable node recording.
 *
 * The purpose here is to record the lifetime of nodes that we cache as
 * "roots", to be able to compute the probability of a published value to
 * still be present on the DHT one hour later (which is the current hardwired
 * expiration time in the LimeWire servents and chances to have that changed
 * are slim, as of 2009-10-19).
 *
 * The maths:
 *
 * The probability of presence of nodes on the network is modelled through
 * the following probability density function:
 *
 *    f(x) = 1 / (2*x^2)     for x >= 1
 *
 * with "x" being the number of hours a servent is up.  We consider that
 * there is a probability of 0.5 that a servent will not stay up for more
 * than 1 hour.  For those which do, we use f(x), and it is trivial to
 * see that the primitive of f is:
 *
 *    F(x) = -1/2x
 *
 * and that the integral of f(x) between 1 and +oo is F(+oo) - F(1) = 0.5.
 *
 * Therefore, f(x) is a suitable probability density function fox x >= 1,
 * given our hypothesis that P(x >= 1) = 0.5.
 *
 * Given the limit of F(x) is 0 when x |--> +oo, the integral of f(x) between
 * ``a'' and ``+oo'' is -F(a).
 *
 * This is used to easily compute a (theoretical) conditional probability
 * of presence of a node for "h" more hours given that it was already
 * alive for H hours (with H >= 1):
 *
 *    P(x >= h)|x >= H = -F(H + h) / -F(H) = H / (H + h)
 *
 * For the [0, 1] interval, we use the following probability density:
 *
 *    f(x) = 1/2           for x in [0, 1]
 *
 * Its primitive is:
 *
 *    F(x) = x/2
 *
 * and the integral of f(x) over [0, 1] is F(1) - F(0) = 0.5
 *
 * Since f(1) = 0.5 in the two definitions we gave above, the function
 * defines a continuous probability density whose integral over [0, +oo] is 1:
 *
 *     f(x) = 1/2          for x in [0, 1]
 *     f(x) = 1 / (2*x^2)  for x >= 1
 *
 * Applications:
 *
 * - for publishing operations, given the set of nodes to which the value
 *   was stored, and knowing the current stability of these nodes, compute
 *   the probability of having the published value still there before the
 *   next publishing cycle.  If too low, we can then attempt republishing
 *   sooner than the default.
 *
 * - for values we store and for which we have an original, we can determine
 *   the probability that the publisher remains alive for one more publishing
 *   period and, if high enough, increase the expiration date for the published
 *   value, so as to reserve the entry slot in the key (given we have a fixed
 *   limited amount of values per keys) to the most stable publishers.
 *
 * Implementation:
 *
 * We keep a table mapping a node's KUID with the set { first seen, last seen }.
 * Each time we get a STORE reply (not necessarily OK) from the node, we update
 * the "time last seen".  We don't care about IP:port changes because the node
 * remains in the DHT with the same KUID, hence it will be quickly reachable
 * again thanks to the routing table updates.  We don't consider KUID conflicts
 * a problem here given our application.
 *
 * For practical considerations, we make the above table persistent so that
 * immediate restarts can reuse the information collected from a past run.
 *
 * When no updates are seen on a given node for more than 2 * republish period,
 * we consider the node dead and reclaim its entry.
 *
 * @author Raphael Manfredi
 * @date 2009
 */

#include "common.h"

RCSID("$Id$")

#include "stable.h"
#include "storage.h"

#include "if/dht/kuid.h"
#include "if/dht/knode.h"
#include "if/dht/value.h"

#include "core/gnet_stats.h"

#include "if/gnet_property_priv.h"

#include "lib/atoms.h"
#include "lib/cq.h"
#include "lib/dbmw.h"
#include "lib/stringify.h"
#include "lib/tm.h"
#include "lib/override.h"		/* Must be the last header included */

#define	STABLE_DB_CACHE_SIZE	4096	/**< Cached amount of stable nodes */
#define STABLE_MAP_CACHE_SIZE	64		/**< Amount of SDBM pages to cache */

#define STABLE_EXPIRE (2 * DHT_VALUE_REPUBLISH)	/**< 2 republish periods */

#define STABLE_PRUNE_PERIOD	(DHT_VALUE_REPUBLISH * 1000)
#define STABLE_SYNC_PERIOD	(60 * 1000)

/**
 * DBM wrapper to associate a target KUID with the set timestamps.
 */
static dbmw_t *db_lifedata;
static char db_stable_base[] = "dht_stable";
static char db_stable_what[] = "DHT stable nodes";

#define LIFEDATA_STRUCT_VERSION	0

/**
 * Information about a target KUID that is stored to disk.
 * The structure is serialized first, not written as-is.
 */
struct lifedata {
	time_t first_seen;			/**< Time when we first seen the node */
	time_t last_seen;			/**< Last time we saw the node */
	guint8 version;				/**< Structure version information */
};

/**
 * Get lifedata from database, returning NULL if not found.
 */
static struct lifedata *
get_lifedata(const kuid_t *id)
{
	struct lifedata *ld;

	ld = dbmw_read(db_lifedata, id, NULL);

	if (NULL == ld && dbmw_has_ioerr(db_lifedata)) {
		g_warning("DBMW \"%s\" I/O error, bad things could happen...",
			dbmw_name(db_lifedata));
	}

	return ld;
}

/**
 * Given a node who has been alive for t seconds, return the probability
 * that it will be alive in d seconds.
 *
 * @param t		elapsed time the node has been alive
 * @param d		future delta time for which we want the probability
 *
 * @return the probability that the node will be alive in d seconds.
 */
double
stable_alive_probability(time_delta_t t, time_delta_t d)
{
	double th = t / 3600.0;
	double dh = d / 3600.0;

	if (0 == t + d)
		return 0.0;

	/*
	 * See leading file comment for an explanation.
	 *
	 * NB: this is a theoretical probability, implied by our choice for
	 * the probability density function.
	 */

	if (t >= 3600) {
		return th / (th + dh);
	} else {
		/*
		 * The area under f(x) between 1 and +oo is 0.5
		 * The area under f(x) between x and 1 is 0.5 - x/2
		 */

		if (t + d >= 3600) {
			double a1 = 1.0 / (2.0 * (th + dh));	/* From t+d to +oo */
			double a2 = 1.0 - th / 2.0;				/* From t to +oo */
			return a1 / a2;
		} else {
			double a1 = 1.0 - (th + dh) / 2.0;		/* From t+d to +oo */
			double a2 = 1.0 - th / 2.0;				/* From t to +oo */
			return a1 / a2;
		}
	}
}

/**
 * Record activity on the node.
 */
void
stable_record_activity(const knode_t *kn)
{
	struct lifedata *ld;
	struct lifedata new_ld;

	knode_check(kn);
	g_assert(kn->flags & KNODE_F_ALIVE);

	ld = get_lifedata(kn->id);

	if (NULL == ld) {
		ld = &new_ld;

		new_ld.version = LIFEDATA_STRUCT_VERSION;
		new_ld.first_seen = kn->last_seen;
		new_ld.last_seen = new_ld.first_seen;

		gnet_stats_count_general(GNR_DHT_STABLE_NODES_HELD, +1);
	} else {
		if (kn->last_seen <= ld->last_seen)
			return;
		ld->last_seen = kn->last_seen;
	}

	dbmw_write(db_lifedata, kn->id->v, ld, sizeof *ld);
}

/**
 * Estimate probability of presence for a value published to some roots in
 * a given time frame.
 *
 * @param d			how many seconds in the future?
 * @param rs		the STORE lookup path, giving root candidates
 * @param status	the array of STORE status for each entry in the path
 *
 * @return an estimated probability of presence of the value in the network.
 */
double
stable_store_presence(time_delta_t d,
	const lookup_rs_t *rs, const guint16 *status)
{
	double q = 1.0;
	size_t i;
	size_t count = lookup_result_path_length(rs);

	/*
	 * The probability of presence is (1 - q) where q is the probability
	 * that the value be lost by all the nodes, i.e. that all the nodes
	 * to which the value was published to be gone in "d" seconds.
	 */

	for (i = 0; i < count; i++) {
		if (status[i] == STORE_SC_OK) {
			const knode_t *kn = lookup_result_nth_node(rs, i);
			struct lifedata *ld = get_lifedata(kn->id);

			if (NULL == ld) {
				return 0.0;		/* Cannot compute a suitable probability */
			} else {
				time_delta_t alive = delta_time(ld->last_seen, ld->first_seen);
				double p = stable_alive_probability(alive, d);

				q *= (1.0 - p);	/* (1 - p) is proba this node will be gone */
			}
		}
	}

	return 1.0 - q;
}

/**
 * DBMW foreach iterator to remove old entries.
 * @return  TRUE if entry must be deleted.
 */
static gboolean
prune_old(gpointer key, gpointer value, size_t u_len, gpointer u_data)
{
	const kuid_t *id = key;
	const struct lifedata *ld = value;
	time_delta_t d;

	(void) u_len;
	(void) u_data;

	d = delta_time(tm_time(), ld->last_seen);

	if (GNET_PROPERTY(dht_stable_debug) > 4) {
		g_message("DHT STABLE node %s life=%s last_seen=%s%s",
			kuid_to_hex_string(id),
			compact_time(delta_time(tm_time(), ld->first_seen)),
			compact_time2(d), d > STABLE_EXPIRE ? " [EXPIRED]" : "");
	}

	return d > STABLE_EXPIRE;
}

/**
 * Prune the database, removing old entries not updated since at least
 * STABLE_EXPIRE seconds.
 */
static void
stable_prune_old(void)
{
	if (GNET_PROPERTY(dht_stable_debug)) {
		g_message("DHT STABLE pruning old stable node records (%lu)",
			(unsigned long) dbmw_count(db_lifedata));
	}

	dbmw_foreach_remove(db_lifedata, prune_old, NULL);
	gnet_stats_set_general(GNR_DHT_STABLE_NODES_HELD, dbmw_count(db_lifedata));

	if (GNET_PROPERTY(dht_stable_debug)) {
		g_message("DHT STABLE pruned old stable node records (%lu remaining)",
			(unsigned long) dbmw_count(db_lifedata));
	}
}

/**
 * Callout queue periodic event to expire old entries.
 */
static gboolean
stable_periodic_prune(gpointer unused_obj)
{
	(void) unused_obj;

	if (NULL == db_lifedata)
		return FALSE;	/* Stop calling */

	stable_prune_old();
	return TRUE;		/* Keep calling */
}

/**
 * Callout queue periodic event to synchronize persistent DB.
 */
static gboolean
stable_sync(gpointer unused_obj)
{
	(void) unused_obj;

	if (NULL == db_lifedata)
		return FALSE;	/* Stop calling */

	storage_sync(db_lifedata);
	return TRUE;
}

/**
 * Serialization routine for lifedata.
 */
static void
serialize_lifedata(pmsg_t *mb, gconstpointer data)
{
	const struct lifedata *ld = data;

	pmsg_write_u8(mb, LIFEDATA_STRUCT_VERSION);
	pmsg_write_time(mb, ld->first_seen);
	pmsg_write_time(mb, ld->last_seen);
}

/**
 * Deserialization routine for lifedata.
 */
static void
deserialize_lifedata(bstr_t *bs, gpointer valptr, size_t len)
{
	struct lifedata *ld = valptr;

	g_assert(sizeof *ld == len);

	bstr_read_u8(bs, &ld->version);
	bstr_read_time(bs, &ld->first_seen);
	bstr_read_time(bs, &ld->last_seen);
}

/**
 * Initialize node stability caching.
 */
void
stable_init(void)
{
	db_lifedata = storage_open(db_stable_what, db_stable_base,
		KUID_RAW_SIZE, sizeof(struct lifedata), 0,
		serialize_lifedata, deserialize_lifedata, NULL,
		STABLE_DB_CACHE_SIZE, sha1_hash, sha1_eq);

	dbmw_set_map_cache(db_lifedata, STABLE_MAP_CACHE_SIZE);
	stable_prune_old();

	cq_periodic_add(callout_queue, STABLE_SYNC_PERIOD, stable_sync, NULL);
	cq_periodic_add(callout_queue, STABLE_PRUNE_PERIOD,
		stable_periodic_prune, NULL);
}

/**
 * Close node stability caching.
 */
void
stable_close(void)
{
	storage_close(db_lifedata, db_stable_base);
	db_lifedata = NULL;
}

/* vi: set ts=4 sw=4 cindent: */