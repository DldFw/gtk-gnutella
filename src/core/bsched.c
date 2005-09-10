/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Raphael Manfredi
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
 * @ingroup core
 * @file
 *
 * Bandwidth scheduling.
 *
 * @author Raphael Manfredi
 * @date 2002-2003
 */

#include "common.h"

RCSID("$Id$");

#include "bsched.h"
#include "inet.h"
#include "uploads.h"

#include "if/core/wrap.h"		/* For wrapped_io_t */
#include "if/gnet_property_priv.h"

#include "lib/glib-missing.h"
#include "lib/walloc.h"
#include "lib/misc.h"
#include "lib/override.h"		/* Must be the last header included */


/*
 * Global bandwidth schedulers.
 */

struct bws_set bws = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
static GSList *bws_list = NULL;
static GSList *bws_gsteal_list = NULL;
static GSList *bws_out_list = NULL;
static GSList *bws_in_list = NULL;
static gint bws_out_ema = 0;
static gint bws_in_ema = 0;

#define BW_SLOT_MIN		64	 /**< Minimum bandwidth/slot for realloc */

#define BW_OUT_UP_MIN	8192 /**< Minimum out bandwidth for becoming ultra */
#define BW_OUT_GNET_MIN	128	 /**< Minimum out bandwidth per Gnet connection */
#define BW_OUT_LEAF_MIN	32	 /**< Minimum out bandwidth per leaf connection */

#define BW_TCP_MSG		40	 /**< Smallest size of a TCP message */
#define BW_UDP_MSG		20	 /**< Minimal IP overhead for a UDP message */

#define BW_UDP_OVERSIZE	512	 /**< Allow that many bytes over available b/w */

/*
 * Determine how large an I/O vector the kernel can accept.
 */

#if defined(MAXIOV)
#define MAX_IOV_COUNT	MAXIOV			/**< Regular */
#elif defined(UIO_MAXIOV)
#define MAX_IOV_COUNT	UIO_MAXIOV		/**< Linux */
#elif defined(IOV_MAX)
#define MAX_IOV_COUNT	IOV_MAX			/**< Solaris */
#else
#define MAX_IOV_COUNT	16				/**< Unknown, use required minimum */
#endif

/**
 * Wrapper over writev() ensuring that we don't request more than
 * MAX_IOV_COUNT entries at a time.
 */
static ssize_t
safe_writev(wrap_io_t *wio, struct iovec *iov, gint iovcnt)
{
	size_t sent = 0;
	struct iovec *end = iov + iovcnt;
	struct iovec *siov;
	gint siovcnt = MAX_IOV_COUNT;
	gint iovsent = 0;

	for (siov = iov; siov < end; siov += siovcnt) {
		ssize_t r;
		size_t size;
		struct iovec *xiv;
		struct iovec *xend;

		siovcnt = iovcnt - iovsent;
		if (siovcnt > MAX_IOV_COUNT)
			siovcnt = MAX_IOV_COUNT;
		g_assert(siovcnt > 0);

		r = wio->writev(wio, siov, siovcnt);

		if ((ssize_t) -1 == r || 0 == r) {
			if (r == 0 || sent)
				break;				/* Don't flag error if bytes sent */
			return -1;				/* Propagate error */
		}

		sent += r;
		iovsent += siovcnt;		/* We'll break out if we did not send it all */

		/*
		 * How much did we sent?  If not the whole vector, we're blocking,
		 * so stop writing and return amount we sent.
		 */

		for (size = 0, xiv = siov, xend = siov + siovcnt; xiv < xend; xiv++)
			size += xiv->iov_len;

		if ((size_t) r < size)
			break;
	}

	return sent;
}

/**
 * Create a new bandwidth scheduler.
 *
 * @param `name' no brief description.
 * @param `type' refers to the scheduling model.  Only BS_T_STREAM for now.
 * @param `mode' refers to the nature of the sources: either reading or writing.
 * @param `bandwidth' is the expected bandwidth in bytes per second.
 * @param `period' is the scheduling period in ms.
 */
bsched_t *
bsched_make(gchar *name, gint type, guint32 mode, gint bandwidth, gint period)
{
	bsched_t *bs;

	/* Must contain either reading or writing sources */
	g_assert(mode & BS_F_RW);
	g_assert((mode & BS_F_RW) != BS_F_RW);
	g_assert(!(mode & ~BS_F_RW));

	g_assert(bandwidth >= 0);
	g_assert(period > 0);
	g_assert(type == BS_T_STREAM);		/* XXX only mode supported for now */
	g_assert(bandwidth <= BS_BW_MAX);	/* Signed, and multiplied by 1000 */

	bs = (bsched_t *) g_malloc0(sizeof(*bs));

	bs->name = g_strdup(name);
	bs->flags = mode;
	bs->type = type;
	bs->period = period;
	bs->min_period = period >> 1;		/* 50% of nominal period */
	bs->max_period = period << 1;		/* 200% of nominal period */
	bs->period_ema = period;
	bs->bw_per_second = bandwidth;
	bs->bw_max = (gint) (bandwidth / 1000.0 * period);

	return bs;
}

/**
 * Free bandwidth scheduler.
 *
 * All sources have their bsched pointer reset to NULL but are not disposed of.
 * Naturally, they cannot be used for I/O any more.
 */
static void
bsched_free(bsched_t *bs)
{
	GList *l;

	for (l = bs->sources; l; l = g_list_next(l)) {
		bio_source_t *bio = (bio_source_t *) l->data;

		g_assert(bio->bs == bs);
		bio->bs = NULL;				/* Mark orphan source */
	}

	g_list_free(bs->sources);
	g_slist_free(bs->stealers);
	G_FREE_NULL(bs->name);
	G_FREE_NULL(bs);
}

/**
 * Add `stealer' as a bandwidth stealer for underused bandwidth in `bs'.
 * Both must be either reading or writing schedulers.
 */
static void
bsched_add_stealer(bsched_t *bs, bsched_t *stealer)
{
	g_assert(bs != stealer);
	g_assert((bs->flags & BS_F_RW) == (stealer->flags & BS_F_RW));

	bs->stealers = g_slist_prepend(bs->stealers, stealer);
}

/**
 * Reset `stealer' list for scheduler.
 */
static void
bsched_reset_stealers(bsched_t *bs)
{
	if (bs->stealers != NULL)
		g_slist_free(bs->stealers);
	bs->stealers = NULL;
}

/**
 * Allow cross-stealing of unused bandwidth between HTTP/gnet.
 */
void
bsched_config_steal_http_gnet(void)
{
	GSList *l;

	for (l = bws_list; l; l = g_slist_next(l)) {
		bsched_t *bs = (bsched_t *) l->data;
		bsched_reset_stealers(bs);
	}

	bsched_add_stealer(bws.out, bws.gout);
	bsched_add_stealer(bws.out, bws.gout_udp);
	bsched_add_stealer(bws.out, bws.glout);

	bsched_add_stealer(bws.gout, bws.out);
	bsched_add_stealer(bws.gout, bws.gout_udp);
	bsched_add_stealer(bws.gout, bws.glout);

	bsched_add_stealer(bws.in, bws.gin);
	bsched_add_stealer(bws.in, bws.gin_udp);
	bsched_add_stealer(bws.in, bws.glin);

	bsched_add_stealer(bws.gin, bws.in);
	bsched_add_stealer(bws.gin, bws.gin_udp);
	bsched_add_stealer(bws.gin, bws.glin);

	bsched_add_stealer(bws.glout, bws.gout);
	bsched_add_stealer(bws.glout, bws.gout_udp);
	bsched_add_stealer(bws.glout, bws.out);

	bsched_add_stealer(bws.glin, bws.gin);
	bsched_add_stealer(bws.glin, bws.gin_udp);
	bsched_add_stealer(bws.glin, bws.in);

	bsched_add_stealer(bws.gout_udp, bws.gout);
	bsched_add_stealer(bws.gout_udp, bws.glout);
	bsched_add_stealer(bws.gout_udp, bws.out);

	bsched_add_stealer(bws.gin_udp, bws.gin);
	bsched_add_stealer(bws.gin_udp, bws.glin);
	bsched_add_stealer(bws.gin_udp, bws.in);
}

/**
 * Allow cross-stealing of unused bandwidth between TCP and UDP gnet only.
 */
void
bsched_config_steal_gnet(void)
{
	GSList *l;

	for (l = bws_list; l; l = g_slist_next(l)) {
		bsched_t *bs = (bsched_t *) l->data;
		bsched_reset_stealers(bs);
	}

	bsched_add_stealer(bws.gin, bws.gin_udp);
	bsched_add_stealer(bws.gin_udp, bws.gin);
	bsched_add_stealer(bws.gout, bws.gout_udp);
	bsched_add_stealer(bws.gout_udp, bws.gout);
}

/**
 * Initialize global bandwidth schedulers.
 */
void
bsched_init(void)
{
	bws.out = bsched_make("out",
		BS_T_STREAM, BS_F_WRITE, bw_http_out, 1000);

	bws.gout = bsched_make("G TCP out",
		BS_T_STREAM, BS_F_WRITE, bw_gnet_out / 2, 1000);

	bws.gout_udp = bsched_make("G UDP out",
		BS_T_STREAM, BS_F_WRITE, bw_gnet_out / 2, 1000);

	bws.glout = bsched_make("GL out",
		BS_T_STREAM, BS_F_WRITE, bw_gnet_lout, 1000);

	bws.in = bsched_make("in",
		BS_T_STREAM, BS_F_READ, bw_http_in, 1000);

	bws.gin = bsched_make("G TCP in",
		BS_T_STREAM, BS_F_READ, bw_gnet_in / 2, 1000);

	bws.gin_udp = bsched_make("G UDP in",
		BS_T_STREAM, BS_F_READ, bw_gnet_in / 2, 1000);

	bws.glin = bsched_make("GL in",
		BS_T_STREAM, BS_F_READ, bw_gnet_lin, 1000);

	bws_list = g_slist_prepend(bws_list, bws.glin);
	bws_list = g_slist_prepend(bws_list, bws.gin);
	bws_list = g_slist_prepend(bws_list, bws.gin_udp);
	bws_list = g_slist_prepend(bws_list, bws.in);
	bws_list = g_slist_prepend(bws_list, bws.glout);
	bws_list = g_slist_prepend(bws_list, bws.gout);
	bws_list = g_slist_prepend(bws_list, bws.gout_udp);
	bws_list = g_slist_prepend(bws_list, bws.out);

	bws_in_list = g_slist_prepend(bws_in_list, bws.glin);
	bws_in_list = g_slist_prepend(bws_in_list, bws.gin);
	bws_in_list = g_slist_prepend(bws_in_list, bws.gin_udp);
	bws_in_list = g_slist_prepend(bws_in_list, bws.in);

	bws_out_list = g_slist_prepend(bws_out_list, bws.glout);
	bws_out_list = g_slist_prepend(bws_out_list, bws.gout);
	bws_out_list = g_slist_prepend(bws_out_list, bws.gout_udp);
	bws_out_list = g_slist_prepend(bws_out_list, bws.out);

	bws_gsteal_list = g_slist_prepend(bws_gsteal_list, bws.gout);
	bws_gsteal_list = g_slist_prepend(bws_gsteal_list, bws.gout_udp);
	bws_gsteal_list = g_slist_prepend(bws_gsteal_list, bws.gin);
	bws_gsteal_list = g_slist_prepend(bws_gsteal_list, bws.gin_udp);

	/*
	 * We always steal bandwidth between TCP and UDP gnet, since we
	 * forcefully split the allocated bandwidth evenly between the
	 * two traffic types.
	 */

	if (bw_allow_stealing)
		bsched_config_steal_http_gnet();
	else
		bsched_config_steal_gnet();

	bsched_set_peermode(current_peermode);
}

/**
 * Discard global bandwidth schedulers.
 */
void
bsched_close(void)
{
	GSList *l;

	for (l = bws_list; l; l = g_slist_next(l))
		bsched_free(l->data);

	g_slist_free(bws_list);
	g_slist_free(bws_out_list);
	g_slist_free(bws_in_list);
	g_slist_free(bws_gsteal_list);

	bws.out = bws.in = bws.gout = bws.gin = bws.glin = bws.glout = NULL;
	bws.gout_udp = bws.gin_udp = NULL;
}

/**
 * Adapt the overall Gnet/HTTP bandwidth repartition depending on the current
 * peermode.
 *
 * This routine is called each time the peermode changes or each time the
 * settings for the traffic shapers changes.
 */
void
bsched_set_peermode(node_peer_t mode)
{
	guint32 steal;

	switch (mode) {
	case NODE_P_NORMAL:
	case NODE_P_LEAF:
		bsched_set_bandwidth(bws.glin, 1);		/* 0 would disable it */
		bsched_set_bandwidth(bws.glout, 1);
		bsched_set_bandwidth(bws.in, bw_http_in);
		bsched_set_bandwidth(bws.out, bw_http_out);
		break;
	case NODE_P_ULTRA:
		/*
		 * If leaf traffic shaper is enabled, steal bandwidth from HTTP.
		 * Otherwise, bandwidth is unlimited.
		 */

		steal = MIN(bw_http_in, bw_gnet_lin);
		if (bws_glin_enabled && steal) {
			bsched_set_bandwidth(bws.glin, steal);
			bsched_set_bandwidth(bws.in, MAX(1, bw_http_in - steal));
		} else {
			bsched_set_bandwidth(bws.glin, 0);			/* Disables */
			bsched_set_bandwidth(bws.in, bw_http_in);
		}

		steal = MIN(bw_http_out, bw_gnet_lout);
		if (bws_glout_enabled && steal) {
			bsched_set_bandwidth(bws.glout, steal);
			bsched_set_bandwidth(bws.out, MAX(1, bw_http_out - steal));
		} else {
			bsched_set_bandwidth(bws.glout, 0);			/* Disables */
			bsched_set_bandwidth(bws.out, bw_http_out);
		}

		if (bws.glin->bw_per_second && bws_glin_enabled)
			bsched_enable(bws.glin);
		if (bws.glout->bw_per_second && bws_glout_enabled)
			bsched_enable(bws.glout);
		break;
	default:
		g_error("unhandled peer mode %d", mode);
	}
}

/**
 * Enable scheduling, marks the start of the period.
 */
void
bsched_enable(bsched_t *bs)
{

	g_assert(bs);

	bs->flags |= BS_F_ENABLED;
	tm_now(&bs->last_period);
}

/**
 * Disable scheduling.
 */
void
bsched_disable(bsched_t *bs)
{
	g_assert(bs);

	bs->flags &= ~BS_F_ENABLED;
}

/**
 * Enable all known bandwidth schedulers.
 */
void
bsched_enable_all(void)
{
	if (bws.out->bw_per_second && bws_out_enabled)
		bsched_enable(bws.out);

	if (bws.gout->bw_per_second && bws_gout_enabled) {
		bsched_enable(bws.gout);
		bsched_enable(bws.gout_udp);
	}

	if (bws.glout->bw_per_second && bws_glout_enabled)
		bsched_enable(bws.glout);

	if (bws.in->bw_per_second && bws_in_enabled)
		bsched_enable(bws.in);

	if (bws.gin->bw_per_second && bws_gin_enabled) {
		bsched_enable(bws.gin);
		bsched_enable(bws.gin_udp);
	}

	if (bws.glin->bw_per_second && bws_glin_enabled)
		bsched_enable(bws.glin);
}

/**
 * Shutdowning program.
 * Disable all known bandwidth schedulers, so that any pending I/O can
 * go through as quickly as possible.
 */
void
bsched_shutdown(void)
{
	GSList *sl;

	for (sl = bws_list; sl; sl = g_slist_next(sl))
		bsched_disable(sl->data);
}

/**
 * Enable an I/O source.
 */
static void
bio_enable(bio_source_t *bio)
{
	g_assert(0 == bio->io_tag);
	g_assert(bio->io_callback);		/* "passive" sources not concerned */

	bio->io_tag = inputevt_add(bio->wio->fd(bio->wio),
			(bio->flags & BIO_F_READ) ? INPUT_EVENT_RX : INPUT_EVENT_WX,
			bio->io_callback, bio->io_arg);

	g_assert(bio->io_tag);
}

/**
 * Disable I/O source.
 *
 * The value of `bw_available' is ignored, as this is a fairly low-level call.
 * If it is called, then the caller has already taken care of redispatching
 * any remaining bandwidth.
 */
static void
bio_disable(bio_source_t *bio)
{
	g_assert(bio->io_tag);
	g_assert(bio->io_callback);		/* "passive" sources not concerned */

	g_source_remove(bio->io_tag);
	bio->io_tag = 0;
}

/**
 * Add I/O callback to a "passive" I/O source.
 */
void
bio_add_callback(bio_source_t *bio, inputevt_handler_t callback, gpointer arg)
{
	g_assert(bio);
	g_assert(bio->io_callback == NULL);	/* "passive" source */
	g_assert(callback);

	bio->io_callback = callback;
	bio->io_arg = arg;

	if (!(bio->bs->flags & BS_F_NOBW))
		bio_enable(bio);
}

/**
 * Remove I/O callback from I/O source.
 */
void
bio_remove_callback(bio_source_t *bio)
{
	g_assert(bio);
	g_assert(bio->io_callback);		/* Not a "passive" source */

	if (bio->io_tag)
		bio_disable(bio);

	bio->io_callback = NULL;
	bio->io_arg = NULL;
}


/**
 * Disable all sources and flag that we have no more bandwidth.
 */
static void
bsched_no_more_bandwidth(bsched_t *bs)
{
	GList *l;

	for (l = bs->sources; l; l = g_list_next(l)) {
		bio_source_t *bio = (bio_source_t *) l->data;

		g_assert(bio != NULL);

		if (bio->io_tag)
			bio_disable(bio);
	}

	bs->flags |= BS_F_NOBW;
}

/**
 * Remove activation indication on all the sources.
 */
static void
bsched_clear_active(bsched_t *bs)
{
	GList *l;

	for (l = bs->sources; l; l = g_list_next(l)) {
		bio_source_t *bio = (bio_source_t *) l->data;

		g_assert(bio != NULL);

		bio->flags &= ~BIO_F_ACTIVE;
	}
}

/**
 * Called whenever a new scheduling timeslice begins.
 *
 * Re-enable all sources and flag that we have bandwidth.
 * Update the per-source bandwidth statistics.
 * Clears all activation indication on all sources.
 */
static void
bsched_begin_timeslice(bsched_t *bs)
{
	GList *l;
	GList *last = NULL;
	gdouble norm_factor = 1000.0 / bs->period;
	gint count;

	for (count = 0, l = bs->sources; l; l = g_list_next(l)) {
		bio_source_t *bio = (bio_source_t *) l->data;
		guint32 actual;

		g_assert(bio != NULL);

		last = l;			/* Remember last seen source  for rotation */
		count++;			/* Count them for assertion */

		bio->flags &= ~(BIO_F_ACTIVE | BIO_F_USED);
		if (bio->io_tag == 0 && bio->io_callback)
			bio_enable(bio);

		/*
		 * Fast EMA of bandwidth is computed on the last n=3 terms.
		 * The smoothing factor, sm=2/(n+1), is therefore 0.5, which is easy
		 * to compute.  The short period gives us a good estimation of the
		 * "instantaneous bandwidth" used.
		 *
		 * Slow EMA of bandwidth is computed on the last n=127 terms, which at
		 * one computation per second, means an average of the two minutes.
		 * This value is smoother and therefore more suited to use for the
		 * remaining time estimates.
		 *
		 * Because we use integer arithmetic (and therefore loose important
		 * decimals), the actual values are shifted by BIO_EMA_SHIFT.
		 * The fields storing the EMAs should therefore only be accessed via
		 * the macros, which perform the shift in the other way to
		 * re-establish proper scaling.
		 */

		actual = bio->bw_actual << BIO_EMA_SHIFT;
		bio->bw_fast_ema += (actual >> 1) - (bio->bw_fast_ema >> 1);
		bio->bw_slow_ema += (actual >> 6) - (bio->bw_slow_ema >> 6);
		bio->bw_last_bps = (guint) (bio->bw_actual * norm_factor);
		bio->bw_actual = 0;
	}

	g_assert(bs->count == count);	/* All sources are there */

	/*
	 * Rotate sources, since we don't know how glib handles callbacks on
	 * the registered sources.  We don't want to always have the same
	 * sources get most of the bandwidth because they simply get added
	 * first as I/O sources.
	 */

	if (last != NULL && last != bs->sources) {
		bio_source_t *bio;
		g_assert(bs->sources != NULL);
		bio = (bio_source_t *) bs->sources->data;
		g_assert(bio != NULL);
		bs->sources = g_list_remove(bs->sources, bio);
		bs->sources = gm_list_insert_after(bs->sources, last, bio);
	}

	bs->flags &= ~(BS_F_NOBW|BS_F_FROZEN_SLOT|BS_F_CHANGED_BW|BS_F_CLEARED);

	/*
	 * On the first round of source dispatching, don't use the stolen b/w.
	 * Only introduce it when we come back to a source we already
	 * scheduled, to avoid spending bandwidth too early when we have
	 * many sources in various schedulers stealing each other some
	 * bandwidth that could starve others.
	 *
	 * In other words, don't distribute (bs->bw_max + bs->bw_stolen)
	 * among all the slots, but only bs->bw_max.  The remaining
	 * will be distributed by bw_available().
	 *
	 * We artificially raise the bandwidth per slot if we have some capped
	 * bandwidth recorded for the previous timeslice, meaning we did not used
	 * all our (writing) bandwidth and yet refused some bandwidth to active
	 * sources.
	 *
	 * Finally, if we did not use all our sources last time, we give more
	 * bandwidth to active sources.  We add 1 to the amount of sources used
	 * to avoid the same sources using all the bandwidth each time before
	 * it runs out for the time slice.
	 */

	if (bs->count) {
		gint dividor = bs->count;
		if (bs->last_used > 0 && bs->last_used < bs->count)
			dividor = bs->last_used + 1;
		bs->bw_slot = (bs->bw_max + bs->bw_last_capped) / dividor;
	} else
		bs->bw_slot = 0;

	/*
	 * If the slot is less than the minimum we can reach by dynamically
	 * adjusting the bandwidth, then don't bother trying and freeze it.
	 */

	if (bs->bw_slot < BW_SLOT_MIN && bs->bw_stolen == 0)
		bs->flags |= BS_F_FROZEN_SLOT;

	/*
	 * Reset the amount of data we could not write due to kernel flow-control,
	 * and the amount of capped bandwidth for the period.
	 */

	bs->bw_unwritten = 0;			/* Even if `bs' is for read sources... */
	bs->bw_capped = 0;

	bs->current_used = 0;
	bs->looped = FALSE;
}

/**
 * Add new source to the source list of scheduler.
 */
static void
bsched_bio_add(bsched_t *bs, bio_source_t *bio)
{
	g_assert(bio != NULL);

	bs->sources = g_list_append(bs->sources, bio);
	bs->count++;

	bs->bw_slot = (bs->bw_max + bs->bw_stolen) / bs->count;

	/*
	 * If the slot is less than the minimum we can reach by dynamically
	 * adjusting the bandwidth, then don't bother trying and freeze it.
	 */

	if (bs->bw_slot < BW_SLOT_MIN)
		bs->flags |= BS_F_FROZEN_SLOT;
}

/**
 * Remove source from the source list of scheduler.
 */
static void
bsched_bio_remove(bsched_t *bs, bio_source_t *bio)
{
	g_assert(bio != NULL);

	bs->sources = g_list_remove(bs->sources, bio);
	bs->count--;

	if (bs->count)
		bs->bw_slot = (bs->bw_max + bs->bw_stolen) / bs->count;

	g_assert(bs->count >= 0);
}

/**
 * Declare fd as a new source for the scheduler.
 *
 * When `callback' is NULL, the source will be "passive", i.e. its bandwidth
 * will be limited when calls to bio_write() or bio_read() are made, but
 * whether the source can accept those calls without blocking will have to
 * be determined explicitly.
 *
 * @returns new bio_source object.
 */
bio_source_t *
bsched_source_add(
	bsched_t *bs, wrap_io_t *wio, guint32 flags,
	inputevt_handler_t callback, gpointer arg)
{
	bio_source_t *bio;

	/*
	 * Must insert reading sources in reading scheduler and writing ones
	 * in a writing scheduler.
	 */

	g_assert(!(bs->flags & BS_F_READ) == !(flags & BIO_F_READ));
	g_assert(flags & BIO_F_RW);
	g_assert((flags & BIO_F_RW) != BIO_F_RW);	/* Either reading or writing */
	g_assert(!(flags & ~BIO_F_RW));				/* Can only specify r/w flags */

	bio = walloc0(sizeof *bio);

	bio->bs = bs;
	bio->wio = wio;
	bio->flags = flags;
	bio->io_callback = callback;
	bio->io_arg = arg;

	/*
	 * If there is no callback, the I/O source is "passive".  The supplier
	 * has means to know whether it can read/write from the source, and only
	 * uses the scheduler to limit the amount of data read/written from/to
	 * that source.
	 */

	bsched_bio_add(bs, bio);

	if (!(bs->flags & BS_F_NOBW) && bio->io_callback)
		bio_enable(bio);

	return bio;
}

/**
 * Remove bio_source object from the scheduler.
 * The bio_source object is freed and must not be re-used.
 */
void
bsched_source_remove(bio_source_t *bio)
{
	bsched_t *bs = bio->bs;

	if (bs)
		bsched_bio_remove(bs, bio);
	if (bio->io_tag)
		g_source_remove(bio->io_tag);

	wfree(bio, sizeof *bio);
}

/**
 * On-the-fly changing of the allowed bandwidth.
 */
void
bsched_set_bandwidth(bsched_t *bs, gint bandwidth)
{
	g_assert(bs);
	g_assert(bandwidth >= 0);
	g_assert(bandwidth <= BS_BW_MAX);	/* Signed, and multiplied by 1000 */

	bs->bw_per_second = bandwidth;
	bs->bw_max = (gint) (bandwidth / 1000.0 * bs->period);

	/*
	 * If `bandwidth' is 0, then we're disabling bandwidth scheduling and
	 * allow all traffic to go through, unlimited.
	 *
	 * NB: at the next heartbeat, bsched_begin_timeslice() will be called
	 * to re-enable all the sources if any were disabled.
	 */

	if (bandwidth == 0) {
		bsched_disable(bs);
		return;
	}

	/*
	 * When all bandwidth has been used, disable all sources.
	 */

	if (bs->bw_actual >= (bs->bw_max + bs->bw_stolen))
		bsched_no_more_bandwidth(bs);

	bs->flags |= BS_F_CHANGED_BW;
}


/**
 * @param `bio' no brief description.
 * @param `len' is the amount of bytes requested by the application.
 *
 * @returns the bandwidth available for a given source.
 */
static gint
bw_available(bio_source_t *bio, gint len)
{
	bsched_t *bs = bio->bs;
	gint available;
	gint result;
	gboolean capped = FALSE;
	gboolean used;
	gboolean active;

	if (!(bs->flags & BS_F_ENABLED))		/* Scheduler disabled */
		return len;							/* Use amount requested */

	if (bs->flags & BS_F_NOBW)				/* No more bandwidth */
		return 0;							/* Grant nothing */

	if (bio->io_callback && !bio->io_tag)	/* Source already disabled */
		return 0;							/* No bandwidth available */

	/*
	 * If source was already active, recompute the per-slot value since
	 * we already looped once through all the sources.  This prevents the
	 * first scheduled sources to eat all the bandwidth.
	 *
	 * At this point, we'll distribute the stolen bandwidth, which was
	 * not initially distributed.  If the stolen bandwidth is an order of
	 * magnitude larger than the regular bandwidth (bs->bw_max), distribute
	 * only the regular bandwidth for now.  Hence the test below.
	 */

	available = bs->bw_max + bs->bw_stolen - bs->bw_actual;

	if (dbg > 8)
		printf("bw_available: "
			"[fd #%d] max=%d, stolen=%d, actual=%d => avail=%d\n",
			bio->wio->fd(bio->wio), bs->bw_max, bs->bw_stolen, bs->bw_actual,
			available);

	if (available > bs->bw_max) {
		available = bs->bw_max;
		capped = TRUE;
	}

	/*
	 * The BIO_F_USED flag is set only once during a period, and is used
	 * to identify sources that already triggered.
	 *
	 * The BIO_F_ACTIVE flag is used to mark a source as being used as well,
	 * but can be cleared during a period, when we redistribute bandwidth
	 * among the slots.  So the flag is set when the source was already
	 * used since we recomputed the bandwidth per slot.
	 */

	used = bio->flags & BIO_F_USED;
	active = bio->flags & BIO_F_ACTIVE;

	if (dbg > 8)
		printf("\tcapped=%s, used=%s, active=%s => avail=%d\n",
			capped ? "y" : "n", used ? "y" : "n", active ? "y" : "n",
			available);

	if (!used) {
		bs->current_used++;
		bio->flags |= BIO_F_USED;
	}

	bio->flags |= BIO_F_ACTIVE;

	/*
	 * Set the `looped' flag the first time when we encounter a source that
	 * was already marked used.  It means it is the second time we see
	 * that source trigger during the period and it means we already gave
	 * a chance to all the other sources to trigger.
	 */

	if (!bs->looped && used)
		bs->looped = TRUE;

	if (
		!(bs->flags & BS_F_FROZEN_SLOT) &&
		available > BW_SLOT_MIN &&
		active
	) {
		gint slot = available / bs->count;

		/*
		 * It's not worth redistributing less than BW_SLOT_MIN bytes per slot.
		 * If we ever drop below that value, freeze the slot value to prevent
		 * further redistribution.
		 *
		 * We don't freeze when the amount available which was redistributed
		 * is equal to the regular bandwidth for the scheduler.  This usually
		 * happens when there is some stolen bandwidth that is not used yet.
		 *
		 * NB: we don't freeze the slots if we capped the redistribution above,
		 * because we have more stolen bandwidth to possibly use.
		 *
		 * NB: we run bsched_clear_active() only ONCE per period, because if
		 * we have to run it more, it means that a few set of sources are
		 * very active: just give them bandwidth now, other sources had their
		 * fair chance to trigger and things may change at the next period.
		 */

		if (capped || slot > BW_SLOT_MIN) {
			if (!(bs->flags & BS_F_CLEARED)) {	/* Only once per period */
				bsched_clear_active(bs);
				bs->flags |= BS_F_CLEARED;
			}
			bs->bw_slot = slot;
		} else {
			bs->flags |= BS_F_FROZEN_SLOT;
			bs->bw_slot = BW_SLOT_MIN;
		}

		if (dbg > 7)
			printf("bw_availble: new slot=%d for \"%s\" (%scapped)\n",
				bs->bw_slot, bs->name, capped ? "" : "un");
	}

	/*
	 * If nothing is available, disable all sources.
	 */

	if (available <= 0) {
		bsched_no_more_bandwidth(bs);
		available = 0;
	}

	result = MIN(bs->bw_slot, available);
	available -= result;

	/*
	 * If `bw_last_capped' is not zero, we had to cap the traffic last period,
	 * even though we did not use the whole allocated bandwidth.
	 *
	 * If the source is not flagged as `used', then we already looped through
	 * the other active sources and consumed some bandwidth.
	 *
	 * So if we already looped through the sources, try to consume more data
	 * this time.  The rationale is that we might not write enough data
	 * for each active source, and we don't loop enough time over the sources
	 * to be able to fill our bandwidth allocation.
	 */

	if (
		result < len && available > 0 && bs->looped &&
		(!used || bs->bw_last_capped > 0)
	) {
		gint adj = len - result;
		gint nominal;

		if (bs->bw_last_capped > 0 && bs->bw_last_period < bs->bw_max) {
			gint distribute = MAX(bs->bw_last_capped, available);

			/*
			 * We have capped bandwidth last period, yet we consumed less
			 * than what we were allowed to.
			 *
			 * When source was not used yet, we rely on the previously used
			 * source count, since we don't know how many more sources will
			 * trigger this period.
			 */

			if (used) {
				g_assert(bs->current_used != 0);	/* This source is used! */
				nominal = distribute / bs->current_used;
			} else
				nominal = distribute / MAX(bs->last_used, bs->current_used);
		} else {
			/*
			 * Try to stuff 2 slots worth of data
			 *
			 * If we never used that source, we use the nominal bandwidth for
			 * each slot.  Otherwise we use the current per-slot bandwidth.
			 */

			if (used)
				nominal = 2 * bs->bw_slot;
			else
				nominal = 2 * bs->bw_max / bs->count;
		}

		if (adj > nominal)
			adj = nominal;

		if (adj > available)
			adj = available;

		if (dbg > 4)
			printf("bw_available: \"%s\" adding %d to %d"
				" (len=%d, capped=%d [%d-%d/%d], available=%d, used=%c)\n",
				bs->name, adj, result, len, bs->bw_last_capped,
				bs->last_used, bs->current_used, bs->count,
				available, (bio->flags & BIO_F_USED) ? 'y' : 'n');

		result += adj;
	}

	/*
	 * If we return less than the amount requested, we capped the bandwidth.
	 *
	 * Keep track of that bandwidth, because if we end-up having consumed
	 * less that what we should and we have some capped bandwidth, it means
	 * we're not distributing it correctly: the sources don't trigger "fast
	 * enough" during the period.
	 */

	if (result < len)
		bs->bw_capped += len - result;

	return result;
}

/**
 * Update bandwidth used, and scheduler statistics.
 * If no more bandwidth is available, disable all sources.
 *
 * @param `bs' no brief description.
 * @param `used' is the amount of bytes used by the I/O.
 * @param `requested' is the amount of bytes requested for the I/O.
 */
static void
bsched_bw_update(bsched_t *bs, gint used, gint requested)
{
	g_assert(bs);		/* Ensure I/O source was in alive scheduler */
	g_assert(used <= requested);

	/*
	 * Even when the scheduler is disabled, update the actual bandwidth used
	 * for the statistics and the GUI display.
	 */

	bs->bw_actual += used;

	if (!(bs->flags & BS_F_ENABLED))		/* Scheduler disabled */
		return;								/* Nothing to update */

	/*
	 * For writing schedulers, sum-up the difference between the amount of
	 * data that we originally wished to write and the amount that got
	 * actually written.  If it is positive, it means the kernel
	 * flow-controlled the connection.
	 *
	 * For reading shedulers, we don't care about that difference.
	 */

	if (bs->flags & BS_F_WRITE)
		bs->bw_unwritten += requested - used;

	/*
	 * When all bandwidth has been used, disable all sources.
	 */

	if (bs->bw_actual >= (bs->bw_max + bs->bw_stolen))
		bsched_no_more_bandwidth(bs);
}

/**
 * Write at most `len' bytes from `buf' to source's fd, as bandwidth permits.
 * If we cannot write anything due to bandwidth constraints, return -1 with
 * errno set to EAGAIN.
 */
ssize_t
bio_write(bio_source_t *bio, gconstpointer data, size_t len)
{
	size_t available;
	size_t amount;
	ssize_t r;

	g_assert(bio);
	g_assert(bio->flags & BIO_F_WRITE);

	/*
	 * If we don't have any bandwidth, return -1 with errno set to EAGAIN
	 * to signal that we cannot perform any I/O right now.
	 */

	available = bw_available(bio, len);

	if (available == 0) {
		errno = EAGAIN;
		return -1;
	}

	amount = len > available ? available : len;

	if (dbg > 7)
		printf("bio_write(wio=%d, len=%d) available=%d\n",
			bio->wio->fd(bio->wio), (gint) len, (gint) available);

	r = bio->wio->write(bio->wio, data, amount);

	/*
	 * XXX hack for broken libc, which can return -1 with errno = 0!
	 *
	 * Apparently, when compiling with gcc-3.3.x, one can have a system
	 * call return -1 with errno set to EOK.
	 *		--RAM, 05/10/2003
	 */

	if ((ssize_t) -1 == r && 0 == errno) {
		g_warning("wio->write(fd=%d, len=%d) returned -1 with errno = 0, "
			"assuming EAGAIN", bio->wio->fd(bio->wio), (gint) len);
		errno = EAGAIN;
	}

	if (r > 0) {
		bsched_bw_update(bio->bs, r, amount);
		bio->bw_actual += r;
	}

	return r;
}

/**
 * Write at most `len' bytes from `iov' to source's fd, as bandwidth permits.
 * If we cannot write anything due to bandwidth constraints, return -1 with
 * errno set to EAGAIN.
 */
ssize_t
bio_writev(bio_source_t *bio, struct iovec *iov, gint iovcnt)
{
	size_t available;
	ssize_t r;
	size_t len;
	struct iovec *siov;
	gint slen = -1;			/* Avoid "may be used uninitialized" warning */

	g_assert(bio);
	g_assert(bio->flags & BIO_F_WRITE);

	/*
	 * Compute I/O vector's length.
	 */

	for (r = 0, siov = iov, len = 0; r < iovcnt; r++, siov++)
		len += siov->iov_len;

	/*
	 * If we don't have any bandwidth, return -1 with errno set to EAGAIN
	 * to signal that we cannot perform any I/O right now.
	 */

	available = bw_available(bio, len);

	if (available == 0) {
		errno = EAGAIN;
		return -1;
	}

	/*
	 * If we cannot write the whole vector, we need to trim it.
	 * Because we promise to not corrupt the original I/O vector, we
	 * save the original length of the last I/O entry, should we modify it.
	 */

	if (len > available) {
		size_t curlen;

		for (r = 0, siov = iov, curlen = 0; r < iovcnt; r++, siov++) {
			curlen += siov->iov_len;

			/*
			 * Exact size reached, we just need to adjust down the iov count.
			 * Force siov to NULL before leaving the loop to indicate we did
			 * not have to alter it.
			 */

			if (curlen == available) {
				siov = NULL;
				iovcnt = r + 1;
				break;
			}

			/*
			 * Maximum size reached...  Need to adjust both the iov count
			 * and the length of the current siov entry.
			 */

			if (curlen > available) {
				slen = siov->iov_len;		/* Save for later restore */
				siov->iov_len -= (curlen - available);
				iovcnt = r + 1;
				g_assert(siov->iov_len > 0);
				break;
			}
		}
	}

	/*
	 * Write I/O vector, updating used bandwidth.
	 *
	 * When `iovcnt' is greater than MAX_IOV_COUNT, use our custom writev()
	 * wrapper to avoid failure with EINVAL.
	 *		--RAM, 17/03/2002
	 */

	if (dbg > 7)
		printf("bio_writev(fd=%d, len=%d) available=%d\n",
			bio->wio->fd(bio->wio), (gint) len, (gint) available);

	if (iovcnt > MAX_IOV_COUNT)
		r = safe_writev(bio->wio, iov, iovcnt);
	else
		r = bio->wio->writev(bio->wio, iov, iovcnt);

	/*
	 * XXX hack for broken libc, which can return -1 with errno = 0!
	 *
	 * Apparently, when compiling with gcc-3.3.x, one can have a system
	 * call return -1 with errno set to EOK.
	 *		--RAM, 05/10/2003
	 */

	if ((ssize_t) -1 == r && 0 == errno) {
		g_warning("writev(fd=%d, len=%d) returned -1 with errno = 0, "
			"assuming EAGAIN", bio->wio->fd(bio->wio), (gint) len);
		errno = EAGAIN;
	}

	if (r > 0) {
		g_assert((size_t) r <= available);
		bsched_bw_update(bio->bs, r, MIN(len, available));
		bio->bw_actual += r;
	}

	/*
	 * Restore original I/O vector if we trimmed it.
	 */

	if (len > available && siov) {
		g_assert(slen >= 0);			/* Ensure it was initialized */
		siov->iov_len = slen;
	}

	return r;
}

/**
 * Send UDP datagram to specified destination `to'.
 *
 * @return -1 with errno set to EAGAIN, if we cannot write anything due
 * to bandwidth constraints.
 */
ssize_t
bio_sendto(bio_source_t *bio, gnet_host_t *to, gconstpointer data, size_t len)
{
	size_t available;
	ssize_t r;

	g_assert(bio);
	g_assert(bio->flags & BIO_F_WRITE);

	/*
	 * If we don't have any bandwidth, return -1 with errno set to EAGAIN
	 * to signal that we cannot perform any I/O right now.
	 *
	 * Note that datagrams are necessarily atomic operations, therefore
	 * they must be completely possible or not performed at all, but we
	 * can't really be too strict or large datagrams will never be sent
	 * at all.  Hence we act as if we had BW_UDP_OVERSIZE extra bandwidth
	 * available if we don't have enough bandwidth initially, but have
	 * some available still.
	 */

	available = bw_available(bio, len);

	if (available == 0 || available + BW_UDP_OVERSIZE < len) {
		errno = EAGAIN;
		return -1;
	}

	if (dbg > 7)
		printf("bio_sendto(wio=%d, len=%d) available=%d\n",
			bio->wio->fd(bio->wio), (gint) len, (gint) available);

	g_assert(bio->wio != NULL);
	g_assert(bio->wio->sendto != NULL);
	r = bio->wio->sendto(bio->wio, to, data, len);

	/*
	 * XXX hack for broken libc, which can return -1 with errno = 0!
	 *
	 * Apparently, when compiling with gcc-3.3.x, one can have a system
	 * call return -1 with errno set to EOK.
	 *		--RAM, 05/10/2003
	 */

	if ((ssize_t) -1 == r && 0 == errno) {
		g_warning("wio->sendto(fd=%d, len=%d) returned -1 with errno = 0, "
			"assuming EAGAIN", bio->wio->fd(bio->wio), (gint) len);
		errno = EAGAIN;
	}

	if (r > 0) {
		bsched_bw_update(bio->bs, r + BW_UDP_MSG, len + BW_UDP_MSG);
		bio->bw_actual += r + BW_UDP_MSG;
	}

	return r;
}

#ifdef USE_MMAP
static sigjmp_buf mmap_env;
static sig_atomic_t mmap_access;

/**
 * Handles SIGBUS or SIGSEGV when accessing mmap()ed areas.
 */
static void
signal_handler(int n)
{
	if (mmap_access) {
		mmap_access = 0;
		siglongjmp(mmap_env, n);
	}
	set_signal(n, SIG_DFL);
	raise(n);
}
#endif /* !USE_MMAP */

/**
 * Write at most `len' bytes to source's fd, as bandwidth permits.
 *
 * Bytes are read from `offset' in the in_fd file descriptor, and the value
 * is updated in place by the kernel.
 *
 * @return -1 with errno set to EAGAIN, if we cannot write anything due to
 * bandwidth constraints.
 */
ssize_t
bio_sendfile(sendfile_ctx_t *ctx, bio_source_t *bio, gint in_fd, off_t *offset,
	size_t len)
{
#if !defined(USE_MMAP) && !defined(HAS_SENDFILE)

	(void) ctx;
	(void) bio;
	(void) in_fd;
	(void) offset;
	(void) len;

	g_assert_not_reached();
	/* NOTREACHED */

	errno = ENOSYS;

	return (ssize_t) -1;

#else /* USE_MMAP || HAS_SENDFILE */

	/*
	 * amount is only declared volatile to shut up the bogus warning by GCC
	 * 2.95.x that ``amount'' *might* be clobbered by sigsetjmp - which is
	 * actually not the case.
	 */
	volatile size_t amount;
	size_t available;
	ssize_t r;
	gint out_fd;
	off_t start;

	g_assert(ctx);
	g_assert(bio);
	g_assert(bio->wio);
	g_assert(bio->flags & BIO_F_WRITE);
	g_assert(offset);
	g_assert(len > 0);

	start = *offset;
	out_fd = bio->wio->fd(bio->wio);

	/*
	 * If we don't have any bandwidth, return -1 with errno set to EAGAIN
	 * to signal that we cannot perform any I/O right now.
	 */

	available = bw_available(bio, len);

	if (available == 0) {
		errno = EAGAIN;
		return -1;
	}

	amount = len > available ? available : len;

	if (dbg > 7)
		printf("bsched_write(fd=%d, len=%d) available=%d\n",
			bio->wio->fd(bio->wio), (gint) len, (gint) available);

#ifdef USE_MMAP
	{
		static gboolean first_call = TRUE;
		const gchar *data;
		gint n;

		if (first_call) {
			first_call = FALSE;
			set_signal(SIGBUS, signal_handler);
			set_signal(SIGSEGV, signal_handler);
		}

		if (
			ctx->map == NULL ||
			start < ctx->map_start ||
			start + amount > ctx->map_end
		) {
			size_t map_len;

			if (ctx->map != NULL) {
				munmap(ctx->map, ctx->map_end - ctx->map_start);
				ctx->map = NULL;
			}
			map_len = MAX(amount, 256 * 1024U);
			ctx->map_start = start;
			ctx->map_end = ctx->map_start + map_len;
			ctx->map = mmap(NULL, map_len, PROT_READ, MAP_PRIVATE, in_fd,
							ctx->map_start);
			if (MAP_FAILED == ctx->map) {
				ctx->map = NULL;
				ctx->map_start = 0;
				ctx->map_end = 0;
				return (ssize_t) -1;
			}
#ifdef MADV_SEQUENTIAL
			/* TODO:	Add _proper_ Configure check for madvise.
			 *			No mere symbol check, please.
			 */
			madvise(ctx->map, map_len, MADV_SEQUENTIAL);
#endif /* MADV_SEQUENTIAL */
		}

		g_assert(ctx->map != NULL);

		if (0 != (n = sigsetjmp(mmap_env, 1))) {
			switch (n) {
			case SIGBUS:
				g_warning("bio_sendfile(): Caught SIGBUS");
				break;
			case SIGSEGV:
				g_warning("bio_sendfile(): Caught SIGSEGV");
				break;
			default:
				g_assert_not_reached();
			}
			errno = EPIPE;
			return -1;
		}

		data = ctx->map;
		g_assert(mmap_access == 0);
		mmap_access = 1;
		r = write(out_fd, &data[start - ctx->map_start], amount);
		mmap_access = 0;

		switch (r) {
		case (ssize_t) -1:
			break;
		case 0:
			munmap(ctx->map, ctx->map_end - ctx->map_start);
			ctx->map = NULL;
			break;
		default:
			*offset = start + r;
		}
	}
#else /* !USE_MMAP */
#ifdef USE_BSD_SENDFILE
	/*
	 * The BSD semantics for sendfile() differ from the Linux one:
	 *
	 * . BSD sendfile() returns 0 on succes, -1 on failure.
	 * . BSD sendfile() returns the amount of written bytes via a parameter
	 *   when EAGAIN.
	 * . BSD sendfile() does not update the offset inplace.
	 *
	 * Emulate the Linux semantics: set `r' to the amount of bytes written,
	 * and update the `offset' variable.
	 */

	{
		off_t written;

		r = sendfile(in_fd, out_fd, start, amount, NULL, &written, 0);
		if ((ssize_t) -1 == r) {
			if (errno == EAGAIN || errno == EINTR)
				r = written > 0 ? (ssize_t) written : (ssize_t) -1;
		} else {
			r = amount;			/* Everything written, but returns 0 if OK */
		}
		if (r > 0)
			*offset = start + r;
	}

#else	/* !USE_BSD_SENDFILE */

	r = sendfile(out_fd, in_fd, offset, amount);

	if (r >= 0 && *offset != start + r) {		/* Paranoid, as usual */
		g_warning("FIXED SENDFILE returned offset: "
			"was set to %s instead of %s (%d byte%s written)",
			uint64_to_string(*offset), uint64_to_string2(start + r),
			(gint) r, r == 1 ? "" : "s");
		*offset = start + r;
	} else if ((ssize_t) -1 == r) {
		*offset = start;	/* Paranoid: in case sendfile() touched it */
	}

#endif	/* USE_BSD_SENDFILE */
#endif	/* USE_MMAP */

	if (r > 0) {
		bsched_bw_update(bio->bs, r, amount);
		bio->bw_actual += r;
	}

	return r;
#endif /* !USE_MMAP && !HAS_SENDFILE */
}

/**
 * Read at most `len' bytes from `buf' from source's fd, as bandwidth
 * permits.
 *
 * @return -1 with errno set to EAGAIN, if we cannot read anything due to
 * bandwidth constraints.
 */
ssize_t
bio_read(bio_source_t *bio, gpointer data, size_t len)
{
	size_t available;
	size_t amount;
	ssize_t r;

	g_assert(bio);
	g_assert(bio->flags & BIO_F_READ);

	/*
	 * If we don't have any bandwidth, return -1 with errno set to EAGAIN
	 * to signal that we cannot perform any I/O right now.
	 */

	available = bw_available(bio, len);
	if (available == 0) {
		errno = EAGAIN;
		return -1;
	}

	amount = len > available ? available : len;
	if (dbg > 7)
		printf("bsched_read(fd=%d, len=%d) available=%d\n",
			bio->wio->fd(bio->wio), (gint) len, (gint) available);

	r = bio->wio->read(bio->wio, data, amount);
	if (r > 0) {
		bsched_bw_update(bio->bs, r, amount);
		bio->bw_actual += r;
		bio->bs->flags |= BS_F_DATA_READ;
	}

	return r;
}

/**
 * Write at most `len' bytes from `buf' to specified fd, and account the
 * bandwidth used.  Any overused bandwidth will be tracked, so that on
 * average, we stick to the requested bandwidth rate.
 *
 * @return The amount of bytes written or (-1) if an error occurred.
 */
ssize_t
bws_write(bsched_t *bs, wrap_io_t *wio, gconstpointer data, size_t len)
{
	ssize_t r;

	g_assert(bs);
	g_assert(bs->flags & BS_F_WRITE);
	g_assert(wio);
	g_assert(wio->write);
	g_assert(len <= INT_MAX);

	r = wio->write(wio, data, len);
	if (r > 0)
		bsched_bw_update(bs, r, len);

	return r;
}

/**
 * Read at most `len' bytes from `buf' from specified fd, and account the
 * bandwidth used.  Any overused bandwidth will be tracked, so that on
 * average, we stick to the requested bandwidth rate.
 */
ssize_t
bws_read(bsched_t *bs, wrap_io_t *wio, gpointer data, size_t len)
{
	ssize_t r;

	g_assert(bs);
	g_assert(bs->flags & BS_F_READ);
	g_assert(wio);
	g_assert(wio->read);
	g_assert(len <= INT_MAX);

	r = wio->read(wio, data, len);
	if (r > 0) {
		bsched_bw_update(bs, r, len);
		bs->flags |= BS_F_DATA_READ;
	}

	return r;
}

/**
 * Account for read data from UDP.
 */
void
bws_udp_count_read(gint len)
{
	gint count = BW_UDP_MSG + len;

	bsched_bw_update(bws.gin_udp, count, count);
	bws.gin_udp->flags |= BS_F_DATA_READ;
}

/**
 * Account for written data to UDP.
 */
void
bws_udp_count_written(gint len)
{
	gint count = BW_UDP_MSG + len;

	bsched_bw_update(bws.gout_udp, count, count);
}

/**
 * Returns adequate b/w shaper depending on the socket type.
 *
 * @returns NULL if there is no b/w shaper to consider.
 */
static bsched_t *
bs_socket(enum socket_direction dir, enum socket_type type)
{
	switch (type) {
	case SOCK_TYPE_DOWNLOAD:
	case SOCK_TYPE_HTTP:
	case SOCK_TYPE_UPLOAD:
	case SOCK_TYPE_PPROXY:
		return (dir == SOCK_CONN_OUTGOING) ? bws.out : bws.in;
	case SOCK_TYPE_CONTROL:
	case SOCK_TYPE_CONNBACK:
		return (dir == SOCK_CONN_OUTGOING) ? bws.gout : bws.gin;
	case SOCK_TYPE_SHELL:
		return NULL;
	case SOCK_TYPE_UDP:
		return (dir == SOCK_CONN_OUTGOING) ? bws.gout_udp : bws.gin_udp;
	default:
		g_warning("bs_socket: unhandled socket type %d", type);
		return (dir == SOCK_CONN_OUTGOING) ? bws.out : bws.in;
	}
}

/**
 * Record that we're issuing a TCP/IP connection of a particular type.
 */
void
bws_sock_connect(enum socket_type type)
{
	bsched_t *bs = bs_socket(SOCK_CONN_OUTGOING, type);

	/*
	 * At worst, a TCP/IP connect sequence can send 3 SYN packets,
	 * of 40 bytes each.  But the connection can be denied immediately
	 * with a RST after the first SYN.
	 *
	 * What we do here is account for 60 bytes only (half the maximum).
	 * If the connection attempt times out, we'll account for 40 more
	 * bytes, as a conservative measure.
	 */

	if (bs != NULL)
		bsched_bw_update(bs, (3 * BW_TCP_MSG) / 2, (3 * BW_TCP_MSG) / 2);
}

/**
 * Record that the connection attempt failed.
 */
void
bws_sock_connect_failed(enum socket_type type)
{
	bsched_t *bs = bs_socket(SOCK_CONN_INCOMING, type);

	/*
	 * We got an RST message.
	 */

	if (bs != NULL)
		bsched_bw_update(bs, BW_TCP_MSG, BW_TCP_MSG);
}

/**
 * A connection attempt of `type' timed out.
 */
void
bws_sock_connect_timeout(enum socket_type type)
{
	bsched_t *bs = bs_socket(SOCK_CONN_OUTGOING, type);

	/*
	 * Assume we sent an extra SYN.
	 */

	if (bs != NULL)
		bsched_bw_update(bs, BW_TCP_MSG, BW_TCP_MSG);
}

/**
 * Record that the connection attempt succeeded.
 */
void
bws_sock_connected(enum socket_type type)
{
	bsched_t *bs = bs_socket(SOCK_CONN_INCOMING, type);

	/*
	 * We got an ACK message.
	 */

	if (bs != NULL)
		bsched_bw_update(bs, BW_TCP_MSG, BW_TCP_MSG);
}

/**
 * We accepted an incoming connection of `type'.
 */
void
bws_sock_accepted(enum socket_type type)
{
	bsched_t *bsout = bs_socket(SOCK_CONN_OUTGOING, type);
	bsched_t *bsin = bs_socket(SOCK_CONN_INCOMING, type);

	/*
	 * We received a SYN message.
	 * We sent back an ACK+SYN message to acknowledge the connection.
	 */

	if (bsout != NULL)
		bsched_bw_update(bsout, BW_TCP_MSG, BW_TCP_MSG);
	if (bsin != NULL)
		bsched_bw_update(bsin, BW_TCP_MSG, BW_TCP_MSG);
}

/**
 * The connection was closed, remotely if `remote' is true.
 */
void
bws_sock_closed(enum socket_type type, gboolean remote)
{
	bsched_t *bsout = bs_socket(SOCK_CONN_OUTGOING, type);
	bsched_t *bsin = bs_socket(SOCK_CONN_INCOMING, type);

	/*
	 * Assume we sent a FIN and an ACK if we closed locally,
	 * or that we got a single FIN if it was closed remotely.
	 */

	if (remote) {
		if (bsout != NULL)
			bsched_bw_update(bsout, BW_TCP_MSG, BW_TCP_MSG); /* Sent an ACK */
		if (bsin != NULL)
			bsched_bw_update(bsin, BW_TCP_MSG, BW_TCP_MSG);	 /* Got a FIN */
	} else {
		/* Sent a FIN and an ACK */
		if (bsout != NULL)
			bsched_bw_update(bsout, 2 * BW_TCP_MSG, 2 * BW_TCP_MSG);
		if (bsin != NULL)
			bsched_bw_update(bsin, BW_TCP_MSG, BW_TCP_MSG);	 /* Got an ACK */
	}
}

/**
 * Do we have the bandwidth to issue a new TCP/IP connection of `type'?
 */
gboolean
bws_can_connect(enum socket_type type)
{
	bsched_t *bsout = bs_socket(SOCK_CONN_OUTGOING, type);
	bsched_t *bsin = bs_socket(SOCK_CONN_INCOMING, type);
	gint available;

	if (bsout != NULL && (bsout->flags & BS_F_ENABLED)) {
		if (bsout->flags & BS_F_NOBW)				/* No more bandwidth */
			return FALSE;

		/*
		 * We need 1.5 TCP messages at least to allow the connection.
		 */

		available = bsout->bw_max + bsout->bw_stolen - bsout->bw_actual;

		if (available < 1.5 * BW_TCP_MSG)
			return FALSE;
	}

	if (bsin != NULL && (bsin->flags & BS_F_ENABLED)) {
		if (bsin->flags & BS_F_NOBW)				/* No more bandwidth */
			return FALSE;

		/*
		 * We need 1 TCP message at least to allow the connection.
		 */

		available = bsin->bw_max + bsin->bw_stolen - bsin->bw_actual;

		if (available < BW_TCP_MSG)
			return FALSE;
	}

	return TRUE;
}

/**
 * Periodic heartbeat.
 */
static void
bsched_heartbeat(bsched_t *bs, tm_t *tv)
{
	GList *l;
	gint delay;
	gint overused;
	gint theoric;
	gint correction;
	gint last_bw_max;
	gint last_capped;
	gint last_used;

	/*
	 * How much time elapsed since last call?
	 */

	delay = (gint) ((tv->tv_sec - bs->last_period.tv_sec) * 1000 +
		(tv->tv_usec - bs->last_period.tv_usec) / 1000);

	if (dbg > 9)
		printf("[%s] tv = %d,%d  bs = %d,%d, delay = %d\n",
			bs->name, (gint) tv->tv_sec, (gint) tv->tv_usec,
			(gint) bs->last_period.tv_sec, (gint) bs->last_period.tv_usec,
			delay);

	/*
	 * It is possible to get a negative delay (i.e. have the current time
	 * be less than the previous time) when the machine runs a time
	 * synchronization daemon.
	 *
	 * In general, this is deemed to happen when the actual delay is less
	 * than min_period (75% of nominal).  We then force the fixed scheduling
	 * period delay and proceed as usual.
	 *
	 * Likewise, it is possible for the time to go forward.  This is a little
	 * more difficult to detect, because we can be delayed due to a high CPU
	 * load.  That's why the max_period is at 150% of the nominal period, and
	 * not at 125%.
	 */

	if (delay < bs->min_period) {
		if (dbg && bs->last_period.tv_sec)
			g_warning("heartbeat (%s) noticed time jumped backwards (~%d ms)",
				bs->name, bs->period - delay);
		delay = bs->period;
	} else if (delay > bs->max_period) {
		if (dbg && bs->last_period.tv_sec)
			g_warning("heartbeat (%s) noticed time jumped forwards (~%d ms)",
				bs->name, delay - bs->period);
		delay = bs->period;
	}

	bs->last_period = *tv;		/* struct copy */

	g_assert(delay > 0);

	/*
	 * Exponential Moving Average (EMA) with smoothing factor sm=1/4.
	 * Since usually one uses sm=2/(n+1), it's a moving average with n=7.
	 *
	 *      EMA(n+1) = sm*x(n) + (1-sm)*EMA(n)
	 */

	bs->period_ema += (delay >> 2) - (bs->period_ema >> 2);
	bs->bw_ema += (bs->bw_actual >> 2) - (bs->bw_ema >> 2);
	bs->bw_stolen_ema += (bs->bw_stolen >> 2) - (bs->bw_stolen_ema >> 2);

	g_assert(bs->period_ema >= 0);
	g_assert(bs->bw_ema >= 0);
	g_assert(bs->bw_stolen_ema >= 0);

	/*
	 * If scheduler is disabled, we don't need to recompute bandwidth.
	 *
	 * Jump to the end, where the new timeslice begins, so that per-source
	 * bandwidth transfer rates are updated nonetheless.  Use "goto" to avoid
	 * indenting the whole routine.
	 */

	if (!(bs->flags & BS_F_ENABLED))
		goto new_timeslice;

	/*
	 * Recompute bandwidth for the next period.
	 */

	last_bw_max = bs->bw_max;
	last_capped = bs->bw_capped;

	theoric = (gint) (bs->bw_per_second / 1000.0 * delay);
	overused = bs->bw_actual - theoric;
	bs->bw_delta += overused;

	overused -= bs->bw_stolen;		/* Correct for computations below */

	bs->bw_max = (gint) (bs->bw_per_second / 1000.0 * bs->period_ema);

	/*
	 * We correct the bandwidth for the next slot.
	 *
	 * However, we don't use the current overuse indication in that case,
	 * but the maximum between the EMA of the bandwidth overused and the
	 * current overuse, to absorb random burst effects and yet account
	 * for constant average overuse.
	 *
	 * If a correction is due but the bandwidth settings changed in the
	 * period, forget it: allow a full period at the nominal new settings.
	 */

	/* Forllowing is the overused "EMA" */
	correction = bs->bw_ema - bs->bw_stolen_ema - theoric;
	correction = MAX(correction, overused);

	if (correction > 0 && !(bs->flags & BS_F_CHANGED_BW)) {
		bs->bw_max -= correction;
		if (bs->bw_max < 0)
			bs->bw_max = 0;
	}

	/*
	 * Disregard amount of capped bandwidth if we used all our
	 * configured maximum, so that it is used more evenly during next slice.
	 * This information is also only perused for writing sources.
	 */

	if (bs->bw_actual >= last_bw_max || !(bs->flags & BS_F_WRITE))
		bs->bw_capped = 0;

	/*
	 * Any unwritten data must be removed from the amount of capped bandwidth.
	 * If we start to be flow-controlled by the kernel, we have to be careful
	 * not to write too much anyway.
	 */

	bs->bw_capped -= bs->bw_unwritten;
	bs->bw_capped = MAX(0, bs->bw_capped);

	/*
	 * Compute the amount of sources used this period.
	 *
	 * This information is used to initially compute the bandwidth per slot.
	 * Indeed, when only a few sources are active, we need to distribute more
	 * bandwidth per slot that triggers in case we don't have the opportunity
	 * to loop through all the sources more than once before the end of
	 * the slot.
	 */

	last_used = 0;

	for (l = bs->sources; l; l = g_list_next(l)) {
		bio_source_t *bio = (bio_source_t *) l->data;

		g_assert(bio != NULL);

		if (bio->flags & BIO_F_USED)
			last_used++;
	}

	g_assert(last_used <= bs->current_used);	/* May have removed a source */

	bs->last_used = last_used;

	if (dbg > 4) {
		printf("bsched_timer(%s): delay=%d (EMA=%d), b/w=%d (EMA=%d), "
			"overused=%d (EMA=%d) stolen=%d (EMA=%d) unwritten=%d "
			"capped=%d (%d) used %d/%d\n",
			bs->name, delay, bs->period_ema, bs->bw_actual, bs->bw_ema,
			overused, bs->bw_ema - bs->bw_stolen_ema - theoric,
			bs->bw_stolen, bs->bw_stolen_ema, bs->bw_unwritten,
			last_capped, bs->bw_capped, bs->last_used, bs->count);
		printf("    -> b/w delta=%d, max=%d, slot=%d, first=%d "
			"(target %d B/s, %d slot%s, real %.02f B/s)\n",
			bs->bw_delta, bs->bw_max,
			bs->count ? bs->bw_max / bs->count : 0,
			bs->count ? (bs->bw_max + bs->bw_capped) / bs->count : 0,
			bs->bw_per_second, bs->count,
			bs->count == 1 ? "" : "s", bs->bw_actual * 1000.0 / delay);
	}

	/*
	 * Reset running counters.
	 */

new_timeslice:

	bs->bw_last_period = bs->bw_actual;
	bs->bw_last_capped = bs->bw_capped;
	bs->bw_actual = bs->bw_stolen = 0;
}

/**
 * Periodic stealing beat, occurs after the heartbeat.
 */
static void
bsched_stealbeat(bsched_t *bs)
{
	GSList *l;
	GSList *all_used = NULL;		/* List of bsched_t that used all b/w */
	gint all_used_count = 0;		/* Amount of bsched_t that used all b/w */
	guint all_bw_count = 0;			/* Sum of configured bandwidth */
	gint steal_count = 0;
	gint underused;

	g_assert(bs->bw_actual == 0);	/* Heartbeat step must have been done */

	if (bs->stealers == NULL)		/* No stealers */
		return;

	if (!(bs->flags & BS_F_ENABLED))	/* Scheduler disabled */
		return;

	/**
	 * Note that we do not use the theoric bandwidth, but bs->bw_max to
	 * estimate the amount of underused bandwidth.  The reason is that
	 * bs->bw_max can be corrected due to traffic spikes.
	 */

	underused = bs->bw_max - bs->bw_last_period;

	/*
	 * If `bs' holds reading sources, there is no further correction needed.
	 *
	 * Howewever, for writing sources, we need to pay attention to possible
	 * outgoing flow-control exercised by the kernel.  We simply correct
	 * the amount of underused bandwidth by the amount of unwritten data.
	 */

	underused -= bs->bw_unwritten;

	/* XXX: Remove that for now -- we don't know if the untriggered sources
	 *		had anything to write or not. -- RAM, 11/05/2003 */

#if 0
	/*
	 * That's not enough for writing schedulers: some sources have no
	 * triggering callback (i.e. we write to them when we have more data),
	 * but others have triggering callbacks invoked only when there is room
	 * for more data.
	 *
	 * If there are such sources that have callbacks and did not trigger,
	 * it means there is already some flow control going on.  Maybe the
	 * remote end is not reading, or we have problem sending.  It's hard to
	 * tell.  In any case, remove half the contribution of each untriggered
	 * source.
	 */

	if (bs->flags & BS_F_WRITE) {
		gint half_contribution = bs->count ? bs->bw_max / (2 * bs->count) : 0;
		GList *bl;

		for (bl = bs->sources; bl && underused > 0; bl = g_list_next(bl)) {
			bio_source_t *bio = (bio_source_t *) bl->data;

			if (bio->io_callback != NULL && !(bio->flags & BIO_F_USED))
				underused -= half_contribution;
		}
	}
#endif

	if (underused <= 0)				/* Nothing to redistribute */
		return;

	/*
	 * Determine who used up all its bandwidth among our stealers.
	 */

	for (l = bs->stealers; l; l = g_slist_next(l)) {
		bsched_t *xbs = (bsched_t *) l->data;

		steal_count++;

		if (xbs->bw_last_period >= xbs->bw_max) {
			all_used = g_slist_prepend(all_used, xbs);
			all_used_count++;
			all_bw_count += xbs->bw_max;
		}
	}

	g_assert(steal_count > 0);

	/*
	 * Distribute our available bandwidth proportionally to all the
	 * schedulers that saturated their bandwidth, or evenly to all the
	 * stealers if noone saturated.
	 */

	if (all_used_count == 0) {
		for (l = bs->stealers; l; l = g_slist_next(l)) {
			bsched_t *xbs = (bsched_t *) l->data;
			xbs->bw_stolen += underused / steal_count;

			if (dbg > 4)
				printf("b/w sched \"%s\" evenly giving %d bytes to \"%s\"\n",
					bs->name, underused / steal_count, xbs->name);
		}
	} else {
		for (l = all_used; l; l = g_slist_next(l)) {
			bsched_t *xbs = (bsched_t *) l->data;
			gdouble amount;

			if (xbs->bw_max == 0)
				continue;

			amount = (gdouble) underused * (gdouble) xbs->bw_max / all_bw_count;

			if ((gdouble) xbs->bw_stolen + amount > (gdouble) BS_BW_MAX)
				xbs->bw_stolen = BS_BW_MAX;
			else
				xbs->bw_stolen += (gint) amount;

			if (dbg > 4)
				printf("b/w sched \"%s\" giving %d bytes to \"%s\"\n",
					bs->name, (gint) amount, xbs->name);
		}
		g_slist_free(all_used);
	}
}

/**
 * Periodic timer.
 */
void
bsched_timer(void)
{
	tm_t tv;
	GSList *l;
	gint out_used = 0;
	gint in_used = 0;
	gboolean read_data = FALSE;

	tm_now(&tv);

	/*
	 * First pass: compute bandwidth used.
	 */

	for (l = bws_list; l; l = g_slist_next(l))
		bsched_heartbeat(l->data, &tv);

	/*
	 * Second pass: possibly steal bandwidth from schedulers that
	 * have not used up all their quota.
	 */

	for (l = bws_list; l; l = g_slist_next(l))
		bsched_stealbeat(l->data);

	/*
	 * Third pass: begin new timeslice.
	 */

	for (l = bws_list; l; l = g_slist_next(l))
		bsched_begin_timeslice(l->data);

	/*
	 * Fourth pass: update the average bandwidth used.
	 */

	for (l = bws_out_list; l; l = g_slist_next(l)) {
		bsched_t *bs = (bsched_t *) l->data;
		out_used += (gint) (bs->bw_last_period * 1000.0 / bs->period_ema);
	}

	bws_out_ema += (out_used >> 6) - (bws_out_ema >> 6);	/* Slow EMA */

	if (dbg > 4)
		printf("Outgoing b/w EMA = %d bytes/s\n", bws_out_ema);

	for (l = bws_in_list; l; l = g_slist_next(l)) {
		bsched_t *bs = (bsched_t *) l->data;

		in_used += (gint) (bs->bw_last_period * 1000.0 / bs->period_ema);

		if (bs->flags & BS_F_DATA_READ) {
			read_data = TRUE;
			bs->flags &= ~BS_F_DATA_READ;
		}
	}

	bws_in_ema += (in_used >> 6) - (bws_in_ema >> 6);		/* Slow EMA */

	if (dbg > 4)
		printf("Incoming b/w EMA = %d bytes/s\n", bws_in_ema);

	/*
	 * Don't simply rely on in_used > 0 since we fake input data when
	 * sockets are closed or timeout on connection.  We really want to know
	 * whether there has been actual data read.
	 *		--RAM, 2005-06-29
	 */

	if (read_data)
		inet_read_activity();
}

/**
 * Needs very short description so that doxygen can parse the following
 * list properly.
 *
 * Determine whether we have enough bandwidth to possibly become an
 * ultra node:
 *
 *  -# There must be more than BW_OUT_UP_MIN outgoing bandwidth available.
 *  -# If bandwidth schedulers are enabled, leaf nodes must not be configured
 *     to steal all the HTTP outgoing bandwidth, unless they disabled uploads.
 *  -# If Gnet out scheduler is enabled, there must be at least BW_OUT_GNET_MIN
 *     bytes per gnet connection.
 *  -# Overall, there must be BW_OUT_LEAF_MIN bytes per configured leaf plus
 *     BW_OUT_GNET_MIN bytes per gnet connection available.
 */
gboolean
bsched_enough_up_bandwidth(void)
{
	guint32 total = 0;

	if (ul_running && bws_out_ema < BW_OUT_UP_MIN)
		return FALSE;		/* 1. */

	if (
		bws_glout_enabled && bws_out_enabled &&
		bw_gnet_lout >= bw_http_out && upload_is_enabled()
	)
		return FALSE;		/* 2. */

	if (
		bws_gout_enabled &&
		bw_gnet_out < BW_OUT_GNET_MIN * (up_connections + max_connections) / 2
	)
		return FALSE;		/* 3. */

	if (bws_gout_enabled)
		total += bw_gnet_out;

	if (bws_out_enabled)
		total += bw_http_out;		/* Leaf b/w stolen from HTTP traffic */
	else if (bws_glout_enabled)
		total += bw_gnet_lout;

	if (
		total <
			(BW_OUT_GNET_MIN * max_connections + BW_OUT_LEAF_MIN * max_leaves)
	)
		return FALSE;		/* 4. */

	return TRUE;
}

/* vi: set ts=4 sw=4 cindent: */
