/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi
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

#ifndef _downloads_h_
#define _downloads_h_

#include "bsched.h"
#include "fileinfo.h"
#include "header.h"
#include "http.h"
#include "tm.h"

/*
 * We keep a list of all the downloads queued per GUID+IP:port (host).  Indeed
 * some broken clients (e.g. Morpheus) share the same GUID, so we cannot
 * fully discriminate on the GUID alone.  So GUID+IP:port forms the "key",
 * the `dl_key' structure.
 *
 * Inside the `dl_server', we keep track all `download' structures and
 * other server-related information, which are shared by all downloads
 * from this host..
 *
 * Within a single server, a download can be in either runnning, waiting
 * or stopped.  An array of lists is kept, and since the download can be
 * in only one of them, it also keeps track of the proper list index.
 */

enum dl_list {
	DL_LIST_RUNNING	= 0,
	DL_LIST_WAITING = 1,
	DL_LIST_STOPPED = 2,
	DL_LIST_SZ		= 3,
};

struct vernum {
	guint major;
	guint minor;
};

struct dl_key {
	gchar *guid;			/* GUID of server (atom) */
	guint32 ip;				/* IP address of server */
	guint16 port;			/* Port of server */
};

struct dl_server {
	struct dl_key *key;			/* Key properties */
	GList *list[DL_LIST_SZ];	/* Download lists */
	gint count[DL_LIST_SZ];		/* Amount of downloads in list */
	const gchar *vendor;		/* Remote server vendor string (atom) */
	const gchar *hostname;		/* Remote hostname, if known (atom) */
	time_t retry_after;			/* Time at which we may retry from this host */
	time_t dns_lookup;			/* Last DNS lookup for hostname */
	struct vernum parq_version;	/* Supported queueing version */
	guint32 attrs;
	GSList *proxies;			/* Known push proxies (struct gnutella_host) */
	time_t proxies_stamp;		/* Time when list was last updated */
};

/*
 * Download states.
 */

typedef enum {
    GTA_DL_QUEUED           = 1,    /* Download queued, will start later */
    GTA_DL_CONNECTING       = 2,    /* We are connecting to the server */
    GTA_DL_PUSH_SENT        = 3,    /* Sent a push, waiting connection */
    GTA_DL_FALLBACK         = 4,    /* Direct request failed, using push */
    GTA_DL_REQ_SENT         = 5,    /* Request sent, waiting for HTTP headers */
    GTA_DL_HEADERS          = 6,    /* We are receiving the HTTP headers */
    GTA_DL_RECEIVING        = 7,    /* We are receiving the data of the file */
    GTA_DL_COMPLETED        = 8,    /* Download is completed */
    GTA_DL_ERROR            = 9,    /* Download is stopped due to error */
    GTA_DL_ABORTED          = 10,   /* User used the 'Abort Download' button */
    GTA_DL_TIMEOUT_WAIT     = 11,   /* Waiting to try connecting again */
    GTA_DL_REMOVED          = 12,   /* Download was removed, pending free */
    GTA_DL_VERIFY_WAIT      = 13,   /* Waiting to verify SHA1 */
    GTA_DL_VERIFYING        = 14,   /* Computing SHA1 */
    GTA_DL_VERIFIED         = 15,   /* Verify of SHA1 done */
    GTA_DL_MOVE_WAIT        = 16,   /* Waiting to be moved to "done/bad" dir */
    GTA_DL_MOVING           = 17,   /* Being moved to "done/bad" dir */
    GTA_DL_DONE             = 18,   /* All done! */
    GTA_DL_SINKING          = 19,   /* Sinking HTML reply */
    GTA_DL_ACTIVE_QUEUED    = 20,   /* Actively queued */
    GTA_DL_PASSIVE_QUEUED   = 21,   /* Passively queued */
    GTA_DL_REQ_SENDING      = 22,   /* Sending HTTP request */
} download_status_t;


struct download {
    gnet_src_t src_handle;      /* Handle */

	gchar error_str[256];	/* Used to sprintf() error strings with vars */
	download_status_t status;   /* Current status of the download */
	gpointer io_opaque;		/* Opaque I/O callback information */

	bio_source_t *bio;		/* Bandwidth-limited source */

	struct dl_server *server;	/* Remote server description */
	enum dl_list list_idx;		/* List to which download belongs in server */

	struct dl_file_info *file_info;
	guint32 record_index;	/* Index of the file on the Gnutella server */
	gchar *file_name;		/* Name of the file on the Gnutella server */
	gchar *escaped_name;	/* Same as file_name, with control chars escaped */
	guint32 file_size;		/* Total size of the file, in bytes */

	guint32 size;			/* Total size of the next request, in bytes */
	guint32 skip;			/* Number of bytes for file we had before start */
	guint32 pos;			/* Number of bytes of the file we currently have */
	guint32 range_end;		/* First byte offset AFTER the requested range */

	struct gnutella_socket *socket;
	gint file_desc;			/* FD for writing into downloaded file */
	guint32 overlap_size;	/* Size of the overlapping window on resume */
	http_buffer_t *req;		/* HTTP request, when partially sent */

	time_t start_date;		/* Download start date */
	time_t last_update;		/* Last status update or I/O */
	time_t last_gui_update;	/* Last stats update on the GUI */
	time_t record_stamp;	/* Stamp of the query hit that launched us */
	time_t retry_after;		/* Time at which we may retry for this download */
	tm_t header_sent;		/* When we sent the headers, for latency measures */

	guint32 retries;
	guint32 timeout_delay;

	const gchar *remove_msg;

	gchar *sha1;			/* Known SHA1 (binary atom), NULL if none */
	guint32 last_dmesh;		/* Time when last download mesh was sent */

	GSList *ranges;			/* PFSP -- known list of ranges, NULL if none */
	guint32 ranges_size;	/* PFSP -- size of remotely available data */
	guint32 sinkleft;		/* Amount of data left to sink */

	guint32 flags;

	gboolean keep_alive;	/* Keep HTTP connection? */
	gboolean visible;		/* The download is visible in the GUI */
	gboolean push;			/* Currently in push mode */
	gboolean always_push;	/* Always use the push method for this download */
	gboolean got_giv;		/* Whether download created from GIV reception */
	gboolean unavailable;	/* Set on Timout, Push route lost */

	struct cproxy *cproxy;	/* Push proxy being used currently */

	gpointer queue_status;	/* Queuing status */
};

/*
 * Download flags.
 */

#define DL_F_URIRES			0x00000001	/* Tried to GET "/uri-res/N2R?" */
#define DL_F_PUSH_IGN		0x00000002	/* Trying to ignore push flag */
#define DL_F_OVERLAPPED		0x00000004	/* We went through overlap checking */
#define DL_F_REPLIED		0x00000008	/* Servent replied to last request */
#define DL_F_CHUNK_CHOSEN	0x00000010	/* Retrying with specific chunk */
#define DL_F_SHRUNK_REPLY	0x00000020	/* Server sending less than we asked */
#define DL_F_SUNK_DATA		0x00000040	/* Whether we previously sunk data */
#define DL_F_ACTIVE_QUEUED	0x00000080	/* Download is actively queued */
#define DL_F_PASSIVE_QUEUED	0x00000100	/* Download is passively queued */
#define DL_F_DNS_LOOKUP		0x00000200	/* Attempted DNS lookup */
#define DL_F_SUSPENDED		0x40000000	/* Suspended, do not schedule */
#define DL_F_MARK			0x80000000	/* Marked in traversal */

/*
 * Server attributes.
 */

#define DLS_A_NO_URIRES		0x00000001	/* No support for "/uri-res/N2R?" */
#define DLS_A_PUSH_IGN		0x00000002	/* Ignore pushes and connect directly */
#define DLS_A_NO_KEEPALIVE	0x00000004	/* No persistent connection */
#define DLS_A_HTTP_1_1		0x00000008	/* Server supports HTTP/1.1 */
#define DLS_A_MINIMAL_HTTP	0x00000010	/* Use minimalist HTTP with server */
#define DLS_A_BANNING		0x00000020	/* Server might be banning us */
#define DLS_A_FAKE_G2		0x00000040	/* Fake a G2 server */
#define DLS_A_DNS_LOOKUP	0x00000080	/* Perform DNS lookup if possible */
#define DLS_A_REMOVED		0x80000000	/* Server marked for removal */

/*
 * Access macros.
 */

#define download_guid(d)		((d)->server->key->guid)
#define download_ip(d)			((d)->server->key->ip)
#define download_port(d)		((d)->server->key->port)
#define download_vendor(d)		((d)->server->vendor)

#define download_vendor_str(d) \
	((d)->server->vendor ? (d)->server->vendor : "")

#define download_path(d)		((d)->file_info->path)
#define download_outname(d)		((d)->file_info->file_name)
#define download_filesize(d)	((d)->file_info->size)
#define download_filedone(d)	((d)->file_info->done)

/*
 * State inspection macros.
 */

#define DOWNLOAD_IS_QUEUED(d)  ((d)->status == GTA_DL_QUEUED)

#define DOWNLOAD_IS_VERIFYING(d)		 \
	(  (d)->status == GTA_DL_VERIFY_WAIT \
	|| (d)->status == GTA_DL_VERIFYING	 \
	|| (d)->status == GTA_DL_VERIFIED	 )

#define DOWNLOAD_IS_MOVING(d)		 	\
	(  (d)->status == GTA_DL_MOVE_WAIT	\
	|| (d)->status == GTA_DL_MOVING		)

#define DOWNLOAD_IS_STOPPED(d)			\
	(  (d)->status == GTA_DL_ABORTED	\
	|| (d)->status == GTA_DL_ERROR		\
	|| (d)->status == GTA_DL_COMPLETED  \
	|| DOWNLOAD_IS_VERIFYING(d)         \
	|| DOWNLOAD_IS_MOVING(d)            \
	|| (d)->status == GTA_DL_DONE       )

#define DOWNLOAD_IS_ACTIVE(d)			\
	(  (d)->status == GTA_DL_RECEIVING	)

#define DOWNLOAD_IS_WAITING(d)			\
	(  (d)->status == GTA_DL_TIMEOUT_WAIT)

#define DOWNLOAD_IS_ESTABLISHING(d)		\
	(  (d)->status == GTA_DL_CONNECTING \
	|| (d)->status == GTA_DL_PUSH_SENT	\
	|| (d)->status == GTA_DL_FALLBACK	\
	|| (d)->status == GTA_DL_REQ_SENT	\
	|| (d)->status == GTA_DL_REQ_SENDING	\
	|| (d)->status == GTA_DL_ACTIVE_QUEUED	\
	|| (d)->status == GTA_DL_SINKING	\
	|| (d)->status == GTA_DL_HEADERS	)

#define DOWNLOAD_IS_EXPECTING_GIV(d)	\
	(  (d)->status == GTA_DL_PUSH_SENT	\
	|| (d)->status == GTA_DL_FALLBACK	)

#define DOWNLOAD_IS_RUNNING(d)			\
	(	DOWNLOAD_IS_ACTIVE(d)			\
	||	DOWNLOAD_IS_ESTABLISHING(d)		)

#define DOWNLOAD_IS_IN_PUSH_MODE(d) (d->push)
#define DOWNLOAD_IS_VISIBLE(d)		(d->visible)

/* 
 * Global Data
 */

extern GSList *sl_unqueued;

/*
 * Global Functions
 */

void download_init(void);
void download_restore_state(void);
void download_store_if_dirty(void);
void download_timer(time_t now);
void download_info_change_all(
	struct dl_file_info *old_fi, struct dl_file_info *new_fi);
void download_orphan_new(
	gchar *file, guint32 size, gchar *sha1, struct dl_file_info *fi);
void download_queue(struct download *d, const gchar *fmt, ...)
	 G_GNUC_PRINTF(2, 3);
void download_freeze_queue(void);
void download_thaw_queue(void);
gint download_queue_is_frozen(void);
void download_stop(struct download *, guint32, const gchar *, ...)
	G_GNUC_PRINTF(3, 4);
gboolean download_remove(struct download *);
void download_push_ack(struct gnutella_socket *);
void download_fallback_to_push(struct download *, gboolean, gboolean);
void download_pickup_queued(void);
void download_clear_stopped(gboolean, gboolean, gboolean, gboolean);
void download_forget(struct download *, gboolean unavailable);
void download_abort(struct download *);
void download_resume(struct download *);
void download_start(struct download *, gboolean);
gboolean download_start_prepare(struct download *d);
gboolean download_start_prepare_running(struct download *d);
void download_requeue(struct download *);
void download_send_request(struct download *);
void download_retry(struct download *);
void download_close(void);
gint download_remove_all_from_peer(gchar *guid, guint32 ip, guint16 port,
	gboolean unavailable);
gint download_remove_all_named(const gchar *name);
gint download_remove_all_with_sha1(const gchar *sha1);
void download_remove_file(struct download *d, gboolean reset);
gboolean download_file_exists(struct download *d);
gboolean download_server_nopush(gchar *guid, guint32 ip, guint16 port);
const gchar *build_url_from_download(struct download *d);
void download_free_removed(void);
void download_redirect_to_server(struct download *d, guint32 ip, guint16 port);
void download_actively_queued(struct download *d, gboolean queued);

void download_verify_start(struct download *d);
void download_verify_progress(struct download *d, guint32 hashed);
void download_verify_done(struct download *d, gchar *digest, time_t elapsed);
void download_verify_error(struct download *d);

void download_move_start(struct download *d);
void download_move_progress(struct download *d, guint32 copied);
void download_move_done(struct download *d, time_t elapsed);
void download_move_error(struct download *d);

guint extract_retry_after(const header_t *header);
gboolean is_faked_download(struct download *d);

struct download *download_find_waiting_unparq(guint32 ip, guint16 port);
void download_set_socket_rx_size(gint rx_size);

void download_proxy_newstate(struct download *d);
void download_proxy_sent(struct download *d);
void download_proxy_failed(struct download *d);

#endif /* _downloads_h_ */
