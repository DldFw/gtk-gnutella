/*
 * Copyright (c) 2003, Jeroen Asselman
 *
 * Passive/Active Remote Queuing.
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
 
#ifndef _parq_h_
#define _parq_h_

#include "header.h"
#include "downloads.h"
#include "uploads.h"

#define PARQ_MAX_UL_RETRY_DELAY 1200	/* 20 minutes retry rate max. */
#define PARQ_GRACE_TIME			90		/* Grace period after life expired */

/*
 * Public interface.
 */

void parq_init(void);
void parq_close(void);

gint get_parq_dl_position(const struct download *d);
gint get_parq_dl_queue_length(const struct download *d);
gint get_parq_dl_eta(const struct download *d);
gint get_parq_dl_retry_delay(const struct download *d);
gchar *get_parq_dl_id(const struct download *d);
void parq_dl_reparent_id(struct download *d, struct download *cd);

gpointer parq_dl_create(struct download *d);
void parq_dl_add_id(struct download *d, const gchar *new_id);
void parq_dl_remove(struct download *d);
void parq_dl_free(struct download *d);
	
void parq_download_retry_active_queued(struct download *d);
gboolean parq_download_supports_parq(header_t *header);
gboolean parq_download_parse_queue_status(struct download *d, header_t *header);
gboolean parq_download_is_active_queued(struct download *d);
void parq_download_add_header(
		gchar *buf, gint len, gint *rw, struct download *d);
gboolean parq_download_is_passive_queued(struct download *d);
void parq_download_queue_ack(struct gnutella_socket *s);
	
void parq_upload_timer(time_t now);
void parq_upload_add_header(
	gchar *buf, gint *retval, gpointer arg, guint32 flags);
void parq_upload_add_header_id(
	gchar *buf, gint *retval, gpointer arg, guint32 flags);
gpointer parq_upload_get(
	gnutella_upload_t *u, header_t *header, gboolean replacing);
gboolean parq_upload_request(gnutella_upload_t *u, gpointer handle, 
		guint used_slots);
gboolean parq_upload_request_force(gnutella_upload_t *u, gpointer handle, 
	  guint used_slots);
guint parq_upload_lookup_position(gnutella_upload_t *u);
gchar* parq_upload_lookup_id(gnutella_upload_t *u);
gboolean parq_upload_queue_full(gnutella_upload_t *u);
guint parq_upload_lookup_size(gnutella_upload_t *u);
gboolean parq_upload_ip_can_proceed(gnutella_upload_t *u);

time_t parq_upload_lookup_lifetime(gnutella_upload_t *u);
time_t parq_upload_lookup_retry(gnutella_upload_t *u);
guint parq_upload_lookup_eta(gnutella_upload_t *u);
guint parq_upload_lookup_queue_no(gnutella_upload_t *u);

gboolean parq_upload_queued(gnutella_upload_t *u);
gboolean parq_upload_remove(gnutella_upload_t *u);
void parq_upload_upload_got_freed(gnutella_upload_t *u);
void parq_upload_upload_got_cloned(gnutella_upload_t *u, gnutella_upload_t *cu);
void parq_upload_force_remove(gnutella_upload_t *u);
void parq_upload_add(gnutella_upload_t *u);
void parq_upload_busy(gnutella_upload_t *u, gpointer handle);
void parq_upload_save_queue(void);
void parq_upload_send_queue_conf(gnutella_upload_t *u);

gboolean parq_ul_id_sent(gnutella_upload_t *u);

time_t parq_banned_source_expire(guint32 ip);
#endif /* _parq_h_ */
