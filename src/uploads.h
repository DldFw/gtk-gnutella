/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi
 * Copyright (c) 2000 Daniel Walker (dwalker@cats.ucsc.edu)
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

#ifndef _uploads_h_
#define _uploads_h_

#include <sys/types.h>		/* For off_t */

#include "bsched.h"

struct gnutella_node;
struct dl_file_info;
	
typedef struct upload {
    gnet_upload_t upload_handle;
	guint32 flags;					/* Operating flags */
	upload_stage_t status;
	struct gnutella_socket *socket;
	gint error_sent;				/* HTTP error code sent back */
	gpointer io_opaque;				/* Opaque I/O callback information */
	gpointer parq_opaque;			/* Opaque parq information */
	
	gint file_desc;
	bio_source_t *bio;				/* Bandwidth-limited source */

	gchar *buffer;
	gint bpos;
	gint bsize;
	gint buf_size;

	guint index;
	gchar *name;
	guint32 file_size;

	time_t start_date;
	time_t last_update;

	struct dl_file_info *file_info;	/* For PFSP: only set when partial file */

	guint32 ip;						/* Remote IP address */
	gchar *user_agent;				/* Remote user agent */
	guint skip;						/* First byte to send, inclusive */
	guint end;						/* Last byte to send, inclusive */
	off_t pos;						/* Read position in file we're sending */
	guint32 sent;					/* Bytes sent in this request */

	guint32 last_dmesh;				/* Time when last download mesh was sent */
	gchar *sha1;					/* SHA1 of requested file */
	off_t total_requested;			/* Total amount of bytes requested */
	gint http_major;				/* HTTP major version */
	gint http_minor;				/* HTTP minor version */

	gboolean keep_alive;			/* Keep HTTP connection? */
	gboolean push;
	gboolean queue;					/* Similar to PUSH, but this time it is due
				                       to parq */
	gboolean accounted;				/* True when upload was accounted for */
	gboolean unavailable_range;		/* True when last request ended with 416 */
	gboolean n2r;					/* True when they sent an N2R request */

	gboolean parq_status;
} gnutella_upload_t;

#define upload_vendor_str(u)	((u)->user_agent ? (u)->user_agent : "")

/*
 * Operating flags
 */

#define UPLOAD_F_STALLED		0x00000001	/* Stall condition present */
#define UPLOAD_F_EARLY_STALL	0x00000002	/* Pre-stalling condition */

/*
 * This structure is used for HTTP status printing callbacks.
 */
struct upload_http_cb {
	gnutella_upload_t *u;			/* Upload being ACK'ed */
	time_t now;						/* Current time */
	time_t mtime;					/* File modification time */
	struct shared_file *sf;
};

/* 
 * Global Data
 */

extern GSList *uploads;
extern guint32 count_uploads;

/* 
 * Global Functions
 */

gboolean upload_is_enabled(void);
void upload_timer(time_t now);
void upload_remove(struct upload *, const gchar *, ...) G_GNUC_PRINTF(2, 3);
void handle_push_request(struct gnutella_node *);
void upload_add(struct gnutella_socket *s);
void upload_connect_conf(struct upload *u);
void upload_init(void);
void upload_close(void);
void upload_kill_ip(guint32 ip);
void upload_stop_all(struct dl_file_info *fi, const gchar *reason);
void upload_send_giv(guint32 ip, guint16 port, guint8 hops, guint8 ttl,
	guint32 file_index, const gchar *file_name, gboolean banning);
gnutella_upload_t *upload_create(struct gnutella_socket *s, gboolean push);
void upload_fire_upload_info_changed(gnutella_upload_t *n);
void expect_http_header(gnutella_upload_t *u, upload_stage_t new_status);

#endif /* _uploads_h_ */
