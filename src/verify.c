/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Raphael Manfredi
 *
 * Hash verification.
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

#include "gnutella.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

#include "downloads.h"
#include "verify.h"
#include "file.h"
#include "override.h"		/* Must be the last header included */

RCSID("$Id$");

#define HASH_BLOCK_SHIFT	12			/* Power of two of hash unit credit */
#define HASH_BUF_SIZE		65536		/* Size of the reading buffer */

static gpointer verify_daemon = NULL;

#define VERIFYD_MAGIC	0x000e31f8

/*
 * Verification daemon context.
 */
struct verifyd {
	gint magic;				/* Magic number */
	struct download *d;		/* Current download */
	gint fd;				/* Opened file descriptor, -1 if none */
	time_t start;			/* Start time, to determine computation rate */
	off_t size;				/* Size of file */
	off_t hashed;			/* Amount of data hashed so far */
	SHA1Context context;	/* SHA1 computation context */
	gchar *buffer;			/* Large buffer, where data is read */
	gint error;				/* Error code */
};

/*
 * d_free
 *
 * Freeing of computation context.
 */
static void d_free(gpointer ctx)
{
	struct verifyd *vd = (struct verifyd *) ctx;

	g_assert(vd->magic == VERIFYD_MAGIC);

	if (vd->fd != -1)
		close(vd->fd);

	g_free(vd->buffer);
	wfree(vd, sizeof(*vd));
}

/*
 * d_notify
 *
 * Daemon's notification of start/stop.
 */
static void d_notify(gpointer h, gboolean on)
{
	gnet_prop_set_boolean_val(PROP_SHA1_VERIFYING, on);
}

/*
 * d_start
 *
 * Daemon's notification: starting to work on item.
 */
static void d_start(gpointer h, gpointer ctx, gpointer item)
{
	struct verifyd *vd = (struct verifyd *) ctx;
	struct download *d = (struct download *) item;
	gchar *filename;

	g_assert(vd->magic == VERIFYD_MAGIC);
	g_assert(vd->fd == -1);
	g_assert(vd->d == NULL);

	download_verify_start(d);

	filename = g_strdup_printf("%s/%s", download_path(d), download_outname(d));
	g_return_if_fail(NULL != filename);

	vd->fd = file_open(filename, O_RDONLY);

	if (vd->fd == -1) {
		vd->error = errno;
		g_warning("can't open %s to verify SHA1: %s",
			filename, g_strerror(errno));
		G_FREE_NULL(filename);
		return;
	}

	vd->d = d;
	vd->start = time(NULL);
	vd->size = (off_t) download_filesize(d);
	vd->hashed = 0;
	vd->error = 0;
	SHA1Reset(&vd->context);

	if (dbg > 1)
		printf("Verifying SHA1 digest for %s\n", filename);
	G_FREE_NULL(filename);
}

/*
 * d_end
 *
 * Daemon's notification: finished working on item.
 */
static void d_end(gpointer h, gpointer ctx, gpointer item)
{
	struct verifyd *vd = (struct verifyd *) ctx;
	struct download *d = (struct download *) item;
	time_t elapsed = 0;
	guint8 digest[SHA1HashSize];

	g_assert(vd->magic == VERIFYD_MAGIC);

	if (vd->fd == -1) {				/* Did not start properly */
		g_assert(vd->error);
		goto finish;
	}

	g_assert(vd->d == d);

	close(vd->fd);
	vd->fd = -1;
	vd->d = NULL;

	if (vd->error == 0) {
		g_assert(vd->hashed == vd->size);
		SHA1Result(&vd->context, digest);
	}

	elapsed = time(NULL) - vd->start;
	elapsed = MAX(1, elapsed);

	if (dbg > 1)
		printf("Computed SHA1 digest for %s at %lu bytes/sec [error=%d]\n",
			download_outname(d), (gulong) vd->size / elapsed, vd->error);

finish:
	if (vd->error == 0)
		download_verify_done(d, (gchar *) digest, elapsed);
	else
		download_verify_error(d);
}

/*
 * d_step_compute
 *
 * Compute SHA1 of current file.
 */
static bgret_t d_step_compute(gpointer h, gpointer u, gint ticks)
{
	struct verifyd *vd = (struct verifyd *) u;
	gint r;
	gint amount;
	gint res;
	guint32 remain;
	gint used;

	g_assert(vd->magic == VERIFYD_MAGIC);

	if (vd->fd == -1)			/* Could not open the file */
		return BGR_DONE;		/* Computation done */

	if (vd->size == 0)			/* Empty file */
		return BGR_DONE;

	remain = vd->size - vd->hashed;

	g_assert(remain > 0);

	/*
	 * Each tick we have can buy us 2^HASH_BLOCK_SHIFT bytes.
	 *
	 * We read into a HASH_BUF_SIZE bytes buffer, and at most vd->size
	 * bytes total, to stop before the fileinfo trailer.
	 */

	amount = ticks << HASH_BLOCK_SHIFT;
	remain = MIN(remain, HASH_BUF_SIZE);
	amount = MIN(amount, remain);

	g_assert(amount > 0);

	r = read(vd->fd, vd->buffer, amount);

	if (r < 0) {
		vd->error = errno;
		g_warning("error while reading %s for computing SHA1: %s",
			download_outname(vd->d), g_strerror(errno));
		return BGR_DONE;
	}

	if (r == 0) {
		g_warning("EOF while reading %s for computing SHA1!",
			download_outname(vd->d));
		vd->error = -1;
		return BGR_DONE;
	}

	/*
	 * Any partially read block counts as one block, hence the second term.
	 */

	used = (r >> HASH_BLOCK_SHIFT) +
		((r & ((1 << HASH_BLOCK_SHIFT) - 1)) ? 1 : 0);

	if (used != ticks)
		bg_task_ticks_used(h, used);

	res = SHA1Input(&vd->context, (guint8 *) vd->buffer, r);
	if (res != shaSuccess) {
		g_warning("SHA1 computation error for %s", download_outname(vd->d));
		vd->error = -1;
		return BGR_DONE;
	}

	vd->hashed += r;
	download_verify_progress(vd->d, vd->hashed);

	return vd->hashed == vd->size ? BGR_DONE : BGR_MORE;
}

/*
 * verify_queue
 *
 * Enqueue completed download file for verification.
 */
void verify_queue(struct download *d)
{
	bg_daemon_enqueue(verify_daemon, d);
}

/*
 * verify_init
 *
 * Initializes the background verification task.
 */
void verify_init(void)
{
	struct verifyd *vd;
	bgstep_cb_t step = d_step_compute;

	vd = walloc(sizeof(*vd));
	vd->magic = VERIFYD_MAGIC;
	vd->fd = -1;
	vd->buffer = g_malloc(HASH_BUF_SIZE);
	vd->d = NULL;

	verify_daemon = bg_daemon_create("SHA1 verification",
		&step, 1,
		vd, d_free,
		d_start, d_end, NULL,
		d_notify);
}

/*
 * verify_close
 *
 * Called at shutdown time.
 */
void verify_close(void)
{
	bg_task_cancel(verify_daemon);
}

