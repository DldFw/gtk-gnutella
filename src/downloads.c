/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi
 *
 * Handle downloads.
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
#include "downloads_gui.h"
#include "downloads_gui_common.h"
#include "sockets.h"
#include "downloads.h"
#include "hosts.h"
#include "routing.h"
#include "routing.h"
#include "gmsg.h"
#include "bsched.h"
#include "huge.h"
#include "dmesh.h"
#include "http.h"
#include "version.h"
#include "ignore.h"
#include "ioheader.h"
#include "verify.h"
#include "move.h"
#include "settings.h"
#include "nodes.h"
#include "parq.h"
#include "token.h"
#include "hostiles.h"
#include "clock.h"
#include "uploads.h"
#include "ban.h"
#include "guid.h"
#include "pproxy.h"
#include "tm.h"
#include "file.h"

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <regex.h>

#include "override.h"		/* Must be the last header included */

RCSID("$Id$");

#define DOWNLOAD_MIN_OVERLAP	64			/* Minimum overlap for safety */
#define DOWNLOAD_SHORT_DELAY	2			/* Shortest retry delay */
#define DOWNLOAD_MAX_SINK		16384		/* Max amount of data to sink */
#define DOWNLOAD_SERVER_HOLD	15			/* Space requests to same server */
#define DOWNLOAD_DNS_LOOKUP		7200		/* Period of server DNS lookups */

#define IO_AVG_RATE		5		/* Compute global recv rate every 5 secs */

static GSList *sl_downloads = NULL; /* All downloads (queued + unqueued) */
GSList *sl_unqueued = NULL;			/* Unqueued downloads only */
GSList *sl_removed = NULL;			/* Removed downloads only */
GSList *sl_removed_servers = NULL;	/* Removed servers only */
static gchar dl_tmp[4096];
static gint queue_frozen = 0;

static const gchar DL_OK_EXT[] = ".OK";		/* Extension to mark OK files */
static const gchar DL_BAD_EXT[] = ".BAD"; 	/* "Bad" files (SHA1 mismatch) */
static const gchar DL_UNKN_EXT[] = ".UNKN";	/* For unchecked files */
static const gchar file_what[] = "downloads";/* What we're persisting to file */

static GHashTable *pushed_downloads = 0;

static void download_add_to_list(struct download *d, enum dl_list idx);
static gboolean send_push_request(const gchar *, guint32, guint16);
static void download_read(gpointer data, gint source, inputevt_cond_t cond);
static void download_request(struct download *d, header_t *header, gboolean ok);
static void download_push_ready(struct download *d, getline_t *empty);
static void download_push_remove(struct download *d);
static void download_push(struct download *d, gboolean on_timeout);
static void download_resume_bg_tasks(void);
static void download_incomplete_header(struct download *d);
static gboolean has_blank_guid(const struct download *d);
static void download_verify_sha1(struct download *d);
static gboolean download_get_server_name(struct download *d, header_t *header);
static gboolean use_push_proxy(struct download *d);
static void download_unavailable(struct download *d, guint32 new_status,
	const gchar * reason, ...) G_GNUC_PRINTF(3 ,4);
static void download_reparent(struct download *d, struct dl_server *new_server);
static void download_queue_delay(struct download *d, guint32 delay,
	const gchar *fmt, ...) G_GNUC_PRINTF(3 ,4);
static void download_queue_hold(struct download *d, guint32 hold,
	const gchar *fmt, ...) G_GNUC_PRINTF(3 ,4);

static gboolean download_dirty = FALSE;
static void download_store(void);
static void download_retrieve(void);

/*
 * Download structures.
 *
 * This `dl_key' is inserted in the `dl_by_host' hash table were we find a
 * `dl_server' structure describing all the downloads for the given host.
 *
 * All `dl_server' structures are also inserted in the `dl_by_time' struct,
 * where hosts are sorted based on their retry time.
 *
 * The `dl_count_by_name' hash tables is indexed by name, and counts the
 * amount of downloads scheduled with that name.
 */

static GHashTable *dl_by_host = NULL;
static GHashTable *dl_count_by_name = NULL;
static GHashTable *dl_count_by_sha1 = NULL;

#define DHASH_SIZE	1024			/* Hash list size, must be a power of 2 */
#define DHASH_MASK 	(DHASH_SIZE - 1)
#define DL_HASH(x)	((x) & DHASH_MASK)

static struct {
	GList *servers[DHASH_SIZE];		/* Lists of servers, by retry time */
	gint change[DHASH_SIZE];		/* Counts changes to the list */
} dl_by_time;

/*
 * To handle download meshes, where we only know the IP/port of the host and
 * not its GUID, we need to be able to locate the server.  We know that the
 * IP will not be a private one.
 *
 * Therefore, for each (GUID, IP, port) tuple, where IP is NOT private, we
 * store the (IP, port) => server association as well.	There should be only
 * one such entry, ever.  If there is more, it means the server changed its
 * GUID, which is possible, in which case we simply supersede the old entry.
 */

static GHashTable *dl_by_ip = NULL;

struct dl_ip {				/* Keys in the `dl_by_ip' table. */
	guint32 ip;				/* IP address of server */
	guint16 port;			/* Port of server */
};

static gint dl_establishing = 0;		/* Establishing downloads */
static gint dl_active = 0;				/* Active downloads */

#define count_running_downloads()	(dl_establishing + dl_active)
#define count_running_on_server(s)	(s->count[DL_LIST_RUNNING])

extern gint sha1_eq(gconstpointer a, gconstpointer b);
extern gint guid_eq(gconstpointer a, gconstpointer b);

/***
 *** Sources API
 ***/

static struct event *src_events[EV_SRC_EVENTS] = {
	NULL, NULL, NULL, NULL
};

static idtable_t *src_handle_map = NULL;

static void src_init(void)
{
    src_handle_map = idtable_new(32, 32);

    src_events[EV_SRC_ADDED]          = event_new("src_added");
    src_events[EV_SRC_REMOVED]        = event_new("src_removed");
    src_events[EV_SRC_INFO_CHANGED]   = event_new("src_info_changed");
    src_events[EV_SRC_STATUS_CHANGED] = event_new("src_status_changed");
}

static void src_close(void)
{
    guint n;

    /* See FIXME in download_close()!! */
#if 0
    g_assert(idtable_ids(src_handle_map) == 0);
#endif
    idtable_destroy(src_handle_map);

    for (n = 0; n < G_N_ELEMENTS(src_events); n ++)
        event_destroy(src_events[n]);
}

void src_add_listener(src_listener_t cb, gnet_src_ev_t ev, 
    frequency_t t, guint32 interval)
{
    g_assert(ev < EV_SRC_EVENTS);

    event_add_subscriber(src_events[ev], (GCallback) cb,
        t, interval);
}

void src_remove_listener(src_listener_t cb, gnet_src_ev_t ev)
{
    g_assert(ev < EV_SRC_EVENTS);

    event_remove_subscriber(src_events[ev], (GCallback) cb);
}

/***
 *** Traditional downloads API
 ***/

#ifdef USE_GTK2
#define g_strdown(s) strlower((s), (s))
#endif

/*
 * dl_key_hash
 *
 * Hashing of a `dl_key' structure.
 */
static guint dl_key_hash(gconstpointer key)
{
	const struct dl_key *k = (const struct dl_key *) key;
	guint hash;
	extern guint guid_hash(gconstpointer key);

	hash = guid_hash(k->guid);
	hash ^= k->ip;
	hash ^= (k->port << 16) | k->port;

	return hash;
}

/*
 * dl_key_eq
 *
 * Comparison of `dl_key' structures.
 */
static gint dl_key_eq(gconstpointer a, gconstpointer b)
{
	const struct dl_key *ak = (const struct dl_key *) a;
	const struct dl_key *bk = (const struct dl_key *) b;

	return ak->ip == bk->ip &&
		ak->port == bk->port &&
		guid_eq(ak->guid, bk->guid);
}

/*
 * dl_ip_hash
 *
 * Hashing of a `dl_ip' structure.
 */
static guint dl_ip_hash(gconstpointer key)
{
	const struct dl_ip *k = (const struct dl_ip *) key;
	guint32 hash;

	WRITE_GUINT32_LE(k->ip, &hash); /* Reverse IP, 192.x.y.z -> z.y.x.192 */
	hash ^= (k->port << 16) | k->port;

	return (guint) hash;
}

/*
 * dl_ip_eq
 *
 * Comparison of `dl_ip' structures.
 */
static gint dl_ip_eq(gconstpointer a, gconstpointer b)
{
	const struct dl_ip *ak = (const struct dl_ip *) a;
	const struct dl_ip *bk = (const struct dl_ip *) b;

	return ak->ip == bk->ip && ak->port == bk->port;
}

/*
 * dl_retry_cmp
 *
 * Compare two `download' structures based on the `retry_after' field.
 * The smaller that time, the smaller the structure is.
 */
static gint dl_retry_cmp(gconstpointer a, gconstpointer b)
{
	const struct download *as = (const struct download *) a;
	const struct download *bs = (const struct download *) b;

	if (as->retry_after == bs->retry_after)
		return 0;

	return as->retry_after < bs->retry_after ? -1 : +1;
}

/*
 * dl_server_retry_cmp
 *
 * Compare two `dl_server' structures based on the `retry_after' field.
 * The smaller that time, the smaller the structure is.
 */
static gint dl_server_retry_cmp(gconstpointer a, gconstpointer b)
{
	const struct dl_server *as = (const struct dl_server *) a;
	const struct dl_server *bs = (const struct dl_server *) b;

	if (as->retry_after == bs->retry_after)
		return 0;

	return as->retry_after < bs->retry_after ? -1 : +1;
}

/*
 * has_blank_guid
 *
 * Returns whether download has a blank (fake) GUID.
 */
static gboolean has_blank_guid(const struct download *d)
{
	const gchar *g = download_guid(d);
	gint i;

	for (i = 0; i < 16; i++)
		if (*g++)
			return FALSE;

	return TRUE;
}

/*
 * is_faked_download
 *
 * Returns whether download was faked to reparent a complete orphaned file.
 */
gboolean is_faked_download(struct download *d)
{
	return download_ip(d) == 0 && download_port(d) == 0 && has_blank_guid(d);
}

/*
 * has_good_sha1
 */
static gboolean has_good_sha1(struct download *d)
{
	struct dl_file_info *fi = d->file_info;

	return fi->sha1 == NULL || sha1_eq(fi->sha1, fi->cha1);
}

/* ----------------------------------------- */

/*
 * download_init
 *
 * Initialize downloading data structures.
 */
void download_init(void)
{
	pushed_downloads = g_hash_table_new(g_str_hash, g_str_equal);
	dl_by_host = g_hash_table_new(dl_key_hash, dl_key_eq);
	dl_by_ip = g_hash_table_new(dl_ip_hash, dl_ip_eq);
	dl_count_by_name = g_hash_table_new(g_str_hash, g_str_equal);
	dl_count_by_sha1 = g_hash_table_new(g_str_hash, g_str_equal);

	src_init();
}

/*
 * download_restore_state
 *
 * Initialize downloading data structures.
 */
void download_restore_state(void)
{
	/*
	 * The order of the following calls matters.
	 */

	file_info_retrieve();					/* Get all fileinfos */
	file_info_scandir(save_file_path);		/* Pick up orphaned files */
	download_retrieve();					/* Restore downloads */
	file_info_spot_completed_orphans();		/* 100% done orphans => fake dl. */
	download_resume_bg_tasks();				/* Reschedule SHA1 and moving */
	file_info_store();
}


/* ----------------------------------------- */

/*
 * download_timer
 *
 * Download heartbeat timer.
 */
void download_timer(time_t now)
{
	GSList *l = sl_unqueued;		/* Only downloads not in the queue */

	if (queue_frozen > 0) {
		gui_update_download_clear_now();
		return;
	}

	while (l) {
		struct download *d = (struct download *) l->data;
		guint32 t;

		g_assert(d != NULL);

		l = l->next;

		switch (d->status) {
		case GTA_DL_RECEIVING:
			/*
			 * Update the global average reception rate periodically.
			 */

			{
				struct dl_file_info *fi = d->file_info;

				g_assert(fi->recvcount > 0);

				if (now - fi->recv_last_time > IO_AVG_RATE) {
					fi->recv_last_rate =
						fi->recv_amount / (now - fi->recv_last_time);
					fi->recv_amount = 0;
					fi->recv_last_time = now;
				}
			}
			/* FALL THROUGH */

		case GTA_DL_ACTIVE_QUEUED:
		case GTA_DL_HEADERS:
		case GTA_DL_PUSH_SENT:
		case GTA_DL_CONNECTING:
		case GTA_DL_REQ_SENDING:
		case GTA_DL_REQ_SENT:
		case GTA_DL_FALLBACK:
		case GTA_DL_SINKING:

			if (!is_inet_connected) {
				download_queue(d, _("No longer connected"));
				break;
			}

			switch (d->status) {
			case GTA_DL_ACTIVE_QUEUED:
 				t = get_parq_dl_retry_delay(d);
 				break;
			case GTA_DL_PUSH_SENT:
			case GTA_DL_FALLBACK:
				t = download_push_sent_timeout;
				break;
			case GTA_DL_CONNECTING:
			case GTA_DL_REQ_SENT:
			case GTA_DL_HEADERS:
				t = download_connecting_timeout;
				break;
			default:
				t = download_connected_timeout;
				break;
			}

			if (now - d->last_update > t) {
				/*
				 * When the 'timeout' has expired, first check whether the
				 * download was activly queued. If so, tell parq to retry the
				 * download in which case the HTTP connection wasn't closed
				 *   --JA 31 jan 2003
				 */
				if (d->status == GTA_DL_ACTIVE_QUEUED)
					parq_download_retry_active_queued(d);
				else if (d->status == GTA_DL_CONNECTING)
					download_fallback_to_push(d, TRUE, FALSE);
				else if (d->status == GTA_DL_HEADERS)
					download_incomplete_header(d);
				else {
					if (d->retries++ < download_max_retries)
						download_retry(d);
					else {
						/*
						 * Host is down, probably.  Abort all other downloads
						 * queued for that host as well.
						 */

						download_unavailable(d, GTA_DL_ERROR, "Timeout");
						download_remove_all_from_peer(
							download_guid(d), download_ip(d), download_port(d),
							TRUE);
					}
				}
			} else if (now != d->last_gui_update)
				gui_update_download(d, TRUE);
			break;
		case GTA_DL_TIMEOUT_WAIT:
			if (!is_inet_connected) {
				download_queue(d, _("No longer connected"));
				break;
			}

			if (now - d->last_update > d->timeout_delay)
				download_start(d, TRUE);
			else
				gui_update_download(d, FALSE);
			break;
		case GTA_DL_VERIFYING:
		case GTA_DL_MOVING:
			gui_update_download(d, FALSE);
			break;
		case GTA_DL_COMPLETED:
		case GTA_DL_ABORTED:
		case GTA_DL_ERROR:
		case GTA_DL_VERIFY_WAIT:
		case GTA_DL_VERIFIED:
		case GTA_DL_MOVE_WAIT:
		case GTA_DL_DONE:
		case GTA_DL_REMOVED:
			break;
		case GTA_DL_QUEUED:
			g_error("found queued download in sl_unqueued list: \"%s\"",
				d->file_name);
			break;
		default:
			g_warning("Hmm... new download state %d not handled for \"%s\"",
				d->status, d->file_name);
			break;
		}
	}

	download_clear_stopped(
		clear_complete_downloads, 
		clear_failed_downloads,
		clear_unavailable_downloads,
		FALSE);

	download_free_removed();
	gui_update_download_clear_now();

	/* Dequeuing */
	if (is_inet_connected)
		download_pickup_queued();
}

/* ----------------------------------------- */

/*
 * dl_by_time_insert
 *
 * Insert server by retry time into the `dl_by_time' structure.
 */
static void dl_by_time_insert(struct dl_server *server)
{
	gint idx = DL_HASH(server->retry_after);

	dl_by_time.change[idx]++;
	dl_by_time.servers[idx] = g_list_insert_sorted(dl_by_time.servers[idx],
		server, dl_server_retry_cmp);
}

/*
 * dl_by_time_remove
 *
 * Remove server from the `dl_by_time' structure.
 */
static void dl_by_time_remove(struct dl_server *server)
{
	gint idx = DL_HASH(server->retry_after);

	dl_by_time.change[idx]++;
	dl_by_time.servers[idx] = g_list_remove(dl_by_time.servers[idx], server);
}

/*
 * hostvec_to_slist
 *
 * Convert a vector of host to a single-linked list.
 * Returns new list, with every item cloned.
 */
static GSList *hostvec_to_slist(gnet_host_vec_t *vec)
{
	GSList *l = NULL;
	gint i;
	
	for (i = vec->hvcnt - 1; i >= 0; i--) {
		gnet_host_t *h = &vec->hvec[i];
		gnet_host_t *host = walloc(sizeof(*host));

		host->ip = h->ip;
		host->port = h->port;

		l = g_slist_prepend(l, host);
	}

	return l;
}

/*
 * free_proxies
 *
 * Get rid of the list of push proxies held in the server.
 */
static void free_proxies(struct dl_server *server)
{
	GSList *l;

	g_assert(server);
	g_assert(server->proxies);

	for (l = server->proxies; l; l = g_slist_next(l)) {
		struct gnutella_host *h = (struct gnutella_host *) l->data;
		wfree(h, sizeof(*h));
	}

	g_slist_free(server->proxies);
	server->proxies = NULL;
}

/*
 * remove_proxy
 *
 * Remove push proxy from server.
 */
static void remove_proxy(struct dl_server *server, guint32 ip, guint16 port)
{
	GSList *l;
	
	for (l = server->proxies; l; l = g_slist_next(l)) {
		struct gnutella_host *h = (struct gnutella_host *) l->data;
		g_assert(h != NULL);

		if (h->ip == ip && h->port == port) {
			server->proxies = g_slist_remove_link(server->proxies, l);
			g_slist_free_1(l);
			wfree(h, sizeof(*h));
			return;
		}
	}

	/*
	 * The following could happen when we reset the list of push-proxies
	 * for a host after having selected a push-proxy from the old stale list.
	 */

	if (dbg)
		g_warning("did not find push-proxy %s in server %s",
			ip_port_to_gchar(ip, port), ip_to_gchar(server->key->ip));
}

/*
 * allocate_server
 *
 * Allocate new server structure.
 */
static struct dl_server *allocate_server(
	const gchar *guid, guint32 ip, guint16 port)
{
	struct dl_key *key;
	struct dl_server *server;

	key = walloc(sizeof(*key));
	key->ip = ip;
	key->port = port;
	key->guid = atom_guid_get(guid);

	server = walloc0(sizeof(*server));
	server->key = key;
	server->retry_after = time(NULL);

	g_hash_table_insert(dl_by_host, key, server);
	dl_by_time_insert(server);

	/*
	 * If host is reacheable directly, its GUID does not matter much to
	 * identify the server as the (IP, port) should be unique.
	 */

	if (host_is_valid(ip, port)) {
		struct dl_ip *ipk;
		gpointer ipkey;
		gpointer x;					/* Don't care about freeing values */
		gboolean existed;

		ipk = walloc(sizeof(*ipk));
		ipk->ip = ip;
		ipk->port = port;

		existed = g_hash_table_lookup_extended(dl_by_ip, ipk, &ipkey, &x);
		g_hash_table_insert(dl_by_ip, ipk, server);

		if (existed)
			wfree(ipkey, sizeof(*ipk));	/* Old key superseded by new one */
	}

	return server;
}

/*
 * free_server
 *
 * Free server structure.
 */
static void free_server(struct dl_server *server)
{
	struct dl_ip ipk;

	dl_by_time_remove(server);
	g_hash_table_remove(dl_by_host, server->key);

	if (server->vendor)
		atom_str_free(server->vendor);
	atom_guid_free(server->key->guid);

	/*
	 * We only inserted the server in the `dl_ip' table if it was "reachable".
	 */

	ipk.ip = server->key->ip;
	ipk.port = server->key->port;

	if (host_is_valid(ipk.ip, ipk.port)) {
		gpointer ipkey;
		gpointer x;					/* Don't care about freeing values */

		if (g_hash_table_lookup_extended(dl_by_ip, &ipk, &ipkey, &x)) {
			g_hash_table_remove(dl_by_ip, &ipk);
			wfree(ipkey, sizeof(struct dl_ip));
		}
	}

	/*
	 * Get rid of the known push proxies, if any.
	 */

	if (server->proxies)
		free_proxies(server);

	if (server->hostname != NULL)
		atom_str_free(server->hostname);

	wfree(server->key, sizeof(struct dl_key));
	wfree(server, sizeof(*server));
}

/*
 * get_server
 *
 * Fetch server entry identified by IP:port first, then GUID+IP:port.
 * Returns NULL if not found.
 */
static struct dl_server *get_server(
	gchar *guid, guint32 ip, guint16 port)
{
	struct dl_ip ikey;
	struct dl_key key;
	struct dl_server *server;

	g_assert(guid);

	ikey.ip = ip;
	ikey.port = port;

	server = (struct dl_server *) g_hash_table_lookup(dl_by_ip, &ikey);
	if (server)
		return server;

	key.guid = guid;
	key.ip = ip;
	key.port = port;

	return (struct dl_server *) g_hash_table_lookup(dl_by_host, &key);
}

/*
 * change_server_ip
 *
 * The server IP address changed.
 */
static void change_server_ip(struct dl_server *server, guint32 new_ip)
{
	struct dl_key *key = server->key;
	struct dl_server *dup;
	guint32 old_ip;
	GSList *l;

	g_assert(key->ip != new_ip);

	g_hash_table_remove(dl_by_host, key);

	/*
	 * We only inserted the server in the `dl_ip' table if it was "reachable".
	 */

	if (host_is_valid(key->ip, key->port)) {
		struct dl_ip ipk;
		gpointer ipkey;
		gpointer x;					/* Don't care about freeing values */

		ipk.ip = key->ip;
		ipk.port = key->port;

		if (g_hash_table_lookup_extended(dl_by_ip, &ipk, &ipkey, &x)) {
			g_hash_table_remove(dl_by_ip, &ipk);
			wfree(ipkey, sizeof(struct dl_ip));
		}
	}

	/*
	 * Get rid of the known push proxies, if any.
	 */

	if (server->proxies)
		free_proxies(server);

	if (dbg)
		g_warning("server <%s> at %s:%u changed its IP from %s to %s",
			server->vendor == NULL ? "UNKNOWN" : server->vendor,
			server->hostname == NULL ? "NONAME" : server->hostname,
			key->port, ip_to_gchar(key->ip), ip2_to_gchar(new_ip));

	/*
	 * Perform the IP change.
	 */

	old_ip = key->ip;
	key->ip = new_ip;

	/*
	 * Look for a duplicate.  It's quite possible that we saw some IP
	 * address 1.2.3.4 and 5.6.7.8 without knowing that they both were
	 * for the foo.example.com host.  And now we learn that the name
	 * foo.example.com which we thought was 5.6.7.8 is at 1.2.3.4...
	 */

	dup = get_server(key->guid, new_ip, key->port);

	if (dup != NULL) {
		g_assert(dup->key->ip == key->ip);
		g_assert(dup->key->port == key->port);

		if (dbg) g_warning(
			"new IP %s for server <%s> at %s:%u was used by <%s> at %s:%u",
			ip_to_gchar(new_ip),
			server->vendor == NULL ? "UNKNOWN" : server->vendor,
			server->hostname == NULL ? "NONAME" : server->hostname,
			key->port,
			dup->vendor == NULL ? "UNKNOWN" : dup->vendor,
			dup->hostname == NULL ? "NONAME" : dup->hostname,
			dup->key->port);

		/*
		 * If there was no GUID known for `server', copy the one from `dup'.
		 */

		if (
			guid_eq(key->guid, blank_guid) &&
			!guid_eq(dup->key->guid, blank_guid)
		) {
			atom_guid_free(key->guid);
			key->guid = atom_guid_get(dup->key->guid);
		} else if (
			!guid_eq(key->guid, dup->key->guid) &&
			!guid_eq(dup->key->guid, blank_guid)
		)
			g_warning("found two distinct GUID for <%s> at %s:%u, keeping %s",
				server->vendor == NULL ? "UNKNOWN" : server->vendor,
				server->hostname == NULL ? "NONAME" : server->hostname,
				key->port, guid_hex_str(key->guid));

		/*
		 * All the downloads attached to the `dup' server need to be
		 * reparented to `server' instead.
		 */

		for (l = sl_downloads; l; l = g_slist_next(l)) {
			struct download *d = (struct download *) l->data;
			g_assert(d != NULL);

			if (d->status == GTA_DL_REMOVED)
				continue;

			if (d->server == dup)
				download_reparent(d, server);
		}
	}

	/*
	 * We can now blindly insert `server' in the hash.  If there was a
	 * conflicting entry, all its downloads have been reparented and that
	 * server will be freed later, asynchronously.
	 */

	g_hash_table_insert(dl_by_host, key, server);

	if (host_is_valid(key->ip, key->port)) {
		struct dl_ip *ipk;
		gpointer ipkey;
		gpointer x;					/* Don't care about freeing values */
		gboolean existed;

		ipk = walloc(sizeof(*ipk));
		ipk->ip = new_ip;
		ipk->port = key->port;

		existed = g_hash_table_lookup_extended(dl_by_ip, ipk, &ipkey, &x);
		g_hash_table_insert(dl_by_ip, ipk, server);

		if (existed)
			wfree(ipkey, sizeof(*ipk));	/* Old key superseded by new one */
	}
}

/*
 * set_server_hostname
 *
 * Set/change the server's hostname.
 */
static void set_server_hostname(struct dl_server *server, gchar *hostname)
{
	if (server->hostname != NULL) {
		atom_str_free(server->hostname);
		server->hostname = NULL;
	}

	if (hostname != NULL)
		server->hostname = atom_str_get(hostname);
}

/*
 * download_server_nopush
 *
 * Check whether we can safely ignore Push indication for this server,
 * identified by its GUID, IP and port.
 */
gboolean download_server_nopush(gchar *guid, guint32 ip, guint16 port)
{
	struct dl_server *server = get_server(guid, ip, port);

	if (server == NULL)
		return FALSE;

	/* 
	 * Returns true if we already made a direct connection to this server.
	 */

	return server->attrs & DLS_A_PUSH_IGN;
}

/*
 * count_running_downloads_with_name
 *
 * How many downloads with same filename are running (active or establishing)?
 */
static guint count_running_downloads_with_name(const char *name)
{
	return GPOINTER_TO_UINT(g_hash_table_lookup(dl_count_by_name, name));
}

/*
 * downloads_with_name_inc
 *
 * Add one to the amount of downloads running and bearing the filename.
 */
static void downloads_with_name_inc(const gchar *name)
{
	guint val;

	val = GPOINTER_TO_UINT(g_hash_table_lookup(dl_count_by_name, name));
	g_hash_table_insert(dl_count_by_name, (gchar *) name,
		GUINT_TO_POINTER(val + 1));
}

/*
 * downloads_with_name_dec
 *
 * Remove one from the amount of downloads running and bearing the filename.
 */
static void downloads_with_name_dec(gchar *name)
{
	guint val;

	val = GPOINTER_TO_UINT(g_hash_table_lookup(dl_count_by_name, name));

	g_assert(val);		/* Cannot decrement something not present */

	if (val > 1)
		g_hash_table_insert(dl_count_by_name, name, GUINT_TO_POINTER(val - 1));
	else
		g_hash_table_remove(dl_count_by_name, name);
}

/*
 * has_same_download
 *
 * Check whether we already have an identical (same file, same SHA1, same host)
 * running or queued download.
 *
 * Returns found active download, or NULL if we have no such download yet.
 */
static struct download *has_same_download(
	const gchar *file, const gchar *sha1, gchar *guid,
	guint32 ip, guint16 port)
{
	struct dl_server *server = get_server(guid, ip, port);
	GList *l;
	gint n;
	enum dl_list listnum[] = { DL_LIST_WAITING, DL_LIST_RUNNING };

	if (server == NULL)
		return NULL;

	/*
	 * Note that we scan the WAITING downloads first, and then only
	 * the RUNNING ones.  This is because that routine can now be called
	 * from download_convert_to_urires(), where the download is actually
	 * running!
	 */

	for (n = 0; n < G_N_ELEMENTS(listnum); n++) {
		for (l = server->list[n]; l; l = l->next) {
			struct download *d = (struct download *) l->data;

			g_assert(!DOWNLOAD_IS_STOPPED(d));

			if (sha1 && d->sha1 && sha1_eq(d->sha1, sha1))
				return d;
			if (0 == strcmp(file, d->file_name))
				return d;
		}
	}

	return NULL;
}

/*
 * download_actively_queued
 *
 * Mark a download as being actively queued.
 */
void download_actively_queued(struct download *d, gboolean queued)
{
	if (queued) {
		d->status = GTA_DL_ACTIVE_QUEUED;

		if (d->flags & DL_F_ACTIVE_QUEUED)		/* Already accounted for */
			return;

		d->flags |= DL_F_ACTIVE_QUEUED;
		gnet_prop_set_guint32_val(PROP_DL_AQUEUED_COUNT, dl_aqueued_count + 1);
	} else {
		if (!(d->flags & DL_F_ACTIVE_QUEUED))	/* Already accounted for */
			return;

		gnet_prop_set_guint32_val(PROP_DL_AQUEUED_COUNT, dl_aqueued_count - 1);
		g_assert((gint) dl_aqueued_count >= 0);
		d->flags &= ~DL_F_ACTIVE_QUEUED;
	}
}

/*
 * download_passively_queued
 *
 * Mark download as being passively queued.
 */
static void download_passively_queued(struct download *d, gboolean queued)
{
	if (queued) {
		if (d->flags & DL_F_PASSIVE_QUEUED)		/* Already accounted for */
			return;

		d->flags |= DL_F_PASSIVE_QUEUED;
		gnet_prop_set_guint32_val(PROP_DL_PQUEUED_COUNT, dl_pqueued_count + 1);
	} else {
		if (!(d->flags & DL_F_PASSIVE_QUEUED))	/* Already accounted for */
			return;

		gnet_prop_set_guint32_val(PROP_DL_PQUEUED_COUNT, dl_pqueued_count - 1);
		g_assert((gint) dl_pqueued_count >= 0);
		d->flags &= ~DL_F_PASSIVE_QUEUED;
	}
}

/*
 * download_file_exists
 *
 * Returns whether the download file exists in the temporary directory.
 */
gboolean download_file_exists(struct download *d)
{
	gboolean ret;
	gchar *path;
	struct stat buf;

	path = g_strdup_printf("%s/%s",
				d->file_info->path, d->file_info->file_name);
	g_return_val_if_fail(NULL != path, FALSE);

	ret = -1 != stat(path, &buf);
	G_FREE_NULL(path);

	return ret;
}

/*
 * download_remove_file
 *
 * Remove temporary download file.
 *
 * Optionally reset the fileinfo if unlinking is successfull and `reset' is
 * TRUE.  The purpose of resetting on unlink is to prevent the fileinfo
 * from being discarded at the next relaunch (we discard non-reset fileinfos
 * when the file is missing).
 */
void download_remove_file(struct download *d, gboolean reset)
{
	struct dl_file_info *fi = d->file_info;
	GSList *l;

	file_info_unlink(fi);
	if (reset)
		file_info_reset(fi);

	/*
	 * Requeue all the active downloads that were referencing that file.
	 */

	for (l = sl_downloads; l; l = g_slist_next(l)) {
		struct download *d = (struct download *) l->data;
		g_assert(d != NULL);

		if (d->status == GTA_DL_REMOVED)
			continue;

		if (d->file_info != fi)
			continue;

		/*
		 * An actively queued download is counted as running, but for our
		 * purposes here, it does not matter: we're not in the process of
		 * requesting the file.  Likewise for other special states that are
		 * counted as running but are harmless here.
		 *		--RAM, 17/05/2003
		 */

		switch (d->status) {
		case GTA_DL_ACTIVE_QUEUED:
		case GTA_DL_PUSH_SENT:
		case GTA_DL_FALLBACK:
		case GTA_DL_SINKING:		/* Will only make a new request after */
		case GTA_DL_CONNECTING:
			continue;
		default:
			break;		/* go on */
		}

		if (DOWNLOAD_IS_RUNNING(d)) {
			download_stop(d, GTA_DL_TIMEOUT_WAIT, NULL);
			download_queue(d, "Requeued due to file removal");
		}
	}
}

/*
 * download_info_change_all
 *
 * Change all the fileinfo of downloads from `old_fi' to `new_fi'.
 *
 * All running downloads are requeued immediately, since a change means
 * the underlying file we're writing to can change.
 */
void download_info_change_all(
	struct dl_file_info *old_fi, struct dl_file_info *new_fi)
{
	GSList *l;

	for (l = sl_downloads; l; l = g_slist_next(l)) {
		struct download *d = (struct download *) l->data;
		gboolean is_running;
		g_assert(d != NULL);

		if (d->status == GTA_DL_REMOVED)
			continue;
	
		if (d->file_info != old_fi)
			continue;

		is_running = DOWNLOAD_IS_RUNNING(d);

		/*
		 * The following states are marked as being running, but the
		 * fileinfo structure has not yet been used to request anything,
		 * so we don't need to stop.
		 */

		switch (d->status) {
		case GTA_DL_ACTIVE_QUEUED:
		case GTA_DL_PUSH_SENT:
		case GTA_DL_FALLBACK:
		case GTA_DL_SINKING:
		case GTA_DL_CONNECTING:
			is_running = FALSE;
			break;
		default:
			break;
		}

		if (is_running)
			download_stop(d, GTA_DL_TIMEOUT_WAIT, NULL);

		switch (d->status) {
		case GTA_DL_COMPLETED:
		case GTA_DL_ABORTED:
		case GTA_DL_ERROR:
		case GTA_DL_VERIFY_WAIT:
		case GTA_DL_VERIFYING:
		case GTA_DL_VERIFIED:
		case GTA_DL_MOVE_WAIT:
		case GTA_DL_MOVING:
		case GTA_DL_DONE:
			break;
		default:
			g_assert(old_fi->lifecount > 0);
			old_fi->lifecount--;
			new_fi->lifecount++;
			break;
		}

		g_assert(old_fi->refcount > 0);
		file_info_remove_source(old_fi, d, FALSE); /* Keep it around */
		file_info_add_source(new_fi, d);

		d->flags &= ~DL_F_SUSPENDED;
		if (new_fi->flags & FI_F_SUSPEND)
			d->flags |= DL_F_SUSPENDED;

		if (is_running)
			download_queue(d, _("Requeued by file info change"));
	}
}

/*
 * download_info_reget
 *
 * Invalidate improper fileinfo for the download, and get new one.
 *
 * This usually happens when we discover the SHA1 of the file on the remote
 * server, and see that it does not match the one for the associated file on
 * disk, as described in `file_info'.
 */
static void download_info_reget(struct download *d)
{
	struct dl_file_info *fi = d->file_info;

	g_assert(fi);
	g_assert(fi->lifecount > 0);
	g_assert(fi->lifecount <= fi->refcount);

	downloads_with_name_dec(fi->file_name);		/* File name can change! */
	file_info_clear_download(d, TRUE);			/* `d' might be running */

	fi->lifecount--;
	file_info_remove_source(fi, d, FALSE);		/* Keep it around for others */

	fi = file_info_get(
		d->file_name, save_file_path, d->file_size, d->sha1);
	file_info_add_source(fi, d);
	fi->lifecount++;

	d->flags &= ~DL_F_SUSPENDED;
	if (fi->flags & FI_F_SUSPEND)
		d->flags |= DL_F_SUSPENDED;

	downloads_with_name_inc(fi->file_name);
}

/*
 * queue_suspend_downloads_with_file
 *
 * Mark all downloads that point to the file_info struct as "suspended" if
 * `suspend' is TRUE, or clear that mark if FALSE.
 */
static void queue_suspend_downloads_with_file(
	struct dl_file_info *fi, gboolean suspend)
{
	GSList *sl;

	for (sl = sl_downloads; sl != NULL; sl = g_slist_next(sl)) {
		struct download *d = (struct download *) sl->data;
		g_assert(d != NULL);

		switch (d->status) {
		case GTA_DL_REMOVED:
		case GTA_DL_COMPLETED:
		case GTA_DL_VERIFY_WAIT:
		case GTA_DL_VERIFYING:
		case GTA_DL_VERIFIED:
		case GTA_DL_MOVE_WAIT:
		case GTA_DL_MOVING:
			continue;
		case GTA_DL_DONE:		/* We want to be able to "un-suspend" */
			break;
		default:
			break;
		}

		if (d->file_info != fi)
			continue;

		if (suspend) {
			if (DOWNLOAD_IS_RUNNING(d))
				download_queue(d, _("Suspended (SHA1 checking)"));
			d->flags |= DL_F_SUSPENDED;		/* Can no longer be scheduled */
		} else
			d->flags &= ~DL_F_SUSPENDED;
	}

	if (suspend)
		fi->flags |= FI_F_SUSPEND;
	else
		fi->flags &= ~FI_F_SUSPEND;
}

/*
 * queue_remove_downloads_with_file
 *
 * Removes all downloads that point to the file_info struct.
 * If `skip' is non-NULL, that download is skipped.
 */
static void queue_remove_downloads_with_file(
	struct dl_file_info *fi, struct download *skip)
{
	GSList *sl;
	GSList *to_remove = NULL;

	for (sl = sl_downloads; sl != NULL; sl = g_slist_next(sl)) {
		struct download *d = (struct download *) sl->data;

		g_assert(d != NULL);

		switch (d->status) {
		case GTA_DL_REMOVED:
		case GTA_DL_COMPLETED:
		case GTA_DL_VERIFY_WAIT:
		case GTA_DL_VERIFYING:
		case GTA_DL_VERIFIED:
		case GTA_DL_MOVE_WAIT:
		case GTA_DL_MOVING:
		case GTA_DL_DONE:
			continue;
		default:
			break;
		}

		if (d->file_info != fi || d == skip)
			continue;

		to_remove = g_slist_prepend(to_remove, d);
	}

	for (sl = to_remove; sl != NULL; sl = g_slist_next(sl))
		download_remove((struct download *) sl->data);

	g_slist_free(to_remove);
}

/*
 * download_remove_all_from_peer
 *
 * Remove all downloads to a given peer from the download queue
 * and abort all connections to peer in the active download list.
 *
 * When `unavailable' is TRUE, the downloads are marked unavailable,
 * so that they can be cleared up differently by the GUI .
 *
 * Return the number of removed downloads.
 */
gint download_remove_all_from_peer(gchar *guid, guint32 ip, guint16 port,
	gboolean unavailable)
{
	struct dl_server *server[2];
	gint n = 0;
	enum dl_list listnum[] = { DL_LIST_RUNNING, DL_LIST_WAITING };
	GSList *to_remove = NULL;
	GSList *sl;
	gint i;
	gint j;

	/*
	 * There can be two distinct server entries for a given IP:port.
	 * One with the GUID, and one with a blank GUID.  The latter is
	 * used when we enqueue entries from the download mesh: we don't
	 * have the GUID handy at that point.
	 *
	 * NB: It is conceivable that a server could change GUID between two
	 * sessions, and therefore we may miss to remove downloads from the
	 * same IP:port.  Apart from looping throughout the whole queue,
	 * there is nothing we can do.
	 *		--RAM, 15/10/2002.
	 */

	server[0] = get_server(guid, ip, port);
	server[1] = get_server(blank_guid, ip, port);

	if (server[1] == server[0])
		server[1] = NULL;

	for (i = 0; i < 2; i++) {
		if (server[i] == NULL)
			continue;

		for (j = 0; j < G_N_ELEMENTS(listnum); j++) {
			enum dl_list idx = listnum[j];
			GList *l;

			for (l = server[i]->list[idx]; l; l = g_list_next(l)) {
				struct download *d = (struct download *) l->data;

				g_assert(d);
				g_assert(d->status != GTA_DL_REMOVED);

				n++;
				to_remove = g_slist_prepend(to_remove, d);
			}
		}
	}

	/*
	 * We "forget" instead of "aborting"  all requested downloads: we do
	 * not want to delete the file on the disk if they selected "delete on
	 * abort".
	 * Do NOT mark the fileinfo as "discard".
	 */

	for (sl = to_remove; sl != NULL; sl = g_slist_next(sl)) {
		struct download *d = (struct download *) sl->data;
		download_forget(d, unavailable);
	}

	g_slist_free(to_remove);

	return n;
}

/*
 * download_remove_all_named
 *
 * remove all downloads with a given name from the download queue
 * and abort all connections to peer in the active download list.
 * Returns the number of removed downloads.
 */
gint download_remove_all_named(const gchar *name)
{
	GSList *sl;
	GSList *to_remove = NULL;
	gint n = 0;

	g_return_val_if_fail(name, 0);
	
	for (sl = sl_downloads; sl != NULL; sl = g_slist_next(sl)) {
		struct download *d = (struct download *) sl->data;

		g_assert(d != NULL);

		if (
			(d->status == GTA_DL_REMOVED) ||
			(strcmp(name, d->file_name) != 0)
		)
			continue;

		n++;
		to_remove = g_slist_prepend(to_remove, d);
	}

	/*
	 * Abort all requested downloads, and mark their fileinfo as "discard"
	 * so that we reclaim it when the last reference is gone: if we came
	 * here, it means they are no longer interested in that file, so it's
	 * no use to keep it around for "alternate" source location matching.
	 *		--RAM, 05/11/2002
	 */

	for (sl = to_remove; sl != NULL; sl = g_slist_next(sl)) {
		struct download *d = (struct download *) sl->data;
		file_info_set_discard(d->file_info, TRUE);
		download_abort(d);
	}

	g_slist_free(to_remove);

	return n;
}

/*
 * download_remove_all_with_sha1
 *
 * remove all downloads with a given sha1 hash from the download queue
 * and abort all conenctions to peer in the active download list.
 * Returns the number of removed downloads.
 * Note: if sha1 is NULL, we do not clear all download with sha1==NULL
 *		but abort instead.
 */
gint download_remove_all_with_sha1(const gchar *sha1)
{
	GSList *sl;
	GSList *to_remove = NULL;
	gint n = 0;

	g_return_val_if_fail(sha1 != NULL, 0);
	
	for (sl = sl_downloads; sl != NULL; sl = g_slist_next(sl)) {
		struct download *d = (struct download *) sl->data;

		g_assert(d != NULL);

		if (
			(d->status == GTA_DL_REMOVED) ||
			(d->file_info->sha1 == NULL) ||
			(memcmp(sha1, d->file_info->sha1, SHA1_RAW_SIZE) != 0)
		)
			continue;

		n++;
		to_remove = g_slist_prepend(to_remove, d);
	}

	/*
	 * Abort all requested downloads, and mark their fileinfo as "discard"
	 * so that we reclaim it when the last reference is gone: if we came
	 * here, it means they are no longer interested in that file, so it's
	 * no use to keep it around for "alternate" source location matching.
	 *		--RAM, 05/11/2002
	 */

	for (sl = to_remove; sl != NULL; sl = g_slist_next(sl)) {
		struct download *d = (struct download *) sl->data;
		file_info_set_discard(d->file_info, TRUE);
		download_abort(d);
	}

	g_slist_free(to_remove);

	return n;
}

/*
 * download_set_socket_rx_size
 *
 * Change the socket RX buffer size for all the currently connected
 * downloads.
 */
void download_set_socket_rx_size(gint rx_size)
{
	GSList *sl;

	g_assert(rx_size > 0);

	for (sl = sl_downloads; sl != NULL; sl = g_slist_next(sl)) {
		struct download *d = (struct download *) sl->data;
	
		if (d->socket != NULL)
			sock_recv_buf(d->socket, rx_size, TRUE);
	}
}

/*
 * GUI operations
 */

/* 
 * download_clear_stopped:
 * 
 * Remove stopped downloads. 
 * complete == TRUE:    removes DONE | COMPLETED
 * failed == TRUE:      removes ERROR | ABORTED without `unavailable' set
 * unavailable == TRUE: removes ERROR | ABORTED with `unavailable' set
 * now == TRUE:         remove immediately, else remove only downloads
 *                      idle since at least "entry_removal_timeout" seconds 
 */
void download_clear_stopped(gboolean complete,
	gboolean failed, gboolean unavailable, gboolean now)
{
	GSList *sl;
	time_t current_time = 0;

	if (sl_unqueued == NULL)
		return;

	if (!now)
		current_time = time(NULL);

	for (sl = sl_unqueued; sl; sl = g_slist_next(sl)) {
		struct download *d = (struct download *) sl->data;
		g_assert(d != NULL);

		if (d->status == GTA_DL_REMOVED)
			continue;

		switch (d->status) {
		case GTA_DL_ERROR:
		case GTA_DL_ABORTED:
		case GTA_DL_COMPLETED:
		case GTA_DL_DONE:
			break;
		default:
			continue;
		}

		if (now || (current_time - d->last_update) > entry_removal_timeout) {
			if (
				complete && (
					d->status == GTA_DL_DONE || 
					d->status == GTA_DL_COMPLETED) 
			) {
				download_remove(d);
			}
			else if (
				d->status == GTA_DL_ERROR ||
				d->status == GTA_DL_ABORTED
			) {
				if (
					(failed && !d->unavailable) ||
					(unavailable && d->unavailable)
				)
					download_remove(d);
			}
		}
	}

	gui_update_download_abort_resume();
	gui_update_download_clear();
}

/*
 * Downloads management
 */

/*
 * download_add_to_list
 */
static void download_add_to_list(struct download *d, enum dl_list idx)
{
	struct dl_server *server = d->server;

	g_assert(idx != -1);
	g_assert(d->list_idx == -1);			/* Not in any list */

	d->list_idx = idx;

	/*
	 * The DL_LIST_WAITING list is sorted by increasing retry after.
	 */

	server->list[idx] = (idx == DL_LIST_WAITING) ?
		g_list_insert_sorted(server->list[idx], d, dl_retry_cmp) :
		g_list_prepend(server->list[idx], d);

	server->count[idx]++;
}

/*
 * download_move_to_list
 *
 * Move download from its current list to the `idx' one.
 */
static void download_move_to_list(struct download *d, enum dl_list idx)
{
	struct dl_server *server = d->server;
	enum dl_list old_idx = d->list_idx;

	g_assert(d->list_idx != -1);			/* In some list */
	g_assert(d->list_idx != idx);			/* Not in the target list */

	/*
	 * Global counters update.
	 */

	if (old_idx == DL_LIST_RUNNING) {
		if (DOWNLOAD_IS_ACTIVE(d))
			dl_active--;
		else {
			g_assert(DOWNLOAD_IS_ESTABLISHING(d));
			g_assert(dl_establishing > 0);
			dl_establishing--;
		}
		downloads_with_name_dec(download_outname(d));
	} else if (idx == DL_LIST_RUNNING) {
		dl_establishing++;
		downloads_with_name_inc(download_outname(d));
	}

	g_assert(dl_active >= 0 && dl_establishing >= 0);

	/*
	 * Local counter and list update.
	 * The DL_LIST_WAITING list is sorted by increasing retry after.
	 */

	g_assert(server->count[old_idx] > 0);

	server->list[old_idx] = g_list_remove(server->list[old_idx], d);
	server->count[old_idx]--;

	server->list[idx] = (idx == DL_LIST_WAITING) ?
		g_list_insert_sorted(server->list[idx], d, dl_retry_cmp) :
		g_list_append(server->list[idx], d);

	server->count[idx]++;

	d->list_idx = idx;
}

/*
 * download_server_retry_after
 *
 * Change the `retry_after' field of the host where this download runs.
 * If a non-zero `hold' is specified, make sure nothing will be scheduled
 * from this server before the next `hold' seconds.
 */
static void download_server_retry_after(
	struct dl_server *server, time_t now, time_t hold)
{
	struct download *d;
	time_t after;

	g_assert(server != NULL);
	g_assert(server->count[DL_LIST_WAITING]);	/* We have queued something */

	/*
	 * Always consider the earliest time in the future for all the downloads
	 * enqueued in the server when updating its `retry_after' field.
	 *
	 * Indeed, we may have several downloads queued with PARQ, and each
	 * download bears its own retry_after time.  But we need to know the
	 * earliest time at which we should start browsing through the downloads
	 * for a given server.
	 *		--RAM, 16/07/2003
	 */

	d = (struct download *) server->list[DL_LIST_WAITING]->data;
	after = d->retry_after;

	/*
	 * We impose a minimum of DOWNLOAD_SERVER_HOLD seconds between retries.
	 * If we have some entries passively queued, well, we have some grace time
	 * before the entry expires.  And even if it expires, we won't loose the
	 * slot.  People having 100 entries passively queued on the same host with
	 * low retry rates will have problems, but if they requested too often,
	 * they would get banned anyway.  Let the system regulate itself via chaos.
	 *		--RAM, 17/07/2003
	 */

	if (after < now + DOWNLOAD_SERVER_HOLD)
		after = now + DOWNLOAD_SERVER_HOLD;

	/*
	 * If server was given a "hold" period (e.g. requests to it were
	 * timeouting) then put it on hold now and reset the holding period.
	 */

	if (hold != 0)
		after = MAX(after, now + hold);

	if (server->retry_after != after) {
		dl_by_time_remove(server);
		server->retry_after = after;
		dl_by_time_insert(server);
	}
}

/*
 * download_reclaim_server
 *
 * Reclaim download's server if it is no longer holding anything.
 * If `delayed' is true, we're performing a batch free of downloads.
 */
static void download_reclaim_server(struct download *d, gboolean delayed)
{
	struct dl_server *server;

	g_assert(d);
	g_assert(d->server);
	g_assert(d->list_idx == -1);

	server = d->server;
	d->server = NULL;

	/*
	 * We cannot reclaim the server structure immediately if `delayed' is set,
	 * because we can be removing physically several downloads that all
	 * pointed to the same server, and which have all been removed from it.
	 * Therefore, the server structure appears empty but is still referenced.
	 */

	if (
		server->count[DL_LIST_RUNNING] == 0 &&
		server->count[DL_LIST_WAITING] == 0 &&
		server->count[DL_LIST_STOPPED] == 0
	) {
		if (delayed) {
			if (!(server->attrs & DLS_A_REMOVED)) {
				server->attrs |= DLS_A_REMOVED;		/* Insert once in list */
				sl_removed_servers =
					g_slist_prepend(sl_removed_servers, server);
			}
		} else
			free_server(server);
	}
}

/*
 * download_remove_from_server
 *
 * Remove download from server.
 * Reclaim server if this was the last download held and `reclaim' is true.
 */
static void download_remove_from_server(struct download *d, gboolean reclaim)
{
	struct dl_server *server;
	enum dl_list idx;

	g_assert(d);
	g_assert(d->server);
	g_assert(d->list_idx != -1);

	idx = d->list_idx;
	server = d->server;
	d->list_idx = -1;

	server->list[idx] = g_list_remove(server->list[idx], d);
	server->count[idx]--;

	g_assert(server->count[idx] >= 0);

	if (reclaim)
		download_reclaim_server(d, FALSE);
}

/*
 * download_reparent
 *
 * Move download from a server to another one.
 */
static void download_reparent(struct download *d, struct dl_server *new_server)
{
	enum dl_list list_idx;

	g_assert(d);
	g_assert(d->server);

	list_idx = d->list_idx;			/* Save index, before removal from server */
	download_remove_from_server(d, FALSE);	/* Server reclaimed later */
	download_reclaim_server(d, TRUE);		/* Delays free if empty */
	d->server = new_server;

	/*
	 * Insert download in new server, in the same list.
	 */

	d->list_idx = -1;			/* Pre-condition for download_add_to_list() */

	download_add_to_list(d, list_idx);
}

/*
 * download_redirect_to_server
 *
 * Move download from a server to another when the IP:port changed due
 * to a Location: redirection for instance, or because of a QUEUE callback.
 */
void download_redirect_to_server(struct download *d, guint32 ip, guint16 port)
{
	struct dl_server *server;
	gchar old_guid[16];
	enum dl_list list_idx;
	
	g_assert(d);
	g_assert(d->server);

	/*
	 * If neither the IP nor the port changed, do nothing.
	 */

	server = d->server;
	if (server->key->ip == ip && server->key->port == port)
		return;

	/*
	 * We have no way to know the GUID of the new IP:port server, so we
	 * reuse the old one.  We must save it before removing the download
	 * from the old server.
	 */

	list_idx = d->list_idx;			/* Save index, before removal from server */

	memcpy(old_guid, download_guid(d), 16);
	download_remove_from_server(d, TRUE);

	/*
	 * Create new server.
	 */

	server = get_server(old_guid, ip, port);
	if (server == NULL)
		server = allocate_server(old_guid, ip, port);
	d->server = server;

	/*
	 * Insert download in new server, in the same list.
	 */

	d->list_idx = -1;			/* Pre-condition for download_add_to_list() */

	download_add_to_list(d, list_idx);
}

/*
 * download_stop_v
 *
 * Vectorized version common to download_stop() and download_unavailable().
 */
static void download_stop_v(struct download *d, guint32 new_status,
				   const gchar * reason, va_list ap)
{
	gboolean store_queue = FALSE;		/* Shall we call download_store()? */
	enum dl_list list_target;

	g_assert(d);
	g_assert(!DOWNLOAD_IS_QUEUED(d));
	g_assert(!DOWNLOAD_IS_STOPPED(d));
	g_assert(d->status != new_status);
	g_assert(d->file_info);
	g_assert(d->file_info->refcount);

	if (d->status == GTA_DL_RECEIVING) {
		g_assert(d->file_info->recvcount > 0);
		g_assert(d->file_info->recvcount <= d->file_info->refcount);
		g_assert(d->file_info->recvcount <= d->file_info->lifecount);

		d->file_info->recvcount--;
		d->file_info->dirty_status = TRUE;
	}

	switch (new_status) {
	case GTA_DL_COMPLETED:
	case GTA_DL_ABORTED:
		list_target = DL_LIST_STOPPED;
		store_queue = TRUE;
		break;
	case GTA_DL_ERROR:
		list_target = DL_LIST_STOPPED;
		break;
	case GTA_DL_TIMEOUT_WAIT:
		list_target = DL_LIST_WAITING;
		break;
	default:
		g_error("unexpected new status %d !", new_status);
		return;
	}

	switch (new_status) {
	case GTA_DL_COMPLETED:
	case GTA_DL_ABORTED:
	case GTA_DL_ERROR:
		g_assert(d->file_info->lifecount <= d->file_info->refcount);
		g_assert(d->file_info->lifecount > 0);
		d->file_info->lifecount--;
		break;
	default:
		break;
	}

	if (reason) {
		gm_vsnprintf(d->error_str, sizeof(d->error_str), reason, ap);
		d->error_str[sizeof(d->error_str) - 1] = '\0';	/* May be truncated */
		d->remove_msg = d->error_str;
	} else
		d->remove_msg = NULL;

	if (d->file_desc != -1) {		/* Close output file */
		close(d->file_desc);
		d->file_desc = -1;
	}
	if (d->socket) {				/* Close socket */
		socket_free(d->socket);
		d->socket = NULL;
	}
	if (d->io_opaque) {				/* I/O data */
		io_free(d->io_opaque);
		g_assert(d->io_opaque == NULL);
	}
	if (d->bio) {
		bsched_source_remove(d->bio);
		d->bio = NULL;
	}
	if (d->req) {
		http_buffer_free(d->req);
		d->req = NULL;
	}
	if (d->cproxy) {
		cproxy_free(d->cproxy);
		d->cproxy = NULL;
	}

	/* Don't clear ranges if simply queuing, or if completed */

	if (d->ranges) {
		switch (new_status) {
		case GTA_DL_ERROR:
		case GTA_DL_ABORTED:
			http_range_free(d->ranges);
			d->ranges = NULL;
			break;
		default:
			break;
		}
	}

	if (d->list_idx != list_target)
		download_move_to_list(d, list_target);

	/* Register the new status, and update the GUI if needed */

	d->status = new_status;
	d->last_update = time((time_t *) NULL);

	if (d->status != GTA_DL_TIMEOUT_WAIT)
		d->retries = 0;		/* If they retry, go over whole cycle again */

	if (DOWNLOAD_IS_VISIBLE(d))
		gui_update_download(d, TRUE);

	if (store_queue)
		download_dirty = TRUE;		/* Refresh list, in case we crash */

	if (DOWNLOAD_IS_STOPPED(d) && DOWNLOAD_IS_IN_PUSH_MODE(d))
		download_push_remove(d);

	if (DOWNLOAD_IS_VISIBLE(d)) {
		gui_update_download_abort_resume();
		gui_update_download_clear();
	}

	file_info_clear_download(d, FALSE);
	d->flags &= ~DL_F_CHUNK_CHOSEN;

	download_actively_queued(d, FALSE);

	gnet_prop_set_guint32_val(PROP_DL_RUNNING_COUNT, count_running_downloads());
	gnet_prop_set_guint32_val(PROP_DL_ACTIVE_COUNT, dl_active);
}

/*
 * download_stop
 *
 * Stop an active download, close its socket and its data file descriptor.
 */
void download_stop(struct download *d, guint32 new_status,
				   const gchar * reason, ...)
{
	va_list args;

	d->unavailable = FALSE;

	va_start(args, reason);
	download_stop_v(d, new_status, reason, args);
	va_end(args);
}

/*
 * download_unavailable
 *
 * Like download_stop(), but flag the download as "unavailable".
 */
static void download_unavailable(struct download *d, guint32 new_status,
				   const gchar * reason, ...)
{
	va_list args;

	d->unavailable = TRUE;

	va_start(args, reason);
	download_stop_v(d, new_status, reason, args);
	va_end(args);
}

/*
 * download_queue_v
 *
 * The vectorized (message-wise) version of download_queue().
 */
static void download_queue_v(struct download *d, const gchar *fmt, va_list ap)
{
	g_assert(d);
	g_assert(!DOWNLOAD_IS_QUEUED(d));
	g_assert(d->file_info);
	g_assert(d->file_info->refcount > 0);
	g_assert(d->file_info->lifecount > 0);
	g_assert(d->file_info->lifecount <= d->file_info->refcount);
	g_assert(d->sha1 == NULL || d->file_info->sha1 == d->sha1);

	/*
	 * Put a download in the queue :
	 * - it's a new download, but we have reached the max number of
	 *	running downloads
	 * - the user requested it with the popup menu "Move back to the queue"
	 */

	if (fmt) {
		gm_vsnprintf(d->error_str, sizeof(d->error_str), fmt, ap);
		d->error_str[sizeof(d->error_str) - 1] = '\0';	/* May be truncated */
		/* d->remove_msg updated below */
	}

	if (DOWNLOAD_IS_VISIBLE(d))
		download_gui_remove(d);

	if (DOWNLOAD_IS_RUNNING(d))
		download_stop(d, GTA_DL_TIMEOUT_WAIT, NULL);
	else
		file_info_clear_download(d, TRUE);	/* Also done by download_stop() */

	/*
	 * Since download stop can change "d->remove_msg", update it now.
	 */

	d->remove_msg = fmt ? d->error_str: NULL;
	d->status = GTA_DL_QUEUED;

	g_assert(d->socket == NULL);

	if (d->list_idx != DL_LIST_WAITING)		/* Timeout wait is in "waiting" */
		download_move_to_list(d, DL_LIST_WAITING);

	sl_unqueued = g_slist_remove(sl_unqueued, d);

	gnet_prop_set_guint32_val(PROP_DL_QUEUE_COUNT, dl_queue_count + 1);
	if (d->flags & DL_F_REPLIED)
		gnet_prop_set_guint32_val(PROP_DL_QALIVE_COUNT, dl_qalive_count + 1);

	download_gui_add(d);
	gui_update_download(d, TRUE);
}

/*
 * download_queue
 *
 * Put download into queue.
 */
void download_queue(struct download *d, const gchar *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	download_queue_v(d, fmt, args);
	va_end(args);
}

/*
 * download_freeze_queue
 *
 * Freeze the scheduling queue. Multiple freezing requires
 * multiple thawing.
 */
void download_freeze_queue(void)
{
	queue_frozen++;
	gui_update_queue_frozen();
}

/*
 * download_thaw_queue
 *
 * Thaw the scheduling queue. Multiple freezing requires
 * multiple thawing.
 */
void download_thaw_queue(void)
{
	g_return_if_fail(queue_frozen > 0);

	queue_frozen--;
	gui_update_queue_frozen();
}

/*
 * download_queue_is_frozen
 *
 * Test whether download queue is frozen.
 */
gint download_queue_is_frozen(void)
{
	return queue_frozen;
}

/*
 * download_queue_hold_delay_v
 *
 * Common vectorized code for download_queue_delay() and download_queue_hold().
 */
static void download_queue_hold_delay_v(struct download *d,
	time_t delay, time_t hold,
	const gchar *fmt, va_list ap)
{
	time_t now = time((time_t *) NULL);

	/*
	 * Must update `retry_after' before enqueuing, since the "waiting" list
	 * is sorted by increasing retry_after for a given server.
	 */

	d->last_update = now;
	d->retry_after = now + delay;

	download_queue_v(d, fmt, ap);
	download_server_retry_after(d->server, now, hold);
}

/*
 * download_queue_delay
 *
 * Put download back to queue, but don't reconsider it for starting
 * before the next `delay' seconds. -- RAM, 03/09/2001
 */
static void download_queue_delay(struct download *d, guint32 delay,
	const gchar *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	download_queue_hold_delay_v(d, (time_t) delay, 0, fmt, args);
	va_end(args);
}

/*
 * download_queue_hold
 *
 * Same as download_queue_delay(), but make sure we don't consider
 * scheduling any currently queued download to this server before
 * the holding delay.
 */
static void download_queue_hold(struct download *d, guint32 hold,
	const gchar *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	download_queue_hold_delay_v(d, (time_t) hold, (time_t) hold, fmt, args);
	va_end(args);
}

/*
 * download_retry_no_urires
 *
 * If we sent a "GET /uri-res/N2R?" and we don't know the remote
 * server does not support it, then mark it here and retry as if we
 * got a 503 busy.
 *
 * `delay' is the Retry-After delay we got, 0 if none.
 * `ack_code' is the HTTP status code we got, 0 if none.
 *
 * Returns TRUE if we marked the download for retry.
 */
static gboolean download_retry_no_urires(struct download *d,
	gint delay, gint ack_code)
{
	/*
	 * Gtk-gnutella servers understand /uri-res.  Therefore, if we get an
	 * HTTP error after sending such a request, trust it (i.e. don't retry).
	 */

	if (0 == strncmp(download_vendor_str(d), "gtk-gnutella/", 13))
		return FALSE;

	if (!(d->server->attrs & DLS_A_NO_URIRES) && (d->flags & DL_F_URIRES)) {
		/*
		 * We sent /uri-res, and never marked server as not supporting it.
		 */

		d->server->attrs |= DLS_A_NO_URIRES;

		if (dbg > 3)
			printf("Server %s (%s) does not support /uri-res/N2R?\n",
				ip_port_to_gchar(download_ip(d), download_port(d)),
				download_vendor_str(d));

		download_passively_queued(d, FALSE);

		if (ack_code)
			download_queue_delay(d, delay ? delay : DOWNLOAD_SHORT_DELAY,
				"Server cannot handle /uri-res (%d)", ack_code);
		else
			download_queue_delay(d, DOWNLOAD_SHORT_DELAY,
				"Server cannot handle /uri-res (EOF)");

		return TRUE;
	}

	return FALSE;
}

/*
 * download_push_insert
 *
 * Record that we sent a push request for this download.
 */
static void download_push_insert(struct download *d)
{
	union {
		GSList *list;
		gpointer ptr;
	} value;
	gpointer key;
	gboolean found;

	g_assert(!d->push);

	gm_snprintf(dl_tmp, sizeof(dl_tmp), "%u:%s",
		d->record_index, guid_hex_str(download_guid(d)));

	/*
	 * We should not have the download already in the table, since we take care
	 * when starting a download that there is no (active) duplicate.  We also
	 * perform the same check on resuming a stopped download, so the following
	 * warning should not happen.  It will indicate a bug. --RAM, 01/01/2002
	 *
	 * However, it is possible that a servent updates its library, and that
	 * we get another query hit from that servent with a different file name
	 * but with the same index of a file we already recorded in the hash table.
	 * That is possible because we check for duplicate downloads based on
	 * the (name, GUID) tuple only.
	 *
	 * To overcome this, we have to store a list of downloads with the same
	 * key, and prepend newest ones, as being the ones with the "most accurate"
	 * index, supposedly. --RAM, 13/03/2002
	 */

	found = g_hash_table_lookup_extended(pushed_downloads, (gpointer) dl_tmp,
				&key, &value.ptr);

	if (!found) {
		value.list = g_slist_append(NULL, d);
		key = atom_str_get(dl_tmp);
		g_hash_table_insert(pushed_downloads, key, value.list);
	} else {
		GSList *l;

		if ((l = g_slist_find(value.list, d))) {
			struct download *ad = (struct download *) l->data;
			g_assert(ad != NULL);
			g_warning("BUG: duplicate push ignored for \"%s\"", ad->file_name);
			g_warning("BUG: argument is 0x%lx, \"%s\", key = %s, state = %d",
				(gulong) d, d->file_name, (gchar *) key, d->status);
			gm_snprintf(dl_tmp, sizeof(dl_tmp), "%u:%s",
				ad->record_index, guid_hex_str(download_guid(ad)));
			g_warning("BUG: in table has 0x%lx \"%s\", key = %s, state = %d",
				(gulong) ad, ad->file_name, dl_tmp, ad->status);
		} else {
			value.list = g_slist_prepend(value.list, d);
			g_hash_table_insert(pushed_downloads, key, value.list);
		}
	}

	d->push = TRUE;
}

/*
 * download_push_remove
 *
 * Forget that we sent a push request for this download.
 */
static void download_push_remove(struct download *d)
{
	union {
		GSList *list;
		gpointer ptr;
	} value;
	gpointer key;

	g_assert(d->push);

	gm_snprintf(dl_tmp, sizeof(dl_tmp), "%u:%s",
		d->record_index, guid_hex_str(download_guid(d)));

	if (
		g_hash_table_lookup_extended(pushed_downloads, (gpointer) dl_tmp,
			&key, &value.ptr)
	) {
		GSList *l = g_slist_find(value.list, d);

		/*
		 * Value `list' is a list of downloads that share the same key.
		 * We need to remove the entry in the hash table only when the
		 * last downlaod is removed from that list.
		 */

		if (l == NULL) {
			g_warning("BUG: push 0x%lx \"%s\" not found, key = %s, state = %d",
				(gulong) d, d->file_name, dl_tmp, d->status);
		} else {
			g_assert(l->data == (gpointer) d);
			value.list = g_slist_remove(value.list, d);
			if (value.list == NULL) {
				g_hash_table_remove(pushed_downloads, key);
				atom_str_free(key);
			} else
				g_hash_table_insert(pushed_downloads, key, value.list);
		}
	} else
		g_warning("BUG: tried to remove missing push %s", dl_tmp);

	d->push = FALSE;
}

/*
 * download_ignore_requested
 *
 * Check whether download should be ignored, and stop it immediately if it is.
 * Returns whether download was stopped (i.e. if it must be ignored).
 */
static gboolean download_ignore_requested(struct download *d)
{
	enum ignore_val reason = IGNORE_FALSE;
	struct dl_file_info *fi = d->file_info;

	/*
	 * Reject if we're trying to download from ourselves (could happen
	 * if someone echoes back our own alt-locs to us with PFSP).
	 */

	if (download_ip(d) == listen_ip() && download_port(d) == listen_port)
		reason = IGNORE_OURSELVES;
	else if (hostiles_check(download_ip(d)))
		reason = IGNORE_HOSTILE;

	if (reason == IGNORE_FALSE)
		reason = ignore_is_requested(fi->file_name, fi->size, fi->sha1);

	if (reason != IGNORE_FALSE) {
		if (!DOWNLOAD_IS_VISIBLE(d))
			download_gui_add(d);

		download_stop(d, GTA_DL_ERROR, "Ignoring requested (%s)",
			reason == IGNORE_OURSELVES ? "Points to ourselves" :
			reason == IGNORE_HOSTILE ? "Hostile IP" :
			reason == IGNORE_SHA1 ? "SHA1" :
			reason == IGNORE_LIBRARY ? "Already Owned" : "Name & Size");

		/*
		 * If we're ignoring this file, make sure we don't keep any
		 * track of it on disk: dispose of the fileinfo when the last
		 * reference will be removed, remove all known downloads from the
		 * queue and delete the file (if not complete, or it could be in
		 * the process of being moved).
		 */

		switch (reason) {
		case IGNORE_HOSTILE:
		case IGNORE_OURSELVES:
			break;
		default:
			file_info_set_discard(d->file_info, TRUE);
			queue_remove_downloads_with_file(fi, d);
			if (!FILE_INFO_COMPLETE(fi))
				download_remove_file(d, FALSE);
			break;
		}

		return TRUE;
	}

	return FALSE;
}

/*
 * download_unqueue
 *
 * Remove download from queue.
 * It is put in a state where it can be stopped if necessary.
 */
static void download_unqueue(struct download *d)
{
	g_assert(d);
	g_assert(DOWNLOAD_IS_QUEUED(d));
	g_assert(dl_queue_count > 0);

	if (DOWNLOAD_IS_VISIBLE(d))
		download_gui_remove(d);

	sl_unqueued = g_slist_prepend(sl_unqueued, d);
	gnet_prop_set_guint32_val(PROP_DL_QUEUE_COUNT, dl_queue_count - 1);

	if (d->flags & DL_F_REPLIED)
		gnet_prop_set_guint32_val(PROP_DL_QALIVE_COUNT, dl_qalive_count - 1);

	g_assert(dl_qalive_count >= 0);

	d->status = GTA_DL_CONNECTING;		/* Allow download to be stopped */
}

/*
 * download_start_prepare_running
 *
 * Setup the download structure with proper range offset, and check that the
 * download is not otherwise completed.
 *
 * Returns TRUE if we may continue with the download, FALSE if it has been
 * stopped due to a problem.
 */
gboolean download_start_prepare_running(struct download *d)
{
	struct dl_file_info *fi = d->file_info;

	g_assert(d != NULL);
	g_assert(!DOWNLOAD_IS_QUEUED(d));
	g_assert(d->list_idx == DL_LIST_RUNNING);
	g_assert(fi != NULL);
	g_assert(fi->lifecount > 0);

	d->status = GTA_DL_CONNECTING;	/* Most common state if we succeed */

	/*
	 * If we were asked to ignore this download, abort now.
	 */

	if (download_ignore_requested(d))
		return FALSE;

	/*
	 * Even though we should not schedule a "suspended" download, we could
	 * be asked via a user-event to start such a download.
	 */

	if (d->flags & DL_F_SUSPENDED) {
		download_queue(d, _("Suspended (SHA1 checking)"));
		return FALSE;
	}

	/*
	 * If the file already exists, and has less than `download_overlap_range'
	 * bytes, we restart the download from scratch.	Otherwise, we request
	 * that amount before the resuming point.
	 * Later on, in download_write_data(), and as soon as we have read more
	 * than `download_overlap_range' bytes, we'll check for a match.
	 *		--RAM, 12/01/2002
	 */

	d->skip = 0;			/* We're setting it here only if not swarming */
	d->keep_alive = FALSE;	/* Until proven otherwise by server's reply */
	d->got_giv = FALSE;		/* Don't know yet, assume no GIV */

	d->flags &= ~DL_F_OVERLAPPED;		/* Clear overlapping indication */
	d->flags &= ~DL_F_SHRUNK_REPLY;		/* Clear server shrinking indication */

	/*
	 * If this file is swarming, the overlapping size and skipping offset
	 * will be determined before making the requst, in download_pick_chunk().
	 *		--RAM, 22/08/2002.
	 */

	if (!fi->use_swarming) {
		if (fi->done > download_overlap_range)
			d->skip = fi->done;		/* Not swarming => file has no holes */
		d->pos = d->skip;
		d->overlap_size = (d->skip == 0 || d->size <= d->pos) ?
			0 : download_overlap_range;

		g_assert(d->overlap_size == 0 || d->skip > d->overlap_size);
	}

	d->last_update = time((time_t *) NULL);

	/*
	 * Is there anything to get at all?
	 */

	if (FILE_INFO_COMPLETE(fi)) {
		if (!DOWNLOAD_IS_VISIBLE(d))
			download_gui_add(d);
		download_stop(d, GTA_DL_ERROR, "Nothing more to get");
		download_verify_sha1(d);
		return FALSE;
	}

	return TRUE;
}

/*
 * download_start_prepare
 *
 * Make download a "running" one (in running list, unqueued), then call
 * download_start_prepare_running().
 *
 * Returns TRUE if we may continue with the download, FALSE if it has been
 * stopped due to a problem.
 */
gboolean download_start_prepare(struct download *d)
{
	g_assert(d != NULL);
	g_assert(d->list_idx != DL_LIST_RUNNING);	/* Not already running */

	/*
	 * Updata global accounting data.
	 */

	download_move_to_list(d, DL_LIST_RUNNING);

	/*
	 * If the download is in the queue, we remove it from there.
	 */

	if (DOWNLOAD_IS_QUEUED(d))
		download_unqueue(d);

	/*
	 * Reset flags that must be cleared only once per session, i.e. when
	 * we start issuing requests for a queued download, or after we cloned
	 * a completed download.
	 *
	 * Since download_start_prepare_running() is called from download_request(),
	 * we must reset DL_F_SUNK_DATA here, since we want to sink only ONCE
	 * per session.
	 */

	d->flags &= ~DL_F_SUNK_DATA;		/* Restarting, nothing sunk yet */

	return download_start_prepare_running(d);
}

/*
 * download_pick_chunk
 *
 * Called for swarming downloads when we are connected to the remote server,
 * but before making the request, to pick up a chunk for downloading.
 *
 * Returns TRUE if we can continue with the download, FALSE if it has
 * been stopped.
 */
static gboolean download_pick_chunk(struct download *d)
{
	enum dl_chunk_status status;
	guint32 from, to;

	g_assert(d->file_info->use_swarming);

	d->overlap_size = 0;
	d->last_update = time((time_t *) NULL);

	status = file_info_find_hole(d, &from, &to);

	if (status == DL_CHUNK_EMPTY) {

		d->skip = d->pos = from;
		d->size = to - from;

		if (
			from > download_overlap_range &&
			file_info_chunk_status(d->file_info, 
				from - download_overlap_range, from) == DL_CHUNK_DONE
		)
			d->overlap_size = download_overlap_range;

	} else if (status == DL_CHUNK_BUSY) {

		download_queue_delay(d, 10, "Waiting for a free chunk");
		return FALSE;

	} else if (status == DL_CHUNK_DONE) {

		if (!DOWNLOAD_IS_VISIBLE(d))
			download_gui_add(d);

		download_stop(d, GTA_DL_ERROR, "No more gaps to fill");
		queue_remove_downloads_with_file(d->file_info, d);
		return FALSE;
	}

	g_assert(d->overlap_size == 0 || d->skip > d->overlap_size);

	return TRUE;
}

/*
 * download_pick_available
 *
 * Pickup a range we don't have yet from the available ranges.
 *
 * Returns TRUE if we selected a chunk, FALSE if we can't select a chunk
 * (e.g. we have everything the remote server makes available).
 */
static gboolean download_pick_available(struct download *d)
{
	guint32 from, to;

	g_assert(d->ranges != NULL);


	d->overlap_size = 0;
	d->last_update = time((time_t *) NULL);

	if (!file_info_find_available_hole(d, d->ranges, &from, &to)) {
		if (dbg > 3)
			printf("PFSP no interesting chunks from %s for \"%s\", "
				"available was: %s\n",
				ip_port_to_gchar(download_ip(d), download_port(d)),
				download_outname(d), http_range_to_gchar(d->ranges));

		return FALSE;
	}

	/*
	 * We found a chunk that the remote end has and which we miss.
	 */

	d->skip = d->pos = from;
	d->size = to - from;

	/*
	 * Maybe we can do some overlapping check if the remote server has
	 * some data before that chunk and we also have the corresponding
	 * range.
	 */

	if (
		from > download_overlap_range &&
		file_info_chunk_status(d->file_info, 
			from - download_overlap_range, from) == DL_CHUNK_DONE &&
		http_range_contains(d->ranges, from - download_overlap_range, from - 1)
	)
		d->overlap_size = download_overlap_range;

	if (dbg > 3)
		printf("PFSP selected %u-%u (overlap=%u) from %s for \"%s\", "
			"available was: %s\n", from, to - 1, d->overlap_size,
			ip_port_to_gchar(download_ip(d), download_port(d)),
			download_outname(d), http_range_to_gchar(d->ranges));
			
	return TRUE;
}

/*
 * download_bad_source
 *
 * Indicates that this download source is not good enough for us: it is either
 * non-connectible, does not allow resuming, etc...  Remove it from the mesh.
 */
static void download_bad_source(struct download *d)
{
	download_passively_queued(d, FALSE);

	if (!d->always_push && d->sha1)
		dmesh_remove(d->sha1, download_ip(d), download_port(d),
			d->record_index, d->file_name);
}

/*
 * download_connect
 *
 * Establish asynchronous connection to remote server.
 * Returns connecting socket.
 */
static struct gnutella_socket *download_connect(struct download *d)
{
	struct dl_server *server = d->server;
	guint16 port = download_port(d);

	g_assert(server != NULL);

	d->flags &= ~DL_F_DNS_LOOKUP;

	/*
	 * If there is a fully qualified domain name, look it up for possible
	 * change if either sufficient time passed since last lookup, or if the
	 * DLS_A_DNS_LOOKUP attribute was set because of a connection failure.
	 */

	if (
		(server->attrs & DLS_A_DNS_LOOKUP) ||
		(server->hostname != NULL &&
			time(NULL) - server->dns_lookup > DOWNLOAD_DNS_LOOKUP)
	) {
		g_assert(server->hostname != NULL);

		d->flags |= DL_F_DNS_LOOKUP;
		server->attrs &= ~DLS_A_DNS_LOOKUP;
		server->dns_lookup = time(NULL);
		return socket_connect_by_name(
			server->hostname, port, SOCK_TYPE_DOWNLOAD);
	} else
		return socket_connect(download_ip(d), port, SOCK_TYPE_DOWNLOAD);
}

/* (Re)start a stopped or queued download */

void download_start(struct download *d, gboolean check_allowed)
{
	guint32 ip = download_ip(d);
	guint16 port = download_port(d);

	g_assert(d);
	g_assert(d->list_idx != DL_LIST_RUNNING);	/* Waiting or stopped */
	g_assert(d->file_info);
	g_assert(d->file_info->refcount > 0);
	g_assert(d->file_info->lifecount > 0);
	g_assert(d->file_info->lifecount <= d->file_info->refcount);
	g_assert(d->sha1 == NULL || d->file_info->sha1 == d->sha1);

	/*
	 * If caller did not check whether we were allowed to start downloading
	 * this file, do it now. --RAM, 03/09/2001
	 */

	if (check_allowed && (
		count_running_downloads() >= max_downloads ||
		count_running_on_server(d->server) >= max_host_downloads ||
		(!d->file_info->use_swarming &&
			count_running_downloads_with_name(download_outname(d)) != 0))
	) {
		if (!DOWNLOAD_IS_QUEUED(d))
			download_queue(d, _("No download slot"));
		return;
	}

	if (!download_start_prepare(d))
		return;

	g_assert(d->list_idx == DL_LIST_RUNNING);	/* Moved to "running" list */
	g_assert(d->file_info->refcount > 0);		/* Still alive */
	g_assert(d->file_info->lifecount > 0);
	g_assert(d->file_info->lifecount <= d->file_info->refcount);

	if ((is_firewalled || !send_pushes) && d->push)
		download_push_remove(d);

	/*
	 * If server is known to be reachable without pushes, reset the flag.
	 */

	if (d->always_push && (d->server->attrs & DLS_A_PUSH_IGN)) {
		g_assert(host_is_valid(ip, port));	/* Or would not have set flag */
		if (d->push)
			download_push_remove(d);
		d->always_push = FALSE;
	}

	if (!DOWNLOAD_IS_IN_PUSH_MODE(d) && host_is_valid(ip, port)) {
		/* Direct download */
		d->status = GTA_DL_CONNECTING;
		d->socket = download_connect(d);

		if (!DOWNLOAD_IS_VISIBLE(d))
			download_gui_add(d);

		if (!d->socket) {
			/*
			 * If DNS lookup was attempted, and we fail immediately, it
			 * means either the address returned by the DNS was invalid or
			 * there was no successful (synchronous) resolution for this
			 * host.
			 */

			if (d->flags & DL_F_DNS_LOOKUP) {
				atom_str_free(d->server->hostname);
				d->server->hostname = NULL;
				gui_update_download_host(d);
			}

			download_unavailable(d, GTA_DL_ERROR, "Connection failed");
			return;
		}

		d->socket->resource.download = d;
		d->socket->pos = 0;
	} else {					/* We have to send a push request */
		d->status = GTA_DL_PUSH_SENT;

		g_assert(d->socket == NULL);

		if (!DOWNLOAD_IS_VISIBLE(d))
			download_gui_add(d);

		download_push(d, FALSE);
	}

	gnet_prop_set_guint32_val(PROP_DL_RUNNING_COUNT, count_running_downloads());

	gui_update_download(d, TRUE);
	gnet_prop_set_guint32_val(PROP_DL_ACTIVE_COUNT, dl_active);
}

/* pick up new downloads from the queue as needed */

void download_pickup_queued(void)
{
	time_t now = time((time_t *) NULL);
	gint running = count_running_downloads();
	gint i;

	/*
	 * To select downloads, we iterate over the sorted `dl_by_time' list and
	 * look for something we could schedule.
	 *
	 * Note that we jump from one host to the other, even if we have multiple
	 * things to schedule on the same host: It's better to spread load among
	 * all hosts first.
	 */

	for (
		i = 0;
		i < DHASH_SIZE && running < max_downloads &&
			bws_can_connect(SOCK_TYPE_DOWNLOAD);
		i++
	) {
		GList *l;
		gint last_change;

	retry:
		l = dl_by_time.servers[i];
		last_change = dl_by_time.change[i];

		for (/* empty */; l && running < max_downloads; l = g_list_next(l)) {
			struct dl_server *server = (struct dl_server *) l->data;
			GList *w;

			/*
			 * List is sorted, so as soon as we go beyond the current time,
			 * we can stop.
			 */

			if (server->retry_after > now)
				break;

			if (
				server->count[DL_LIST_WAITING] == 0 ||
				count_running_on_server(server) >= max_host_downloads
			)
				continue;

			/*
			 * OK, pick the download at the start of the waiting list, but
			 * do not remove it yet.  This will be done by download_start().
			 */

			g_assert(server->list[DL_LIST_WAITING]);	/* Since count != 0 */

			for (w = server->list[DL_LIST_WAITING]; w; w = g_list_next(w)) {
				struct download *d = (struct download *) w->data;

				if (
					!d->file_info->use_swarming &&
					count_running_downloads_with_name(download_outname(d)) != 0
				)
					continue;

				if ((now - d->last_update) <= d->timeout_delay)
					continue;

				if (now < d->retry_after)
					break;	/* List is sorted */

				if (d->flags & DL_F_SUSPENDED)
					continue;

				download_start(d, FALSE);

				if (DOWNLOAD_IS_RUNNING(d))
					running++;

				break;		/* Don't schedule all files on same host at once */
			}

			/*
			 * It's possible that download_start() ended-up changing the
			 * dl_by_time list we're iterating over.  That's why all changes
			 * to that list update the dl_by_time_change variable, which we
			 * snapshot upon entry into the loop.
			 *		--RAM, 24/08/2002.
			 */

			if (last_change != dl_by_time.change[i])
				goto retry;
		}
	}

	/*
	 * Enable "Start now" only if we would not exceed limits.
	 */

#ifdef USE_GTK2
	
	gtk_widget_set_sensitive(lookup_widget
		(popup_queue, "popup_queue_start_now"), (running < max_downloads));	
	
#else
		
	gtk_widget_set_sensitive(
		lookup_widget(popup_queue, "popup_queue_start_now"), 
		(running < max_downloads) &&
		GTK_CLIST(
			lookup_widget(main_window, "ctree_downloads_queue"))->selection); 
#endif
}

static void download_push(struct download *d, gboolean on_timeout)
{
	gboolean ignore_push = FALSE;

	g_assert(d);

	if (
		(d->flags & DL_F_PUSH_IGN) ||
		(d->server->attrs & DLS_A_PUSH_IGN) ||
		has_blank_guid(d)
	)
		ignore_push = TRUE;

	if (is_firewalled || !send_pushes || ignore_push) {
		if (d->push)
			download_push_remove(d);
		goto attempt_retry;
	}

	/*
	 * The push request is sent with the listening port set to our Gnet port.
	 *
	 * To be able to later distinguish which download is referred to by each
	 * GIV we'll receive back, we record the association file_index/guid of
	 * the to-be-downloaded file with this download into a hash table.
	 * When stopping a download for which d->push is true, we'll have to
	 * remove the mapping.
	 *
	 *		--RAM, 30/12/2001
	 */

	if (!d->push)
		download_push_insert(d);

	g_assert(d->push);

	/*
	 * Before sending a push on Gnet, look whether we have some push-proxies
	 * available for the server.
	 *		--RAM, 18/07/2003
	 */

	if (use_push_proxy(d))
		return;

	if (send_push_request(download_guid(d), d->record_index, listen_port))
		return;

	if (!d->always_push) {
		download_push_remove(d);
		goto attempt_retry;
	} else {
		/*
		 * If the address is not a private IP, it is possible that the
		 * servent set the "Push" flag incorrectly.
		 *		-- RAM, 18/08/2002.
		 */

		if (!host_is_valid(download_ip(d), download_port(d))) {
			download_unavailable(d, GTA_DL_ERROR, "Push route lost");
			download_remove_all_from_peer(
				download_guid(d), download_ip(d), download_port(d), TRUE);
		} else {
			/*
			 * Later on, if we manage to connect to the server, we'll
			 * make sure to mark it so that we ignore pushes to it, and
			 * we will clear the `always_push' indication.
			 * (see download_send_request() for more information)
			 */

			download_push_remove(d);

			if (dbg > 2)
				printf("PUSH trying to ignore them for %s\n",
					ip_port_to_gchar(download_ip(d), download_port(d)));

			d->flags |= DL_F_PUSH_IGN;
			download_queue(d, _("Ignoring Push flag"));
		}
	}

	return;

attempt_retry:
	/*
	 * If we're aborting a download flagged with "Push ignore" due to a
	 * timeout reason, chances are great that this host is indeed firewalled!
	 * Tell them so. -- RAM, 18/08/2002.
	 */

	if (
		d->always_push &&						/* Normally requires a push */
		(d->flags & DL_F_PUSH_IGN) &&			/* Started to ignore pushes */
		!(d->server->attrs & DLS_A_PUSH_IGN)	/* But never connected yet */
	) {
		d->retries++;
		if (on_timeout || d->retries > 5) {
			/*
			 * Looks like we won't be able to ever reach this host.
			 * Abort the download, and remove all the ones for the same host.
			 */

			download_unavailable(d, GTA_DL_ERROR,
				"Can't reach host (Push or Direct)");
			download_remove_all_from_peer(
				download_guid(d), download_ip(d), download_port(d), TRUE);
		} else
			download_queue_hold(d, download_retry_refused_delay,
				"No direct connection yet (%d retr%s)",
				d->retries, d->retries == 1 ? "y" : "ies");
	} else if (d->retries < download_max_retries) {
		d->retries++;
		if (on_timeout)
			download_queue_hold(d, download_retry_timeout_delay,
				"Timeout (%d retr%s)",
				d->retries, d->retries == 1 ? "y" : "ies");
		else
			download_queue_hold(d, download_retry_refused_delay,
				"Connection refused (%d retr%s)",
				d->retries, d->retries == 1 ? "y" : "ies");
	} else {
		/*
		 * Looks like this host is down.  Abort the download, and remove all
		 * the ones queued for the same host.
		 */

		download_unavailable(d, GTA_DL_ERROR, "Timeout (%d retr%s)",
				d->retries, d->retries == 1 ? "y" : "ies");

		download_remove_all_from_peer(
			download_guid(d), download_ip(d), download_port(d), TRUE);
	}

	/*
	 * Remove this source from mesh, since we don't seem to be able to
	 * connect to it properly.
	 */

	download_bad_source(d);
}

/* Direct download failed, let's try it with a push request */

void download_fallback_to_push(struct download *d,
	gboolean on_timeout, gboolean user_request)
{
	g_return_if_fail(d);

	if (DOWNLOAD_IS_QUEUED(d)) {
		g_warning
			("download_fallback_to_push() called on a queued download !?!");
		return;
	}

	if (DOWNLOAD_IS_STOPPED(d))
		return;

	if (!d->socket)
		g_warning("download_fallback_to_push(): no socket for '%s'",
				  d->file_name);
	else {
		/*
		 * If a DNS lookup error occurred, discard the hostname we have.
		 * Due to the async nature of the DNS lookups, we must check for
		 * a non-NULL hostname, in case we already detected it earlier for
		 * this server, in another connection attempt.
		 *
		 * XXX we should allow for DNS failure and mark the hostname bad
		 * XXX for a while only, then re-attempt periodically, instead of
		 * XXX simply discarding it.
		 */

		if (socket_bad_hostname(d->socket) && d->server->hostname != NULL) {
			g_warning("hostname \"%s\" for %s could not resolve, discarding",
				d->server->hostname,
				ip_port_to_gchar(download_ip(d), download_port(d)));
			atom_str_free(d->server->hostname);
			d->server->hostname = NULL;
			gui_update_download_host(d);
		}

		/*
		 * If we could not connect to the host, but we have a hostname and
		 * we did not perform a DNS lookup this time, request one for the
		 * next attempt.
		 */

		if (d->server->hostname != NULL && !(d->flags & DL_F_DNS_LOOKUP))
			d->server->attrs |= DLS_A_DNS_LOOKUP;

		socket_free(d->socket);
		d->socket = NULL;
	}

	if (d->file_desc != -1) {
		close(d->file_desc);
		d->file_desc = -1;
	}

	if (user_request)
		d->status = GTA_DL_PUSH_SENT;
	else
		d->status = GTA_DL_FALLBACK;

	d->last_update = time(NULL);		/* Reset timeout if we send the push */
	download_push(d, on_timeout);

	gui_update_download(d, TRUE);
}

/*
 * Downloads creation and destruction
 */

/*
 * create_download
 * 
 * Create a new download
 *
 * When `interactive' is false, we assume that `file' was already duped,
 * and take ownership of the pointer.
 *
 * NB: If `record_index' == 0, and a `sha1' is also supplied, then
 * this is our convention for expressing a /uri-res/N2R? download URL.
 * However, we don't forbid 0 as a valid record index if it does not
 * have a SHA1.
 *
 * Returns created download structure, or NULL if none.
 */
static struct download *create_download(
	gchar *file, guint32 size, guint32 record_index,
	guint32 ip, guint16 port, gchar *guid, gchar *hostname,
	gchar *sha1, time_t stamp,
	gboolean push, gboolean interactive, struct dl_file_info *file_info,
	gnet_host_vec_t *proxies)
{
	struct dl_server *server;
	struct download *d;
	gchar *file_name = interactive ? atom_str_get(file) : file;
	struct dl_file_info *fi;
	gboolean server_created = FALSE;		/* For assertions only */

	/*
	 * Reject if we're trying to download from ourselves (could happen
	 * if someone echoes back our own alt-locs to us with PFSP).
	 */

	if (ip == listen_ip() && port == listen_port) {
		atom_str_free(file_name);
		return NULL;
	}

	/*
	 * Create server if none exists already.
	 */

	server = get_server(guid, ip, port);
	if (server == NULL) {
		server = allocate_server(guid, ip, port);
		server_created = TRUE;
	}

	/*
	 * If some push proxies are given, and provided the `stamp' argument
	 * is recent enough, drop the existing list and replace it with the
	 * one coming from the query hit.
	 */

	if (proxies != NULL && stamp > server->proxies_stamp) {
		if (server->proxies)
			free_proxies(server);
		server->proxies = hostvec_to_slist(proxies);
		server->proxies_stamp = stamp;
	}

	/*
	 * Refuse to queue the same download twice. --RAM, 04/11/2001
	 */

	if ((d = has_same_download(file_name, sha1, guid, ip, port))) {
		g_assert(!server_created);		/* Obviously! */
		if (interactive)
			g_warning("rejecting duplicate download for %s", file_name);
		atom_str_free(file_name);
		return NULL;
	}

	/*
	 * Initialize download.
	 */

	d = (struct download *) walloc0(sizeof(struct download));

	d->src_handle = idtable_new_id(src_handle_map, d);
	d->server = server;
	d->list_idx = -1;

	/*
	 * If we know that this server can be directly connected to, ignore
	 * the push flag. --RAM, 18/08/2002.
	 */

	if (d->server->attrs & DLS_A_PUSH_IGN)
		push = FALSE;

	d->file_name = file_name;
	d->escaped_name = url_escape_cntrl(file_name);

	/*
	 * Note: size and skip will be filled by download_pick_chunk() later
	 * if we use swarming.
	 */

	d->file_size = size;			/* Never changes */
	d->size = size;					/* Will be changed if range requested */
	d->record_index = record_index;
	d->file_desc = -1;
	d->always_push = push;
	if (sha1)
		d->sha1 = atom_sha1_get(sha1);
	if (push)
		download_push_insert(d);
	else
		d->push = FALSE;
	d->record_stamp = stamp;

	/*
	 * If fileinfo is maked with FI_F_SUSPEND, it means we are in the process
	 * of verifying the SHA1 of the download.  If it matches with the SHA1
	 * we got initially, we'll remove the downloads, otherwise we will
	 * restart it.
	 *
	 * That's why we still accept downloads for that fileinfo, but do not
	 * schedule them: we wait for the outcome of the SHA1 verification process.
	 */

	fi = file_info == NULL ?
		file_info_get(file_name, save_file_path, size, sha1) : file_info;

	if (fi->flags & FI_F_SUSPEND)
		d->flags |= DL_F_SUSPENDED;

	file_info_add_source(fi, d);
	fi->lifecount++;

	download_add_to_list(d, DL_LIST_WAITING);
	sl_downloads = g_slist_prepend(sl_downloads, d);
	sl_unqueued = g_slist_prepend(sl_unqueued, d);

	download_dirty = TRUE;			/* Refresh list, in case we crash */

	/*
	 * Record server's hostname if non-NULL and not empty.
	 */

	if (hostname != NULL && *hostname != '\0')
		set_server_hostname(d->server, hostname);

	/*
	 * Insert in download mesh if it does not require a push and has a SHA1.
	 */

	if (!d->always_push && d->sha1)
		dmesh_add(d->sha1, ip, port, record_index, file_name, stamp);

	/*
	 * When we know our SHA1, if we don't have a SHA1 in the `fi' and we
	 * looked for it, it means that they didn't have "strict_sha1_matching"
	 * at some point in time.
	 *
	 * If we have a SHA1, it must match.
	 */

	if (d->sha1 != NULL && fi->sha1 == NULL) {
		gboolean success = file_info_got_sha1(fi, d->sha1);
		if (success) {
			g_warning("forced SHA1 %s after %u byte%s downloaded for %s",
				sha1_base32(d->sha1), fi->done, fi->done == 1 ? "" : "s",
				download_outname(d));
			if (DOWNLOAD_IS_QUEUED(d))		/* file_info_got_sha1() can queue */
				return d;
		} else {
			download_info_reget(d);
			download_queue(d, _("Dup SHA1 during creation"));
			return d;
		}
	}

	g_assert(d->sha1 == NULL || d->file_info->sha1 == d->sha1);


	if (d->flags & DL_F_SUSPENDED)
		download_queue(d, _("Suspended (SHA1 checking)"));
	else if (
		count_running_downloads() < max_downloads &&
		count_running_on_server(d->server) < max_host_downloads &&
		(d->file_info->use_swarming ||
			count_running_downloads_with_name(download_outname(d)) == 0)
	) {
		download_start(d, FALSE);		/* Start the download immediately */
	} else {
		/* Max number of downloads reached, we have to queue it */
		download_queue(d, _("No download slot"));
	}

	return d;
}


/* Automatic download request */

void download_auto_new(gchar *file, guint32 size, guint32 record_index,
					guint32 ip, guint16 port, gchar *guid, gchar *hostname,
					gchar *sha1, time_t stamp, gboolean push,
					struct dl_file_info *fi, gnet_host_vec_t *proxies)
{
	gchar *file_name;
	const char *reason;
	enum ignore_val ign_reason;

	/*
	 * Make sure we're not prevented from downloading that file.
	 */

	ign_reason = ignore_is_requested(
		fi ? fi->file_name : file,
		fi ? fi->size : size,
		fi ? fi->sha1 : sha1);

	switch (ign_reason) {
	case IGNORE_FALSE:
		break;
	case IGNORE_SHA1:
		reason = "ignore by SHA1 requested";
		goto abort_download;
	case IGNORE_NAMESIZE:
		reason = "ignore by name & size requested";
		goto abort_download;
	case IGNORE_LIBRARY:
		reason = "SHA1 is already in library";
		goto abort_download;
	default:
		g_error("ignore_is_requested() returned unexpected %d", ign_reason);
	}

	/*
	 * Create download.
	 */

	file_name = atom_str_get(file);

	(void) create_download(file_name, size, record_index, ip, port,
		guid, hostname, sha1, stamp, push, FALSE, fi, proxies);
	return;

abort_download:
	if (dbg > 4)
		printf("ignoring auto download for \"%s\": %s\n", file, reason);
	return;
}

/*
 * download_clone
 *
 * Clone download, resetting most dynamically allocated structures in the
 * original since they are shallow-copied to the new download.
 *
 * (This routine is used because each different download from the same host
 * will become a line in the GUI, and the GUI stores download structures in
 * ts row data, expecting a one-to-one mapping between a download and the GUI).
 */
static struct download *download_clone(struct download *d)
{
	struct download *cd = walloc0(sizeof(struct download));
	struct dl_file_info *fi;

	g_assert(!(d->flags & (DL_F_ACTIVE_QUEUED|DL_F_PASSIVE_QUEUED)));

	fi = d->file_info;

	*cd = *d;						/* Struct copy */
	cd->src_handle = idtable_new_id(src_handle_map, cd); /* new handle */
	cd->file_info = NULL;			/* has not been added to fi sources list */
	file_info_add_source(fi, cd);	/* add clonded source */

	g_assert(d->io_opaque == NULL);		/* If cloned, we were receiving! */

	cd->bio = NULL;						/* Recreated on each transfer */
	cd->file_desc = -1;					/* File re-opened each time */
	cd->socket->resource.download = cd;	/* Takes ownership of socket */
	cd->file_info->lifecount++;			/* Both are still "alive" for now */
	cd->list_idx = -1;
	cd->file_name = atom_str_get(d->file_name);
	cd->visible = FALSE;
	cd->push = FALSE;
	cd->status = GTA_DL_CONNECTING;

	if (d->escaped_name == d->file_name)
		cd->escaped_name = cd->file_name;
	else
		cd->escaped_name = url_escape_cntrl(cd->file_name);

	download_add_to_list(cd, DL_LIST_WAITING);

	sl_downloads = g_slist_prepend(sl_downloads, cd);
	sl_unqueued = g_slist_prepend(sl_unqueued, cd);

	if (d->push) {
		download_push_remove(d);
		download_push_insert(cd);
	}

	if (d->queue_status != NULL)
		parq_dl_reparent_id(d, cd);

	if (d->cproxy != NULL)
		cproxy_reparent(d, cd);
	
	g_assert(d->queue_status == NULL);	/* Cleared by parq_dl_reparent_id() */

	/*
	 * The following have been copied and appropriated by the cloned download.
	 * They are reset so that a download_free() on the original will not
	 * free them.
	 */

	d->sha1 = NULL;
	d->socket = NULL;
	d->ranges = NULL;
	
	return cd;
}

/* search has detected index change in queued download --RAM, 18/12/2001 */

void download_index_changed(guint32 ip, guint16 port, gchar *guid,
	guint32 from, guint32 to)
{
	struct dl_server *server = get_server(guid, ip, port);
	GList *l;
	gint nfound = 0;
	GSList *to_stop = NULL;
	GSList *sl;
	gint n;
	enum dl_list listnum[] = { DL_LIST_RUNNING, DL_LIST_WAITING };

	if (!server)
		return;

	for (n = 0; n < G_N_ELEMENTS(listnum); n++) {
		for (l = server->list[n]; l; l = g_list_next(l)) {
			struct download *d = (struct download *) l->data;
			gboolean push_mode;
			g_assert(d != NULL);

			if (d->record_index != from)
				continue;

			push_mode = d->push;

			/*
			 * When in push mode, we've recorded the index in a hash table,
			 * associating the GIV string to the download structure.
			 * If that index changes, we need to remove the old mapping before
			 * operating the change, and re-install the new mapping after
			 * then change took place.
			 */

			if (push_mode)
				download_push_remove(d);

			d->record_index = to;
			nfound++;

			if (push_mode)
				download_push_insert(d);

			switch (d->status) {
			case GTA_DL_REQ_SENT:
			case GTA_DL_HEADERS:
			case GTA_DL_PUSH_SENT:
				/*
				 * We've sent a request with possibly the wrong index.
				 * We can't know for sure, but it's safer to stop it, and
				 * restart it in a while.  Sure, we might loose the download
				 * slot, but we might as well have gotten a wrong file.
				 *
				 * NB: this can't happen when the remote peer is gtk-gnutella
				 * since we check the matching between the index and the file
				 * name, but some peers might not bother.
				 */
				g_warning("Stopping request for '%s': index changed",
					d->file_name);
				to_stop = g_slist_prepend(to_stop, d);
				break;
			case GTA_DL_RECEIVING:
				/*
				 * Ouch.  Pray and hope that the change occurred after we
				 * requested the file.	There's nothing we can do now.
				 */
				g_warning("Index of '%s' changed during reception",
					d->file_name);
				break;
			default:
				/*
				 * Queued or other state not needing special notice
				 */
				if (dbg > 3)
					printf("Noted index change from %u to %u at %s for %s",
						from, to, guid_hex_str(guid), d->file_name);
				break;
			}
		}
	}

	for (sl = to_stop; sl; sl = g_slist_next(sl)) {
		struct download *d = (struct download *) sl->data;
		download_queue_delay(d, download_retry_stopped_delay,
			"Stopped (Index changed)");
	}

	/*
	 * This is a sanity check: we should not have any duplicate request
	 * in our download list.
	 */

	if (nfound > 1)
		g_warning("Found %d requests for index %d (now %d) at %s",
			nfound, from, to, ip_port_to_gchar(ip, port));
}


/*
 * download_new
 *
 * Create a new download, usually called from an interactive user action.
 * Return whether download was created.
 */
gboolean download_new(gchar *file, guint32 size, guint32 record_index,
			  guint32 ip, guint16 port, gchar *guid, gchar *hostname,
			  gchar *sha1, time_t stamp, gboolean push,
			  struct dl_file_info *fi, gnet_host_vec_t *proxies)
{
	return NULL != create_download(file, size, record_index, ip, port, guid,
		hostname, sha1, stamp, push, TRUE, fi, proxies);
}

/*
 * download_orphan_new
 *
 * Fake a new download for an existing file that is marked complete in
 * its fileinfo trailer.
 */
void download_orphan_new(
	gchar *file, guint32 size, gchar *sha1, struct dl_file_info *fi)
{
	(void) create_download(file, size, 0, 0, 0, blank_guid, NULL, sha1,
		time(NULL), FALSE, TRUE, fi, NULL);
}

/*
 * download_free_removed
 *
 * Free all downloads listed in the `sl_removed' list.
 */
void download_free_removed(void)
{
	GSList *l;

	if (sl_removed == NULL)
		return;

	for (l = sl_removed; l; l = g_slist_next(l)) {
		struct download *d = (struct download *) l->data;

		g_assert(d != NULL);
		g_assert(d->status == GTA_DL_REMOVED);

		download_reclaim_server(d, TRUE);	/* Delays freeing of server */

		sl_downloads = g_slist_remove(sl_downloads, d);
		sl_unqueued = g_slist_remove(sl_unqueued, d);

		wfree(d, sizeof(*d));
	}

	g_slist_free(sl_removed);
	sl_removed = NULL;

	for (l = sl_removed_servers; l; l = g_slist_next(l)) {
		struct dl_server *s = (struct dl_server *) l->data;
		free_server(s);
	}

	g_slist_free(sl_removed_servers);
	sl_removed_servers = NULL;
}

/*
 * download_remove
 *
 * Freeing a download cannot be done simply, because it might happen when
 * we are traversing the `sl_downloads' or `sl_unqueued' lists.
 *
 * Therefore download_free() marks the download as "removed" and frees some
 * of the memory used, but does not reclaim the download structure yet, nor
 * does it remove it from the lists.
 *
 * The "freed" download is marked GTA_DL_REMOVED and is put into the
 * `sl_removed' list where it will be reclaimed later on via
 * download_free_removed().
 */
gboolean download_remove(struct download *d)
{
	g_assert(d);
	g_assert(d->status != GTA_DL_REMOVED);		/* Not already freed */

	/*
	 * Make sure download is not used by a background task
	 * 		-- JA 25/10/2003
	 */
	if (d->status == GTA_DL_VERIFY_WAIT || d->status == GTA_DL_VERIFYING)
		return FALSE;
	
	if (DOWNLOAD_IS_VISIBLE(d))
		download_gui_remove(d);

	if (DOWNLOAD_IS_QUEUED(d)) {
		g_assert(dl_queue_count > 0);

		gnet_prop_set_guint32_val(PROP_DL_QUEUE_COUNT, dl_queue_count - 1);
		if (d->flags & DL_F_REPLIED)
			gnet_prop_set_guint32_val(PROP_DL_QALIVE_COUNT, dl_qalive_count - 1);

		g_assert(dl_qalive_count >= 0);
	}

	/*
	 * Abort running download (which will decrement the lifecount), otherwise
	 * make sure we decrement it here (e.g. if the download was queued).
	 */

	if (DOWNLOAD_IS_RUNNING(d))
		download_stop(d, GTA_DL_ABORTED, NULL);
	else if (DOWNLOAD_IS_STOPPED(d))
		/* nothing, lifecount already decremented */;
	else {
		g_assert(d->file_info->lifecount > 0);
		d->file_info->lifecount--;
	}

	g_assert(d->io_opaque == NULL);

	if (d->push)
		download_push_remove(d);

	if (d->sha1) {
		atom_sha1_free(d->sha1);
		d->sha1 = NULL;
	}

	if (d->ranges) {
		http_range_free(d->ranges);
		d->ranges = NULL;
	}

	if (d->req) {
		http_buffer_free(d->req);
		d->req = NULL;
	}
	
	/* 
	 * Let parq remove and free its allocated memory
	 *			-- JA, 18/4/2003
	 */
	parq_dl_remove(d);

	download_remove_from_server(d, FALSE);
	d->status = GTA_DL_REMOVED;

	atom_str_free(d->file_name);
	if (d->escaped_name != d->file_name)
		g_free(d->escaped_name);

	d->file_name = NULL;
	d->escaped_name = NULL;

	file_info_remove_source(d->file_info, d, FALSE); /* Keep fileinfo around */
	d->file_info = NULL;

	idtable_free_id(src_handle_map, d->src_handle);

	sl_removed = g_slist_prepend(sl_removed, d);
	
	/* download structure will be freed in download_free_removed() */
	return TRUE;
}

/* ----------------------------------------- */

/*
 * download_forget
 *
 * Forget about download: stop it if running.
 * When `unavailable' is TRUE, mark the download as unavailable.
 */
void download_forget(struct download *d, gboolean unavailable)
{
	g_assert(d);

	if (DOWNLOAD_IS_STOPPED(d))
		return;

	if (DOWNLOAD_IS_QUEUED(d)) {
		download_unqueue(d);
		download_gui_add(d);
	}

	if (unavailable)
		download_unavailable(d, GTA_DL_ABORTED, NULL);
	else
		download_stop(d, GTA_DL_ABORTED, NULL);
}

/*
 * download_abort
 *
 * Abort download (forget about it) AND delete file if we removed the last
 * reference to it and they want to delete on abort.
 */
void download_abort(struct download *d)
{
	g_assert(d);

	download_forget(d, FALSE);

	/* 
	 * The refcount isn't decreased until "Clear completed", so
	 * we may very well have a file with a high refcount and no active
	 * or queued downloads.  This is why we maintain a lifecount.
	 */

	if (d->file_info->lifecount == 0)
		if (download_delete_aborted)
			download_remove_file(d, FALSE);
}

void download_resume(struct download *d)
{
	g_assert(d);
	g_assert(!DOWNLOAD_IS_QUEUED(d));

	if (DOWNLOAD_IS_RUNNING(d) || DOWNLOAD_IS_WAITING(d))
		return;

	g_assert(d->list_idx == DL_LIST_STOPPED);

	switch (d->status) {
	case GTA_DL_COMPLETED:
	case GTA_DL_VERIFY_WAIT:
	case GTA_DL_VERIFYING:
	case GTA_DL_VERIFIED:
	case GTA_DL_MOVE_WAIT:
	case GTA_DL_MOVING:
	case GTA_DL_DONE:
		return;
	default: ;
		/* FALL THROUGH */
	}

	d->file_info->lifecount++;

	if (
		NULL != has_same_download(d->file_name, d->sha1,
			download_guid(d), download_ip(d), download_port(d))
	) {
		d->status = GTA_DL_CONNECTING;		/* So we may call download_stop */
		download_move_to_list(d, DL_LIST_RUNNING);
		download_stop(d, GTA_DL_ERROR, "Duplicate");
		return;
	}

	download_start(d, TRUE);
}

/*
 * download_requeue
 *
 * Explicitly re-enqueue potentially stopped download.
 */
void download_requeue(struct download *d)
{
	g_assert(d);
	g_assert(!DOWNLOAD_IS_QUEUED(d));

	if (DOWNLOAD_IS_VERIFYING(d))		/* Can't requeue: it's done */
		return;

	if (DOWNLOAD_IS_STOPPED(d))
		d->file_info->lifecount++;

	download_queue(d, _("Explicitly requeued"));
}

/*
 * use_push_proxy
 *
 * Try to setup the download to use the push proxies available on the server.
 * Returns TRUE is we can use a push proxy.
 */
static gboolean use_push_proxy(struct download *d)
{
	struct dl_server *server = d->server;

	g_assert(d->push);
	g_assert(!has_blank_guid(d));

	if (d->cproxy != NULL) {
		cproxy_free(d->cproxy);
		d->cproxy = NULL;
	}

	while (server->proxies != NULL) {
		gnet_host_t *host;

		host = (gnet_host_t *) server->proxies->data;	/* Pick the first */
		d->cproxy = cproxy_create(d, host->ip, host->port,
			download_guid(d), d->record_index);

		if (d->cproxy) {
			gui_update_download(d, TRUE);	/* Will read status in d->cproxy */
			return TRUE;
		}

		remove_proxy(server, host->ip, host->port);
	}

	return FALSE;
}

/*
 * download_proxy_newstate
 *
 * Called when the status of the HTTP request made by the client push-proxy
 * code changes.
 */
void download_proxy_newstate(struct download *d)
{
	gui_update_download(d, TRUE);	/* Will read status in d->cproxy */
}

/*
 * download_proxy_sent
 *
 * Called by client push-proxy side when we got indication that the PUSH
 * has been sent.
 */
void download_proxy_sent(struct download *d)
{
	gui_update_download(d, TRUE);	/* Will read status in d->cproxy */
}

/*
 * download_proxy_failed
 *
 * Called by client push-proxy side to indicate that it could not send a PUSH.
 */
void download_proxy_failed(struct download *d)
{
	struct cproxy *cp = d->cproxy;

	g_assert(cp != NULL);

	gui_update_download(d, TRUE);	/* Will read status in d->cproxy */

	remove_proxy(d->server, cproxy_ip(cp), cproxy_port(cp));
	cproxy_free(d->cproxy);
	d->cproxy = NULL;

	if (!use_push_proxy(d))
		download_retry(d);
}

/*
 * IO functions
 */

/*
 * send_push_request
 *
 * Send a push request to the target GUID, in order to request the push of
 * the file whose index is `file_id' there onto our local port `port'.
 *
 * Returns TRUE if the request could be sent, FALSE if we don't have the route.
 */
static gboolean send_push_request(
	const gchar *guid, guint32 file_id, guint16 port)
{
	struct gnutella_msg_push_request m;
	GSList *nodes;

	nodes = route_towards_guid(guid);
	if (nodes == NULL)
		return FALSE;

	message_set_muid(&m.header, GTA_MSG_PUSH_REQUEST);

	/*
	 * NB: we send the PUSH message with max_ttl, not my_ttl, in case the
	 * message needs to be alternatively routed (the path the query hit used
	 * has been broken).
	 */

	m.header.function = GTA_MSG_PUSH_REQUEST;
	m.header.ttl = max_ttl;
	m.header.hops = 0;

	WRITE_GUINT32_LE(sizeof(struct gnutella_push_request), m.header.size);

	memcpy(m.request.guid, guid, 16);

	WRITE_GUINT32_LE(file_id, m.request.file_id);
	WRITE_GUINT32_BE(listen_ip(), m.request.host_ip);
	WRITE_GUINT16_LE(port, m.request.host_port);

	/*
	 * Send the message to all the nodes that can route our request back
	 * to the source of the query hit.
	 */

	message_add(m.header.muid, GTA_MSG_PUSH_REQUEST, NULL);
	gmsg_sendto_all(nodes, (gchar *) &m, sizeof(m));

	g_slist_free(nodes);

	return TRUE;
}

/***
 *** I/O header parsing callbacks
 ***/

#define DOWNLOAD(x)		((struct download *) (x))

static void err_line_too_long(gpointer o)
{
	download_stop(DOWNLOAD(o), GTA_DL_ERROR, "Failed (Header line too large)");
}

static void err_header_error(gpointer o, gint error)
{
	download_stop(DOWNLOAD(o), GTA_DL_ERROR,
		"Failed (%s)", header_strerror(error));
}

static void err_input_buffer_full(gpointer o)
{
	download_stop(DOWNLOAD(o), GTA_DL_ERROR, "Failed (Input buffer full)");
}

static void err_header_read_error(gpointer o, gint error)
{
	struct download *d = DOWNLOAD(o);

	if (error == ECONNRESET) {
		if (d->retries++ < download_max_retries)
			download_queue_delay(d, download_retry_stopped_delay,
				"Stopped (%s)", g_strerror(error));
		else
			download_unavailable(d, GTA_DL_ERROR,
				"Too many attempts (%d)", d->retries - 1);
	} else
		download_stop(d, GTA_DL_ERROR, "Failed (Read error: %s)",
			g_strerror(error));
}

static void err_header_read_eof(gpointer o)
{
	struct download *d = DOWNLOAD(o);
	header_t *header = io_header(d->io_opaque);

	if (header_lines(header) == 0) {
		/*
		 * If the connection was flagged keep-alive, we were making
		 * a follow-up request but the server did not honour it and
		 * closed the connection (probably after serving the last byte
		 * of the previous request).
		 *		--RAM, 01/09/2002
		 *
		 * If server shrunk our reply, then it's probably a server
		 * implementing some kind of "rotating" queues.  Don't activate
		 * the "no keepalive" attribute then.
		 *		--RAM, 05/07/2003
		 */

		if (d->keep_alive && !(d->flags & DL_F_SHRUNK_REPLY))
			d->server->attrs |= DLS_A_NO_KEEPALIVE;

		/*
		 * If we did not read anything in the header at that point, and
		 * we sent a /uri-res request, maybe the remote server does not
		 * support it and closed the connection abruptly.
		 *		--RAM, 20/06/2002
		 */

		if (download_retry_no_urires(d, 0, 0))
			return;

		/*
		 * Maybe we sent HTTP header continuations and the server does not
		 * understand them, breaking the connection on "invalid" request.
		 * Use minimalist HTTP then when talking to this server!
		 */

		d->server->attrs |= DLS_A_MINIMAL_HTTP;
	} else {
		/*
		 * As some header lines were read, we could at least try to get the
		 * server's name so we can display it.
		 *		-- JA, 22/03/2003
		 */
		download_get_server_name(d, header);
	}

	if (d->retries++ < download_max_retries)
		download_queue_delay(d, download_retry_stopped_delay,
			d->keep_alive ? "Connection not kept-alive (EOF)" :
			"Stopped (EOF)");
	else
		download_unavailable(d, GTA_DL_ERROR,
			"Too many attempts (%d)", d->retries - 1);
}

static struct io_error download_io_error = {
	err_line_too_long,
	NULL,
	err_header_error,
	err_header_read_eof,		/* Input exception, assume EOF */
	err_input_buffer_full,
	err_header_read_error,
	err_header_read_eof,
	NULL,
};

static void download_start_reading(gpointer o)
{
	struct download *d = DOWNLOAD(o);
	tm_t now;
	tm_t elapsed;
	guint32 latency;

	/*
	 * Compute the time it took since we sent the headers, and update
	 * the fast EMA (n=7 terms) storing the HTTP latency, in msecs.
	 */

	tm_now(&now);
	tm_elapsed(&elapsed, &now, &d->header_sent);

	gnet_prop_get_guint32_val(PROP_DL_HTTP_LATENCY, &latency);
	latency += (tm2ms(&elapsed) >> 2) - (latency >> 2);
	gnet_prop_set_guint32_val(PROP_DL_HTTP_LATENCY, latency);

	/*
	 * Update status and GUI, timestamp start of header reading.
	 */

	d->status = GTA_DL_HEADERS;
	d->last_update = time((time_t *) 0);	/* Starting reading */
	gui_update_download(d, TRUE);
}

static void call_download_request(gpointer o, header_t *header)
{
	download_request(DOWNLOAD(o), header, TRUE);
}

static void call_download_push_ready(gpointer o, header_t *header)
{
	struct download *d = DOWNLOAD(o);

	download_push_ready(d, io_getline(d->io_opaque));
}

#undef DOWNLOAD

/*
 * download_overlap_check
 *
 * Check that the leading overlapping data in the socket buffer match with
 * the last ones in the downloaded file.  Then remove them.
 *
 * Returns TRUE if the data match, FALSE if they don't, in which case the
 * download is stopped.
 */
static gboolean download_overlap_check(struct download *d)
{
	gchar *path;
	struct gnutella_socket *s = d->socket;
	struct dl_file_info *fi = d->file_info;
	gint fd = -1;
	struct stat buf;
	gchar *data = NULL;
	gint r;
	off_t offset;
	off_t begin;
	off_t end;
	guint32 backout;

	g_assert(fi->lifecount > 0);
	g_assert(fi->lifecount <= fi->refcount);

	path = g_strdup_printf("%s/%s", fi->path, fi->file_name);
	if (NULL == path)
		goto out;

	fd = file_open(path, O_RDONLY);
	G_FREE_NULL(path);
	if (fd == -1) {
		const gchar * error = g_strerror(errno);
		g_warning("cannot check resuming for \"%s\": %s", fi->file_name, error);
		download_stop(d, GTA_DL_ERROR, "Can't check resume data: %s", error);
		goto out;
	}

	if (-1 == fstat(fd, &buf)) {			/* Should never happen */
		const gchar *error = g_strerror(errno);
		g_warning("cannot stat opened \"%s\": %s", fi->file_name, error);
		download_stop(d, GTA_DL_ERROR, "Can't stat opened file: %s", error);
		goto out;
	}

	/*
	 * Sanity check: if the file is bigger than when we started, abort
	 * immediately.
	 */

	if (!fi->use_swarming && d->skip != fi->done) {
		g_warning("File '%s' changed size (now %lu, but was %u)",
			fi->file_name, (gulong) buf.st_size, d->skip);
		download_queue_delay(d, download_retry_stopped_delay,
			"Stopped (Output file size changed)");
		goto out;
	}

	offset = d->skip - d->overlap_size;
	if (offset != lseek(fd, offset, SEEK_SET)) {
		download_stop(d, GTA_DL_ERROR, "Unable to seek: %s",
			g_strerror(errno));
		goto out;
	}

	/*
	 * We're now at the overlapping start.	Read the data.
	 */

	data = g_malloc(d->overlap_size);
	r = read(fd, data, d->overlap_size);

	if (r < 0) {
		const gchar *error = g_strerror(errno);
		g_warning("cannot read resuming data for \"%s\": %s",
			fi->file_name, error);
		download_stop(d, GTA_DL_ERROR, "Can't read resume data: %s", error);
		goto out;
	}

	if (r != d->overlap_size) {
		g_warning("Short read (%d instead of %d bytes) on resuming data "
			"for \"%s\"", r, d->overlap_size, fi->file_name);
		download_stop(d, GTA_DL_ERROR, "Short read on resume data");
		goto out;
	}

	if (0 != memcmp(s->buffer, data, d->overlap_size)) {
		if (dbg > 3)
			printf("%d overlapping bytes UNMATCHED at offset %d for \"%s\"\n",
				d->overlap_size, d->skip - d->overlap_size, d->file_name);

		download_bad_source(d);		/* Until proven otherwise if we resume it */

		if (dl_remove_file_on_mismatch) {
			download_queue(d, "Resuming data mismatch @ %lu",
				(gulong) (d->skip - d->overlap_size));
			download_remove_file(d, TRUE);
		} else {
			/*
			 * It is most likely that we have a mismatch because
			 * the other guy's data is not in order, but we could
			 * also have received bad data ourselves. Just to be
			 * sure we back out some of our data. Eventually we
			 * should find a host with good data, or we have
			 * backed out enough times for our data to be good
			 * again. This really is a stop-gap measure that TTH
			 * will fill in a more permanent way.
			 */

			end = d->skip + 1;
			gnet_prop_get_guint32_val(PROP_DL_MISMATCH_BACKOUT, &backout);
			if (end >= backout)
				begin = end - backout;
			else
				begin = 0;
			file_info_update(d, begin, end, DL_CHUNK_EMPTY);
			g_warning("Resuming data mismatch on %s, backed out %d bytes block "
				"from %u to %u\n", 
				 d->file_name, backout, (guint32) begin, (guint32) end);

			/*
			 * Don't always keep this source, and since there is doubt,
			 * leave it to randomness.
			 */

			if (random_value(99) >= 50)
				download_stop(d, GTA_DL_ERROR, "Resuming data mismatch @ %lu",
					(gulong) (d->skip - d->overlap_size));
			else
				download_queue_delay(d, download_retry_busy_delay,
					"Resuming data mismatch @ %lu",
					(gulong) (d->skip - d->overlap_size));
		}
		goto out;
	}

	/*
	 * Remove the overlapping data from the socket buffer.
	 */

	if (s->pos > d->overlap_size)
		memmove(s->buffer, &s->buffer[d->overlap_size],
			s->pos - d->overlap_size);
	s->pos -= d->overlap_size;

	G_FREE_NULL(data);
	close(fd);

	if (dbg > 3)
		printf("%d overlapping bytes MATCHED at offset %d for \"%s\"\n",
			d->overlap_size, d->skip - d->overlap_size, d->file_name);

	return TRUE;

out:
	if (fd != -1)
		close(fd);
	if (data)
		G_FREE_NULL(data);

	return FALSE;
}

/*
 * download_write_data
 *
 * Write data in socket buffer to file.
 */
static void download_write_data(struct download *d)
{
	struct gnutella_socket *s = d->socket;
	struct dl_file_info *fi = d->file_info;
	gint written;
	gboolean trimmed = FALSE;
	struct download *cd;					/* Cloned download, if completed */

	g_assert(s->pos > 0);
	g_assert(fi->lifecount > 0);
	g_assert(fi->lifecount <= fi->refcount);

	/*
	 * If we have an overlapping window and DL_F_OVERLAPPED is not set yet,
	 * then the leading data we have in the buffer are overlapping data.
	 *		--RAM, 12/01/2002, revised 23/11/2002
	 */

	if (d->overlap_size && !(d->flags & DL_F_OVERLAPPED)) {
		g_assert(d->pos == d->skip);
		if (s->pos < d->overlap_size)		/* Not enough bytes yet */
			return;							/* Don't even write anything */
		if (!download_overlap_check(d))		/* Mismatch on overlapped bytes? */
			return;							/* Download was stopped */
		d->flags |= DL_F_OVERLAPPED;		/* Don't come here again */
		if (s->pos == 0)					/* No bytes left to write */
			return;
		/* FALL THROUGH */
	}

	if (d->pos != lseek(d->file_desc, d->pos, SEEK_SET)) {
		const char *error = g_strerror(errno);
		g_warning("download_write_data(): failed to seek at offset %u (%s)",
			d->pos, error);
		download_stop(d, GTA_DL_ERROR, "Can't seek to offset %u: %s",
			d->pos, error);
		return;
	}

	/*
	 * We can't have data going farther than what we requested from the
	 * server.  But if we do, trim and warn.  And mark the server as not
	 * being capable of handling keep-alive connections correctly!
	 */

	if (d->pos + s->pos > d->range_end) {
		gint extra = (d->pos + s->pos) - d->range_end;
		g_warning("server %s (%s) gave us %d more byte%s "
			"than requested for \"%s\"",
			ip_port_to_gchar(download_ip(d), download_port(d)),
			download_vendor_str(d),
			extra, extra == 1 ? "" : "s", download_outname(d));
		s->pos -= extra;
		trimmed = TRUE;
		d->server->attrs |= DLS_A_NO_KEEPALIVE;		/* Since we have to trim */
		g_assert(s->pos > 0);	/* We had not reached range_end previously */
	}

	if (-1 == (written = write(d->file_desc, s->buffer, s->pos))) {
		const char *error = g_strerror(errno);
		g_warning("write to file failed (%s) !", error);
		g_warning("tried to write(%d, %p, %d)",
			  d->file_desc, s->buffer, s->pos);
		download_queue_delay(d, download_retry_busy_delay,
			"Can't save data: %s", error);
		return;
	}

	file_info_update(d, d->pos, d->pos + written, DL_CHUNK_DONE);
	gnet_prop_set_guint64_val(PROP_DL_BYTE_COUNT, dl_byte_count + written);

	if (written < s->pos) {
		g_warning("partial write of %d out of %d bytes to file '%s'",
			written, s->pos, fi->file_name);
		download_queue_delay(d, download_retry_busy_delay,
			"Partial write to file");
		return;
	}

	g_assert(written == s->pos);

	d->pos += written;
	s->pos = 0;

	/*
	 * End download if we have completed it.
	 */

	if (fi->use_swarming) {
		enum dl_chunk_status s = file_info_pos_status(fi, d->pos);

		switch(s) {
		case DL_CHUNK_DONE:
			/*
			 * Reached a zone that is completed.  If the file is done,
			 * we can clear the download.
			 *
			 * Otherwise, if we have reached the end of our requested chunk,
			 * meaning we put an upper boundary to our request, we are probably
			 * on a persistent connection where we'll be able to request
			 * another chunk data of data.
			 *
			 * The only remaining possibility is that we have reached a zone
			 * where a competing download is busy (aggressive swarming on),
			 * and since we cannot tell the remote HTTP server that we wish
			 * to interrupt the current download, we have no choice but to
			 * requeue the download, thereby loosing the slot.
			 */
			if (fi->done >= fi->size)
				goto done;
			else if (d->pos == d->range_end)
				goto partial_done;
			else
				download_queue(d, _("Requeued by competing download"));
			break;
		case DL_CHUNK_BUSY:
			if (d->pos < d->range_end) {	/* Still within requested chunk */
				g_assert(!trimmed);
				break;
			}
			/* FALL THROUGH -- going past our own busy-chunk and competing */
		case DL_CHUNK_EMPTY:
			/*
			 * We're done with our busy-chunk.
			 * We've reached a new virgin territory.
			 *
			 * If we are on a persistent connection AND we reached the
			 * end of our requested range, then the server is expecting
			 * a new request from us.
			 *
			 * Otherwise, go on.  We'll be stopped when we bump into another
			 * DONE chunk anyway.
			 *
			 * XXX It would be nice to extend the zone as much as possible to
			 * XXX avoid new downloads starting from here and competing too
			 * XXX soon with us. -- FIXME (original note from Vidar)
			 */

			if (d->pos == d->range_end)
				goto partial_done;

			d->range_end = download_filesize(d);	/* New upper boundary */

			break;					/* Go on... */
		}
	} else if (FILE_INFO_COMPLETE(fi))
		goto done;
	else
		gui_update_download(d, FALSE);

	return;

	/*
	 * Requested chunk is done.
	 */

partial_done:
	g_assert(d->pos == d->range_end);
	g_assert(fi->use_swarming);

	/*
	 * Since a download structure is associated with a GUI line entry, we
	 * must clone it to be able to display the chunk as completed, yet
	 * continue downloading.
	 */

	cd = download_clone(d);
	download_stop(d, GTA_DL_COMPLETED, NULL);

	/*
	 * If we had to trim the data requested, it means the server did not
	 * understand our Range: request properly, and it's going to send us
	 * more data.  Something weird happened, and we can't even think
	 * continuing with this connection.
	 */

	if (trimmed)
		download_queue(cd, _("Requeued after trimmed data"));
	else if (!cd->keep_alive)
		download_queue(cd, _("Chunk done, connection closed"));
	else {
		if (download_start_prepare(cd)) {
			cd->keep_alive = TRUE;			/* Was reset by _prepare() */
			download_gui_add(cd);
			download_send_request(cd);		/* Will pick up new range */
		}
	}

	return;

	/*
	 * We have completed the download of the requested file.
	 */

done:
	download_stop(d, GTA_DL_COMPLETED, NULL);
	download_verify_sha1(d);

	gnet_prop_set_guint32_val(PROP_TOTAL_DOWNLOADS, total_downloads + 1);
}

/*
 * download_moved_permanently
 *
 * Refresh IP:port, download index and name, by looking at the new location
 * in the header ("Location:").
 *
 * Returns TRUE if we managed to parse the new location.
 */
static gboolean download_moved_permanently(
	struct download *d, header_t *header)
{
	gchar *buf;
	dmesh_urlinfo_t info;
	guint32 ip = download_ip(d);
	guint16 port = download_port(d);

	buf = header_get(header, "Location");
	if (buf == NULL)
		return FALSE;

	if (!dmesh_url_parse(buf, &info)) {
		g_warning("could not parse HTTP Location: %s", buf);
		return FALSE;
	}

	/*
	 * If ip/port changed, accept the new ones but warn.
	 */

	if (info.ip != ip || info.port != port)
		g_warning("server %s (file \"%s\") redirecting us to alien %s",
			ip_port_to_gchar(ip, port), d->file_name, buf);

	/*
	 * Check filename.
	 *
	 * If it changed, we don't change the output_name, so we'll continue
	 * to write to the same file we previously started with.
	 *
	 * NB: idx = 0 is used to indicate a /uri-res/N2R? URL, which we don't
	 * really want here (if we have the SHA1, we already asked for it).
	 */

	if (info.idx == 0) {
		g_warning("server %s (file \"%s\") would redirect us to %s",
			ip_port_to_gchar(ip, port), d->file_name, buf);
		atom_str_free(info.name);
		return FALSE;
	}

	if (0 != strcmp(info.name, d->file_name)) {
		g_warning("file \"%s\" was renamed \"%s\" on %s",
			d->file_name, info.name, ip_port_to_gchar(info.ip, info.port));

		/*
		 * If name changed, we must update the global hash counting downloads.
		 * We ensure the current download is in the running list, since only
		 * those can be stored in the hash.
		 */

		g_assert(d->list_idx == DL_LIST_RUNNING);

		atom_str_free(d->file_name);
		if (d->escaped_name != d->file_name)
			g_free(d->escaped_name);

		d->file_name = info.name;			/* Already an atom */
		d->escaped_name = url_escape_cntrl(info.name);
	} else
		atom_str_free(info.name);

	/*
	 * Update download structure.
	 */

	if (d->push)
		download_push_remove(d);			/* About to change index */

	d->record_index = info.idx;

	if (d->push)
		download_push_insert(d);

	download_redirect_to_server(d, info.ip, info.port);

	return TRUE;
}

/*
 * download_get_server_name
 *
 * Extract server name from headers.
 * Returns whether new server name was found.
 */
static gboolean download_get_server_name(
	struct download *d, header_t *header)
{
	const gchar *buf;
	gboolean got_new_server = FALSE;

	buf = header_get(header, "Server");			/* Mandatory */
	if (!buf)
		buf = header_get(header, "User-Agent"); /* Maybe they're confused */

	if (buf) {
		struct dl_server *server = d->server;
		gboolean faked =
			!version_check(buf, header_get(header, "X-Token"), download_ip(d));
		if (server->vendor == NULL) {
			if (faked) {
				gchar *name = g_strdup_printf("!%s", buf);
				server->vendor = atom_str_get(name);
				G_FREE_NULL(name);
			} else
				server->vendor = atom_str_get(buf);
			got_new_server = TRUE;
		} else if (!faked && 0 != strcmp(server->vendor, buf)) {
			/* Name changed? */
			atom_str_free(server->vendor);
			server->vendor = atom_str_get(buf);
			got_new_server = TRUE;
		}
	}

	return got_new_server;
}

/*
 * download_check_status
 *
 * Check status code from status line.
 * Return TRUE if we can continue.
 */
static gboolean download_check_status(
	struct download *d, getline_t *line, gint code)
{
	if (code == -1) {
		g_warning("weird HTTP acknowledgment status line from %s",
			ip_to_gchar(d->socket->ip));
		dump_hex(stderr, "Status Line", getline_str(line),
			MIN(getline_length(line), 80));

		/*
		 * Don't abort the download if we're already on a persistent
		 * connection: the server might have goofed, or we have a bug.
		 * What we read was probably still data coming through.
		 */

		if (d->keep_alive)
			download_queue(d, _("Weird HTTP status (protocol desync?)"));
		else
			download_stop(d, GTA_DL_ERROR, "Weird HTTP status");
		return FALSE;
	}

	return TRUE;
}

/*
 * download_convert_to_urires
 *
 * Convert download to /uri-res/N2R? request.
 *
 * This is called when we have a /get/index/name URL for the download, yet
 * we attempted a GET /uri-res/ and either got a 503, or a 2xx return code.
 * This means the remote server understands /uri-res/ with high probability.
 *
 * Converting the download to /uri-res/N2R? means that we get rid of the
 * old index/name information in the download structure and replace it
 * with URN_INDEX/URN.  Indeed, to access the download, we only need to issue
 * a /uri-res request from now on.
 *
 * As a side effect, we remove the old index/name information from the download
 * mesh as well.
 *
 * Returns TRUE if OK, FALSE if we stopped the download because we finally
 * spotted it as being a duplicate!
 */
static gboolean download_convert_to_urires(struct download *d)
{
	gchar *name;
	struct download *xd;

	g_assert(d->record_index != URN_INDEX);
	g_assert(d->sha1 != NULL);
	g_assert(d->file_info->sha1 == d->sha1);

	/*
	 * In case it is still recorded under its now obsolete index/name...
	 */

	dmesh_remove(d->sha1, download_ip(d), download_port(d),
		d->record_index, d->file_name);

	name = atom_str_get(sha1_base32(d->sha1));

	if (dbg > 2)
		g_warning("download at %s \"%u/%s\" becomes "
			"\"/uri-res/N2R?urn:sha1:%s\"",
			ip_port_to_gchar(download_ip(d), download_port(d)),
			d->record_index, d->file_name, name);
	
	atom_str_free(d->file_name);
	if (d->escaped_name != d->file_name)
		g_free(d->escaped_name);
	d->record_index = URN_INDEX;
	d->file_name = name;
	d->escaped_name = url_escape_cntrl(name);

	/*
	 * Maybe it became a duplicate download, due to our lame detection?
	 */

	xd = has_same_download(name, d->sha1,
			download_guid(d), download_ip(d), download_port(d));

	if (xd != NULL && xd != d) {
		download_stop(d, GTA_DL_ERROR, "Was a duplicate");
		return FALSE;
	}

	return TRUE;
}

/*
 * extract_retry_after
 *
 * Extract Retry-After delay from header, returning 0 if none.
 */
guint extract_retry_after(const header_t *header)
{
	const gchar *buf;
	guint delay = 0;

	/*
	 * A Retry-After header is either a full HTTP date, such as
	 * "Fri, 31 Dec 1999 23:59:59 GMT", or an amount of seconds.
	 */

	buf = header_get(header, "Retry-After");
	if (buf) {
		if (!sscanf(buf, "%u", &delay)) {
			time_t now = time((time_t *) NULL);
			time_t retry = date2time(buf, &now);

			if (retry == -1)
				g_warning("cannot parse Retry-After: %s", buf);
			else
				delay = retry > now ? retry - now : 0;
		}
	}

	return delay;
}

/*
 * check_date
 *
 * Look for a Date: header in the reply and use it to update our skew.
 */
static void check_date(const header_t *header, guint32 ip)
{
	const gchar *buf;

	buf = header_get(header, "Date");
	if (buf) {
		time_t now = time((time_t *) NULL);
		time_t their = date2time(buf, &now);

		if (their == -1)
			g_warning("cannot parse Date: %s", buf);
		else
			clock_update(their, 1, ip);
	}
}

/*
 * check_xhostname
 *
 * Look for an X-Hostname header in the reply.  If we get one, then it means
 * the remote server is not firewalled and can be reached there, using
 * the symbolic hostname given.
 */
static void check_xhostname(struct download *d, const header_t *header)
{
	gchar *buf;
	struct dl_server *server = d->server;

	buf = header_get(header, "X-Hostname");

	if (buf == NULL)
		return;

	/*
	 * If we got a GIV, ignore all pushes to this server from now on.
	 * We'll mark the server as DLS_A_PUSH_IGN the first time we'll
	 * be able to connect to it.
	 */

	if (d->got_giv) {
		if (d->push)
			download_push_remove(d);

		if (dbg > 2)
			printf("PUSH got X-Hostname, trying to ignore them for %s (%s)\n",
				buf, ip_port_to_gchar(download_ip(d), download_port(d)));

		d->flags |= DL_F_PUSH_IGN;
	}

	/*
	 * If we had a hostname for this server, and it has not changed,
	 * then we're done.
	 */

	if (server->hostname != NULL && 0 == strcasecmp(server->hostname, buf))
		return;

	set_server_hostname(server, buf);
	gui_update_download_host(d);
}

/*
 * check_xhost
 *
 * Look for an X-Host header in the reply.  If we get one, then it means
 * the remote server is not firewalled and can be reached there.
 *
 * We only pay attention to such headers for pushed downloads.
 */
static void check_xhost(struct download *d, const header_t *header)
{
	const gchar *buf;
	guint32 ip;
	guint16 port;

	g_assert(d->got_giv);

	buf = header_get(header, "X-Host");

	if (buf == NULL)
		return;

	if (!gchar_to_ip_port(buf, &ip, &port) || !host_is_valid(ip, port))
		return;

	/*
	 * It is possible that the IP:port we already have for this server
	 * be wrong.  We may have gotten an IP:port from a query hit before
	 * the server knew its real IP address.
	 */

	if (ip != download_ip(d) || port != download_port(d))
		download_redirect_to_server(d, ip, port);

	/*
	 * Most importantly, ignore all pushes to this server from now on.
	 * We'll mark the server as DLS_A_PUSH_IGN the first time we'll
	 * be able to connect to it.
	 */

	if (d->push)
		download_push_remove(d);

	if (dbg > 2)
		printf("PUSH got X-Host, trying to ignore them for %s\n",
			ip_port_to_gchar(download_ip(d), download_port(d)));

	d->flags |= DL_F_PUSH_IGN;
}

/*
 * check_content_urn
 *
 * Check for X-Gnutella-Content-URN.
 * Returns FALSE if we cannot continue with the download.
 */
static gboolean check_content_urn(struct download *d, header_t *header)
{
	gchar *buf;
	gchar digest[SHA1_RAW_SIZE];
	gboolean found_sha1 = FALSE;

	buf = header_get(header, "X-Gnutella-Content-Urn");

	/*
	 * Clueless Shareaza chose to blindly and secretly change the header
	 * into X-Content-Urn, which can also contain a list of URNs and not
	 * a single URN (the latter being a good thing actually).
	 *		--RAM, 16/06/2003
	 */

	if (buf == NULL)
		buf = header_get(header, "X-Content-Urn");

	if (buf == NULL) {
		gboolean n2r = FALSE;

		/*
		 * We don't have any X-Gnutella-Content-URN header on this server.
		 * If fileinfo has a SHA1, we must be careful if we cannot be sure
		 * we're writing to the SAME file.
		 */

		if (d->record_index == URN_INDEX)
			n2r = TRUE;
		else if (d->flags & DL_F_URIRES)
			n2r = TRUE;

		/*
		 * If we sent an /uri-res/N2R?urn:sha1: request, the server might
		 * not necessarily send an X-Gnutella-Content-URN in the reply, since
		 * HUGE does not mandate it (it simply says the server "SHOULD" do it).
		 *		--RAM, 15/11/2002
		 */

		if (n2r)
			goto collect_locations;		/* Should be correct in reply */

		/*
		 * If "download_require_urn" is set, stop.
		 *
		 * If they have configured an overlapping range of at least
		 * DOWNLOAD_MIN_OVERLAP, we can requeue the download if we were not
		 * overlapping here, in the hope we'll (later on) request a chunk after
		 * something we have already downloaded.
		 *
		 * If not, stop definitively.
		 */

		if (d->file_info->sha1) {
			if (download_require_urn) {			/* They want strictness */
				download_bad_source(d);
				download_stop(d, GTA_DL_ERROR, "No URN on server (required)");
				return FALSE;
			}
			if (download_overlap_range >= DOWNLOAD_MIN_OVERLAP) {
				if (download_optimistic_start && (d->pos == 0))
					return TRUE;

				if (d->overlap_size == 0) {
					download_queue_delay(d, download_retry_busy_delay,
						"No URN on server, waiting for overlap");
					return FALSE;
				}
			} else {
				download_bad_source(d);
				download_stop(d, GTA_DL_ERROR, "No URN on server to validate");
				return FALSE;
			}
		}

		return TRUE;		/* Nothing to check against, continue */
	}

	found_sha1 = dmesh_collect_sha1(buf, digest);

	if (!found_sha1)
		return TRUE;

	if (d->sha1 && 0 != memcmp(digest, d->sha1, SHA1_RAW_SIZE)) {
		download_bad_source(d);
		download_stop(d, GTA_DL_ERROR, "URN mismatch detected");
		return FALSE;
	}

	/*
	 * Record SHA1 if we did not know it yet.
	 */

	if (d->sha1 == NULL) {
		d->sha1 = atom_sha1_get(digest);

		/*
		 * The following test for equality works because both SHA1
		 * are atoms.
		 */

		if (d->file_info->sha1 != d->sha1) {
			g_warning("discovered SHA1 %s on the fly for %s "
				"(fileinfo has %s)",
				sha1_base32(d->sha1), download_outname(d),
				d->file_info->sha1 ? "another" : "none");

			/*
			 * If the SHA1 does not match that of the fileinfo,
			 * abort the download.
			 */

			if (d->file_info->sha1) {
				g_assert(!sha1_eq(d->file_info->sha1, d->sha1));

				download_info_reget(d);
				download_queue(d, _("URN fileinfo mismatch"));

				g_assert(d->file_info->sha1 == d->sha1);

				return FALSE;
			}

			g_assert(d->file_info->sha1 == NULL);

			/*
			 * Record SHA1 in the fileinfo structure, and make sure
			 * we're not asked to ignore this download, now that we
			 * got the SHA1.
			 *
			 * WARNING: d->file_info can change underneath during
			 * this call, and the current download can be requeued!
			 */

			if (!file_info_got_sha1(d->file_info, d->sha1)) {
				download_info_reget(d);
				download_queue(d, _("Discovered dup SHA1"));
				return FALSE;
			}

			g_assert(d->file_info->sha1 == d->sha1);

			if (DOWNLOAD_IS_QUEUED(d))		/* Queued by call above */
				return FALSE;

			if (download_ignore_requested(d))
				return FALSE;
		}

		/*
		 * Discovery of the SHA1 for a download should be infrequent enough,
		 * yet is very important.   This jusifies immediately storing that
		 * new information without waiting for the "dirty timer" to trigger.
		 */

		download_store();				/* Save SHA1 */
		file_info_store_if_dirty();

		/*
		 * Insert record in download mesh if it does not require
		 * a push.	Since we just got a connection, we use "now"
		 * as the mesh timestamp.
		 */

		if (!d->always_push)
			dmesh_add(d->sha1,
				download_ip(d), download_port(d), d->record_index,
				d->file_name, 0);
	}

	/*
	 * Check for possible download mesh headers.
	 */

collect_locations:
	huge_collect_locations(d->sha1, header, download_vendor(d));

	return TRUE;
}

/*
 * check_push_proxies
 *
 * Extract host:port information out of X-Push-Proxies if present and
 * update the server's list.
 */
static void check_push_proxies(struct download *d, header_t *header)
{
	gchar *buf;
	struct dl_server *server = d->server;
	const gchar *tok;
	GSList *l = NULL;

	buf = header_get(header, "X-Push-Proxies");
	if (buf == NULL)
		buf = header_get(header, "X-Pushproxies");	/* Legacy */

	if (buf == NULL)
		return;

	for (tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
		guint32 ip;
		guint16 port;

		if (gchar_to_ip_port(tok, &ip, &port)) {
			gnet_host_t *host = walloc(sizeof(*host));

			host->ip = ip;
			host->port = port;

			l = g_slist_prepend(l, host);
		}
	}

	if (server->proxies)
		free_proxies(server);

	server->proxies = l;
	server->proxies_stamp = time(NULL);
}

/*
 * update_available_ranges
 *
 * Partial File Sharing Protocol (PFSP) -- client-side
 *
 * If there is an X-Available-Range header, parse it to know
 * whether we can spot a range that is available and which we
 * do not have.
 */
static void update_available_ranges(struct download *d, header_t *header)
{
	gchar *buf;
	const gchar *available = "X-Available-Ranges";

	if (d->ranges != NULL) {
		http_range_free(d->ranges);
		d->ranges = NULL;
	}

	if (!d->file_info->use_swarming)
		return;

	g_assert(header != NULL);
	g_assert(header->headers != NULL);
	
	buf = header_get(header, available);

	if (buf == NULL || download_filesize(d) == 0)
		return;

	/*
	 * Update available range list and total size available remotely.
	 */

	d->ranges = http_range_parse(available, buf,
		download_filesize(d), download_vendor_str(d));

	d->ranges_size = http_range_size(d->ranges);
}

/*
 * download_sink
 *
 * Sink read data.
 * Used when waiting for the end of the previous HTTP reply.
 *
 * When all the data has been sunk, issue the next HTTP request.
 */
static void download_sink(struct download *d)
{
	struct gnutella_socket *s = d->socket;

	g_assert(s->pos >= 0 && s->pos <= sizeof(s->buffer));
	g_assert(d->status == GTA_DL_SINKING);
	g_assert(d->flags & DL_F_CHUNK_CHOSEN);
	g_assert(d->flags & DL_F_SUNK_DATA);

	if (s->pos > d->sinkleft) {
		g_warning("got more data to sink than expected from %s <%s>",
			ip_port_to_gchar(download_ip(d), download_port(d)),
			download_vendor_str(d));
		download_stop(d, GTA_DL_ERROR, "More data to sink than expected");
		return;
	}

	d->sinkleft -= s->pos;
	s->pos = 0;

	/*
	 * When we're done sinking everything, remove the read callback
	 * and send the pending request.
	 */

	if (d->sinkleft == 0) {
		bsched_source_remove(d->bio);
		d->bio = NULL;
		d->status = GTA_DL_CONNECTING;
		download_send_request(d);
	}
}

/*
 * download_sink_read
 *
 * Read callback for file data.
 */
static void download_sink_read(gpointer data, gint source, inputevt_cond_t cond)
{
	struct download *d = (struct download *) data;
	struct gnutella_socket *s = d->socket;
	gint32 r;

	g_assert(s);

	if (cond & INPUT_EVENT_EXCEPTION) {		/* Treat as EOF */
		socket_eof(s);
		download_queue_delay(d, download_retry_busy_delay,
			"Stopped data (EOF)");
		return;
	}

	r = bio_read(d->bio, s->buffer, sizeof(s->buffer));

	if (r <= 0) {
		if (r == 0) {
			socket_eof(s);
			download_queue_delay(d, download_retry_busy_delay,
				"Stopped data (EOF)");
		} else if (errno != EAGAIN) {
			socket_eof(s);
			if (errno == ECONNRESET)
				download_queue_delay(d, download_retry_busy_delay,
					"Stopped data (%s)", g_strerror(errno));
			else
				download_stop(d, GTA_DL_ERROR,
					"Failed (Read error: %s)", g_strerror(errno));
		}
		return;
	}

	s->pos = r;
	d->last_update = time(NULL);

	download_sink(d);
}

/*
 * download_request
 *
 * Called to initiate the download once all the HTTP headers have been read.
 * If `ok' is false, we timed out reading the header, and have therefore
 * something incomplete.
 *
 * Validate the reply, and begin saving the incoming data if OK.
 * Otherwise, stop the download.
 */
static void download_request(
	struct download *d, header_t *header, gboolean ok)
{
	struct gnutella_socket *s = d->socket;
	const gchar *status;
	gint ack_code;
	const gchar *ack_message = "";
	gchar *buf;
	struct stat st;
	gboolean got_content_length = FALSE;
	guint32 check_content_range = 0;
	guint32 requested_size;
	guint32 ip;
	guint16 port;
	gint http_major = 0, http_minor = 0;
	gboolean is_followup = d->keep_alive;
	struct dl_file_info *fi = d->file_info;
	gchar short_read[80];
	guint delay;
	gchar *path = NULL;
	gboolean refusing;

	g_assert(fi->lifecount > 0);
	g_assert(fi->lifecount <= fi->refcount);

	/*
	 * If `ok' is FALSE, we might not even have fully read the status line,
	 * in which case `s->getline' will be null.
	 */

	if (!ok && s->getline == NULL) {
		download_queue_delay(d, download_retry_busy_delay,
			"Timeout reading HTTP status");
		return;
	}

	g_assert(s->getline);				/* Being in the header reading phase */

	status = getline_str(s->getline);
	d->last_update = time(NULL);		/* Done reading headers */

	if (dbg > 2) {
		const gchar *incomplete = ok ? "" : "INCOMPLETE ";
		printf("----Got %sreply from %s:\n", incomplete, ip_to_gchar(s->ip));
		printf("%s\n", status);
		header_dump(header, stdout);
		printf("----\n");
		fflush(stdout);
	}

	/*
	 * If we did not get any status code at all, re-enqueue immediately.
	 */

	if (!ok && getline_length(s->getline) == 0) {
		download_queue_delay(d, download_retry_busy_delay,
			"Timeout reading headers");
		return;
	}

	/*
	 * If we were pushing this download, check for an X-Host header in
	 * the reply: this will indicate that the remote host is not firewalled
	 * and will give us its IP:port.
	 *
	 * NB: do this before extracting the server token, as it may redirect
	 * us to an alternate server, and we could therefore loose the server
	 * vendor string indication (attaching it to a discarded server object).
	 */

	if (d->got_giv) {
		if (!is_followup)
			check_xhost(d, header);
		check_push_proxies(d, header);
	}

	/*
	 * If we get an X-Hostname header, we know the remote end is not
	 * firewalled, and we get its DNS name: even if its IP changes, we'll
	 * be able to recontact it.
	 */

	check_xhostname(d, header);

	/*
	 * Extract Server: header string, if present, and store it unless
	 * we already have it.
	 */

	if (download_get_server_name(d, header))
		gui_update_download_server(d);

	/*
	 * Check status.
	 */

	ack_code = http_status_parse(status, "HTTP",
		&ack_message, &http_major, &http_minor);

	if (!download_check_status(d, s->getline, ack_code))
		return;

	d->retries = 0;				/* Retry successful, we managed to connect */
	d->flags |= DL_F_REPLIED;

	ip = download_ip(d);
	port = download_port(d);

	check_date(header, ip);		/* Update clock skew if we have a Date: */

	/*
	 * Do we have to keep the connection after this request?
	 *
	 * If server supports HTTP/1.1, record it.  This will help us determine
	 * whether to send a Range: request during swarming, at the next
	 * connection attempt.
	 */

	buf = header_get(header, "Connection");

	if (http_major > 1 || (http_major == 1 && http_minor >= 1)) {
		/* HTTP/1.1 or greater -- defaults to persistent connections */
		d->keep_alive = TRUE;
		d->server->attrs |= DLS_A_HTTP_1_1;
		if (buf && 0 == strcasecmp(buf, "close"))
			d->keep_alive = FALSE;
	} else {
		/* HTTP/1.0 or lesser -- must request persistence */
		d->keep_alive = FALSE;
		if (buf && 0 == strcasecmp(buf, "keep-alive"))
			d->keep_alive = TRUE;
	}

	if (!ok)
		d->keep_alive = FALSE;			/* Got incomplete headers -> close */

	/*
	 * Now deal with the return code.
	 */

	if (ok)
		short_read[0] = '\0';
	else {
		gint count = header_lines(header);
		gm_snprintf(short_read, sizeof(short_read),
			"[short %d line%s header] ", count, count == 1 ? "" : "s");
	}

	
	if (ack_code == 503 || (ack_code >= 200 && ack_code <= 299)) {

		/*
		 * If we made a /uri-res/N2R? request, yet if the download still
		 * has the old index/name indication, convert it to a /uri-res/.
	 	 */
		if (d->record_index != URN_INDEX && (d->flags & DL_F_URIRES))
			if (!download_convert_to_urires(d))
				return;
		
		/*
		 * The download could be remotely queued. Check this now before
		 * continuing at all.
		 *   --JA, 31 jan 2003
		 */
		if (ack_code == 503) {			/* Check for queued status */
			
			if (parq_download_parse_queue_status(d, header)) {
				/* If we are queued, there is nothing else we can do for now */
				if (parq_download_is_active_queued(d)) {
					download_passively_queued(d, FALSE);
					
					/* Make sure we're waiting for the right file, 
					   collect alt-locs */
					if (check_content_urn(d, header)) {

						/* Update mesh */
						if (!d->always_push && d->sha1)
							dmesh_add(d->sha1, ip, port, d->record_index,
								d->file_name, 0);
			
						return;
						
					} /* Check content urn failed */

					return;
					
				} /* Download not active queued, continue as normal */
				d->status = GTA_DL_HEADERS;
			}
		} /* ack_code was not 503 */
	}

	update_available_ranges(d, header);		/* Updates `d->ranges' */

	delay = extract_retry_after(header);
	d->retry_after = (delay > 0) ? (time(NULL) + delay) : 0;

	/*
	 * Partial File Sharing Protocol (PFSP) -- client-side
	 *
	 * We can make another request with a range that the remote
	 * servent has if the reply was a keep-alive one.  Both 503 or 416
	 * replies are possible with PFSP.
	 */

	if (d->ranges && d->keep_alive && d->file_info->use_swarming) {
		switch (ack_code) {
		case 503:				/* Range not available, maybe */
		case 416:				/* Range not satisfiable */
			/*
			 * If we were requesting something that is already within the
			 * available ranges, then there is no need to go further.
			 */

			if (http_range_contains(d->ranges, d->skip, d->range_end - 1)) {
				if (dbg > 3)
					printf("PFSP currently requested chunk %u-%u from %s "
						"for \"%s\" already in the available ranges: %s\n",
						d->skip, d->range_end - 1,
						ip_port_to_gchar(download_ip(d), download_port(d)),
						download_outname(d), http_range_to_gchar(d->ranges));

				break;
			}

			/*
			 * Clear current request so we may pick whatever is available
			 * remotely by freeing the current chunk...
			 */

			file_info_clear_download(d, TRUE);		/* `d' is running */

			/* Ensure we're waiting for the right file */
			if (!check_content_urn(d, header))
				return;

			/* Update mesh -- we're about to return */
			if (!d->always_push && d->sha1)
				dmesh_add(d->sha1, ip, port,
					d->record_index, d->file_name, 0);

			if (!download_start_prepare_running(d))
				return;

			/*
			 * If we can pick an available range, re-issue the request.
			 * Due to the above check for a request made for an already
			 * existing range, we won't loop re-requesting chunks forever
			 * if 503 meant "Busy" and not "Range not available".
			 *
			 * As a further precaution, to avoid hammering, we check
			 * whether there is a Retry-After header.  If there is,
			 * `delay' won't be 0 and we will not try to make the request.
			 */

			if (delay == 0 && download_pick_available(d)) {
				/*
				 * Sink the data that might have been returned with the
				 * HTTP status.  When it's done, we'll send the request
				 * with the chunk we have chosen.
				 */

				buf = header_get(header, "Content-Length");	/* Mandatory */

				if (buf == NULL) {
					g_warning("no Content-Length with keep-alive reply "
						"%d \"%s\" from %s <%s>", ack_code, ack_message,
						ip_port_to_gchar(download_ip(d), download_port(d)),
						download_vendor_str(d));
					download_queue_delay(d,
						MAX(delay, download_retry_refused_delay),
						"Partial file, bad HTTP keep-alive support");
					return;
				}
				
				d->sinkleft = (guint32) atol(buf);

				if (d->sinkleft > DOWNLOAD_MAX_SINK) {
					g_warning("too much data to sink (%u bytes) on reply "
						"%d \"%s\" from %s <%s>", d->sinkleft,
						ack_code, ack_message,
						ip_port_to_gchar(download_ip(d), download_port(d)),
						download_vendor_str(d));
					download_queue_delay(d,
						MAX(delay, download_retry_refused_delay),
						"Partial file, too much data to sink (%u bytes)",
						d->sinkleft);
					return;
				}

				/*
				 * Avoid endless request/sinking cycles.  If we already sunk
				 * data previously since we started the connection, requeue.
				 */

				if (d->flags & DL_F_SUNK_DATA) {
					g_warning("would have to sink twice during session "
						"from %s <%s>",
						ip_port_to_gchar(download_ip(d), download_port(d)),
						download_vendor_str(d));
					download_queue_delay(d,
						MAX(delay, download_retry_refused_delay),
						"Partial file, no suitable range found yet");
					return;
				}

				io_free(d->io_opaque);
				getline_free(s->getline);	/* No longer need this */
				s->getline = NULL;

				d->flags |= DL_F_CHUNK_CHOSEN;
				d->flags |= DL_F_SUNK_DATA;		/* Sink only once per session */

				if (d->sinkleft == 0 || d->sinkleft == s->pos) {
					s->pos = 0;
					download_send_request(d);
				} else {
					g_assert(s->gdk_tag == 0);
					g_assert(d->bio == NULL);

					d->status = GTA_DL_SINKING;

					d->bio = bsched_source_add(bws.in, s->file_desc,
						BIO_F_READ, download_sink_read, (gpointer) d);
				
					if (s->pos > 0)
						download_sink(d);

					gui_update_download(d, TRUE);
				}
			} else {
				/* Host might support queueing. If so, retreive queue status */
				/* Server has nothing for us yet, give it time */
				download_queue_delay(d,
					MAX(delay, download_retry_refused_delay),
					"Partial file on server, waiting");
			}
			
			return;
		default:
			break;
		}
	}

	if (ack_code >= 200 && ack_code <= 299) {
		/* OK -- Update mesh */
		if (!d->always_push && d->sha1)
			dmesh_add(d->sha1, ip, port, d->record_index, d->file_name, 0);

		/* If connection is not kept alive, remember it for this server */
		if (!d->keep_alive)
			d->server->attrs |= DLS_A_NO_KEEPALIVE;

		download_passively_queued(d, FALSE);
		download_actively_queued(d, FALSE);

		if (!ok) {
			download_queue_delay(d, download_retry_busy_delay,
				"%sHTTP %d %s", short_read, ack_code, ack_message);
			return;
		}
	} else {
		switch (ack_code) {
		case 301:				/* Moved permanently */
			if (!download_moved_permanently(d, header))
				break;
			download_passively_queued(d, FALSE);
			download_queue_delay(d,
				delay ? delay : download_retry_busy_delay,
				"%sHTTP %d %s", short_read, ack_code, ack_message);
			return;
		case 400:				/* Bad request */
		case 404:				/* Could be sent if /uri-res not understood */
		case 401:				/* Idem, /uri-res is "unauthorized" */
		case 403:				/* Idem, /uri-res is "forbidden" */
		case 410:				/* Idem, /uri-res is "gone" */
		case 500:				/* Server error */
		case 501:				/* Not implemented */
			/*
			 * If we sent a "GET /uri-res/N2R?" and the remote
			 * server does not support it, then retry without it.
			 */
			if (download_retry_no_urires(d, delay, ack_code))
				return;
			break;
		case 416:				/* Requested range not available */
			/*
			 * There was no ranges supplied (or we'd have gone through the
			 * PFSP code above), yet the server is sharing a partial file.
			 * Give it some time and retry.
			 */

			/* Make sure we're waiting for the right file, collect alt-locs */
			if (!check_content_urn(d, header))
				return;

			download_passively_queued(d, FALSE);
			download_queue_hold(d,
				delay ? delay : download_retry_timeout_delay,
				"%sRequested range unavailable yet", short_read);
			return;
		case 503:				/* Busy */			
			/* Make sure we're waiting for the right file, collect alt-locs */
			if (!check_content_urn(d, header))
				return;
			/*
			 * If we made a follow-up request, mark host as not reliable.
			 *
			 * We know 503 means really "busy" here and not "range not
			 * available" because we already checked for PFSP above.
			 */

			if (is_followup)
				d->server->attrs |= DLS_A_NO_KEEPALIVE;

			/* FALL THROUGH */
		case 408:				/* Request timeout */
			/* Update mesh */
			if (!d->always_push && d->sha1)
				dmesh_add(d->sha1, ip, port, d->record_index, d->file_name, 0);

			/* 
			 * We did a fall through on a 503, however, the download could be
			 * queued remotely. We might want to display this.
			 *		-- JA, 21/03/2003 (it is spring!)
			 */
			if (parq_download_is_passive_queued(d)) {
				download_passively_queued(d, TRUE);
				download_queue_delay(d,
					delay ? delay : download_retry_busy_delay,
						"Queued (slot %d/%d) ETA: %s",
							get_parq_dl_position(d),
							get_parq_dl_queue_length(d),
							short_time(get_parq_dl_eta(d))
				);
			} else {
				/* No hammering -- hold further requests on server */
				download_passively_queued(d, FALSE);
				download_queue_hold(d,
					delay ? delay : download_retry_busy_delay,
					"%sHTTP %d %s", short_read, ack_code, ack_message);
			}
			return;
		case 550:				/* Banned */
			download_passively_queued(d, FALSE);
			download_queue_hold(d,
				delay ? delay : download_retry_refused_delay,
				"%sHTTP %d %s", short_read, ack_code, ack_message);
			return;
		default:
			break;
		}

		download_bad_source(d);

		if (ancient_version)
			goto report_error;		/* Old versions don't circumvent banning */

		/*
		 * Check whether server is banning us based on our user-agent.
		 *
		 * If server is a gtk-gnutella, it's not banning us based on that.
		 * Note that if the remote server is a fake GTKG, then its name
		 * will begin with a '!'.
		 *
		 * When remote host is a GTKG, it can't be banning us based on
		 * our user-agent.  So clear the DLS_A_BANNING flag, which could
		 * have been activated previously because the remote host was
		 * looking as a fake GTKG due to a de-synchronized clock.
		 */

		if (0 == strncmp(download_vendor_str(d), "gtk-gnutella/", 13)) {
			gboolean was_banning = d->server->attrs & DLS_A_BANNING;

			d->server->attrs &= ~DLS_A_BANNING;
			d->server->attrs &= ~DLS_A_MINIMAL_HTTP;
			d->server->attrs &= ~DLS_A_FAKE_G2;

			if (was_banning)
				gui_update_download_server(d);

		} else if (!(d->server->attrs & DLS_A_BANNING)) {
			switch (ack_code) {
			case 401:
				if (0 != strncmp(download_vendor_str(d), "BearShare", 9))
					d->server->attrs |= DLS_A_BANNING;	/* Probably */
				break;
			case 403:
				if (0 == strncmp(ack_message, "Network Disabled", 16))
					d->server->attrs |= DLS_A_FAKE_G2;
				d->server->attrs |= DLS_A_BANNING;		/* Probably */
				break;
			case 404:
				if (0 == strncmp(ack_message, "Please Share", 12))
					d->server->attrs |= DLS_A_BANNING;	/* Shareaza 1.8.0.0- */
				break;
			}

			/*
			 * If server might be banning us, use minimal HTTP headers
			 * in our requests from now on.
			 */

			if (d->server->attrs & DLS_A_BANNING) {
				d->server->attrs |= DLS_A_MINIMAL_HTTP;
				g_warning("server \"%s\" at %s might be banning us",
					download_vendor_str(d),
					ip_port_to_gchar(download_ip(d), download_port(d)));

				download_queue_delay(d,
					delay ? delay : download_retry_busy_delay,
					"%sHTTP %d %s", short_read, ack_code, ack_message);

				return;
			}
		}

		/*
		 * If they refuse our downloads, ban them in return for a limited
		 * amout of time and kill all their running uploads.
		 */

		refusing = FALSE;

		switch (ack_code) {
		case 401:
			refusing = TRUE;
			break;
		case 403:
		case 404:
			/*
			 * By only selecting servers that are banning us, we are not
			 * banning gtk-gnutella clients that could be refusing us because
			 * we're too old.  But this is fair, as the user is told about
			 * his revision being too old and decided not to act.
			 *		--RAM, 13/07/2003
			 */
			if (d->server->attrs & DLS_A_BANNING)
				refusing = TRUE;
			break;
		}

		if (refusing) {
			ban_record(download_ip(d), "IP denying uploads");
			upload_kill_ip(download_ip(d));
		}

	report_error:
		download_stop(d, GTA_DL_ERROR,
			"%sHTTP %d %s", short_read, ack_code, ack_message);
		return;
	}

	/*
	 * We got a success status from the remote servent.	Parse header.
	 */

	g_assert(ok);

	/*
	 * Even upon a 2xx reply, a PARQ-compliant server may send us an ID.
	 * That ID will be used when the server sends us a QUEUE, so it's good
	 * to remember it.
	 *		--RAM, 17/05/2003
	 */

	(void) parq_download_parse_queue_status(d, header);

	/*
	 * If an URN is present, validate that we can continue this download.
 	 */
 
 	if (!check_content_urn(d, header))
 		return;


	/*
	 * If they configured us to require a server name, and we have none
	 * at this stage, stop.
	 */

	if (download_require_server_name && download_vendor(d) == NULL) {
		download_bad_source(d);
		download_stop(d, GTA_DL_ERROR, "Server did not supply identification");
		return;
	}

	/*
	 * Normally, a Content-Length: header is mandatory.	However, if we
	 * get a valid Content-Range, relax that constraint a bit.
	 *		--RAM, 08/01/2002
	 */

	requested_size = d->range_end - d->skip + d->overlap_size;

	buf = header_get(header, "Content-Length");		/* Mandatory */
	if (buf) {
		guint32 content_size = atol(buf);
		if (content_size == 0) {
			download_bad_source(d);
			download_stop(d, GTA_DL_ERROR, "Zero Content-Length");
			return;
		} else if (content_size != requested_size) {
			if (content_size == fi->size) {
				g_warning("File '%s': server seems to have "
					"ignored our range request of %u-%u.",
					d->file_name, d->skip - d->overlap_size, d->range_end - 1);
				download_bad_source(d);
				download_stop(d, GTA_DL_ERROR,
					"Server can't handle resume request");
				return;
			} else
				check_content_range = content_size;	/* Need Content-Range */
		}
		got_content_length = TRUE;
	}

	buf = header_get(header, "Content-Range");		/* Optional */
	if (buf) {
		guint32 start, end, total;

		g_strdown(buf);				/* Normalize case */
		if (
			sscanf(buf, "bytes %d-%d/%d", &start, &end, &total) ||	/* Good */
			sscanf(buf, "bytes=%d-%d/%d", &start, &end, &total)		/* Bad! */
		) {
			if (start != d->skip - d->overlap_size) {
				g_warning("file '%s' on %s (%s): "
					"start byte mismatch: wanted %u, got %u",
					d->file_name,
					ip_port_to_gchar(download_ip(d), download_port(d)),
					download_vendor_str(d),
					d->skip - d->overlap_size, start);
				download_bad_source(d);
				download_stop(d, GTA_DL_ERROR, "Range start mismatch");
				return;
			}
			if (total != fi->size) {
				g_warning("file '%s' on %s (%s): "
					"file size mismatch: expected %u, got %u",
					d->file_name,
					ip_port_to_gchar(download_ip(d), download_port(d)),
					download_vendor_str(d),
					fi->size, total);
				download_bad_source(d);
				download_stop(d, GTA_DL_ERROR, "File size mismatch");
				return;
			}
			if (end > d->range_end - 1) {
				g_warning("file '%s' on %s (%s): "
					"end byte too large: expected %u, got %u",
					d->file_name,
					ip_port_to_gchar(download_ip(d), download_port(d)),
					download_vendor_str(d),
					d->range_end - 1, end);
				download_bad_source(d);
				download_stop(d, GTA_DL_ERROR, "Range end too large");
				return;
			}
			if (end < d->range_end - 1) {
				g_warning("file '%s' on %s (%s): "
					"end byte short: wanted %u, got %u (continuing anyway)",
					d->file_name,
					ip_port_to_gchar(download_ip(d), download_port(d)),
					download_vendor_str(d),
					d->range_end - 1, end);

				/*
				 * Since we're getting less than we asked for, we need to
				 * update the end/size information and mark as DL_CHUNK_EMPTY
				 * the trailing part of the range we won't be getting.
				 *		-- RAM, 15/05/2003
				 */

				file_info_clear_download(d, TRUE);
				if (d->skip != end + 1)
					file_info_update(d, d->skip, end + 1, DL_CHUNK_BUSY);

				d->range_end = end + 1;				/* The new end */
				d->size = d->range_end - d->skip;	/* Don't count overlap */
				d->flags |= DL_F_SHRUNK_REPLY;		/* Remember shrinking */

				gui_update_download_range(d);
			}
			got_content_length = TRUE;
			check_content_range = 0;		/* We validated the served range */
		} else
			g_warning("File '%s': malformed Content-Range: %s",
				d->file_name, buf);
	}

	/*
	 * If we needed a Content-Range to validate the served range,
	 * but we didn't have any or could not parse it, abort!
	 */

	if (check_content_range != 0) {
		g_warning("File '%s': expected content of %u, server %s (%s) said %u",
			d->file_name, requested_size,
			ip_port_to_gchar(download_ip(d), download_port(d)),
			download_vendor_str(d),
			check_content_range);

		download_bad_source(d);
		download_stop(d, GTA_DL_ERROR, "Content-Length mismatch");
		return;
	}

	/*
	 * If neither Content-Length nor Content-Range was seen, abort!
	 *
	 * If we were talking to an official web-server, we'd assume the length
	 * to be correct and would be reading until EOF, but we're talking to
	 * an unknown party, that we cannot trust too much.
	 *		--RAM, 09/01/2002
	 */

	if (!got_content_length) {
		const char *ua = header_get(header, "Server");
		ua = ua ? ua : header_get(header, "User-Agent");
		if (ua)
			g_warning("server \"%s\" did not send any length indication", ua);
		download_bad_source(d);
		download_stop(d, GTA_DL_ERROR, "No Content-Length header");
		return;
	}

	/*
	 * Since we may request some overlap, ensure that the server did not
	 * shrink our request to just the overlap range!
	 *		--RAM, 14/10/2003
	 */

	g_assert(d->size >= 0);

	if (d->size == 0) {
		g_assert(d->flags & DL_F_SHRUNK_REPLY);
		download_queue_delay(d,
			MAX(delay, download_retry_busy_delay),
			"Partial file on server, waiting");
		return;
	}

	/*
	 * Open output file.
	 */

	g_assert(d->file_desc == -1);

	path = g_strdup_printf("%s/%s", fi->path, fi->file_name);
	g_return_if_fail(NULL != path);

	if (stat(path, &st) != -1) {
		/* File exists, we'll append the data to it */
		if (!fi->use_swarming && (fi->done != d->skip)) {
			g_warning("File '%s' changed size (now %u, but was %u)",
					fi->file_name, fi->done, d->skip);
			download_queue_delay(d, download_retry_stopped_delay,
				"Stopped (Output file size changed)");
			G_FREE_NULL(path);
			return;
		}

		d->file_desc = file_open(path, O_WRONLY);
	} else {
		if (!fi->use_swarming && d->skip) {
			download_stop(d, GTA_DL_ERROR, "Cannot resume: file gone");
			G_FREE_NULL(path);
			return;
		}
		d->file_desc = file_create(path, O_WRONLY,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH); /* 0644 */
	}

	if (d->file_desc == -1) {
		const gchar *error = g_strerror(errno);
		download_stop(d, GTA_DL_ERROR, "Cannot write into file: %s", error);
		G_FREE_NULL(path);
		return;
	}

	G_FREE_NULL(path);

	if (d->skip && d->skip != lseek(d->file_desc, d->skip, SEEK_SET)) {
		download_stop(d, GTA_DL_ERROR, "Unable to seek: %s",
			g_strerror(errno));
		return;
	}

	/*
	 * We're ready to receive.
	 */

	io_free(d->io_opaque);

	getline_free(s->getline);		/* No longer need this */
	s->getline = NULL;

	d->start_date = time((time_t *) NULL);
	d->status = GTA_DL_RECEIVING;

	if (fi->recvcount == 0) {		/* First source to begin receiving */
		fi->recv_last_time = d->start_date;
		fi->recv_last_rate = 0;
	}
	fi->recvcount++;
	fi->dirty_status = TRUE;

	g_assert(dl_establishing > 0);
	dl_establishing--;
	dl_active++;

	gnet_prop_set_guint32_val(PROP_DL_RUNNING_COUNT, count_running_downloads());
	gui_update_download(d, TRUE);
	gnet_prop_set_guint32_val(PROP_DL_ACTIVE_COUNT, dl_active);

	g_assert(s->gdk_tag == 0);
	g_assert(d->bio == NULL);

	d->bio = bsched_source_add(bws.in, s->file_desc,
		BIO_F_READ, download_read, (gpointer) d);

	/*
	 * Set TOS to low-delay, so that ACKs flow back faster, and set the RX
	 * buffer according to their preference (large for better throughput,
	 * small for better control of the incoming rate).
	 */

	socket_tos_lowdelay(s);
	sock_recv_buf(s, download_rx_size * 1024, TRUE);

	/*
	 * If we have something in the input buffer, write the data to the
	 * file immediately.  Note that this may close the download immediately
	 * if the whole file was already read in the socket buffer.
	 */

	if (s->pos > 0) {
		fi->recv_amount += s->pos;
		download_write_data(d);
	}
}

/*
 * download_incomplete_header
 *
 * Called when header reading times out.
 */
static void download_incomplete_header(struct download *d)
{
	header_t *header = io_header(d->io_opaque);

	download_request(d, header, FALSE);
}

/*
 * download_read
 *
 * Read callback for file data.
 */
static void download_read(gpointer data, gint source, inputevt_cond_t cond)
{
	struct download *d = (struct download *) data;
	struct gnutella_socket *s;
	gint32 r;
	gint32 to_read, remains;
	struct dl_file_info *fi;

	g_assert(d);
	g_assert(d->file_info->recvcount > 0);

	fi = d->file_info;
	s = d->socket;

	g_assert(s);
	g_assert(fi);

	if (cond & INPUT_EVENT_EXCEPTION) {		/* Treat as EOF */
		socket_eof(s);
		download_queue_delay(d, download_retry_stopped_delay,
			"Stopped data (EOF)");
		return;
	}

	g_assert(s->pos >= 0 && s->pos <= sizeof(s->buffer));

	if (s->pos == sizeof(s->buffer)) {
		download_queue_delay(d, download_retry_stopped_delay,
			"Stopped (Read buffer full)");
		return;
	}

	g_assert(d->pos <= fi->size);

	if (d->pos == fi->size) {
		download_stop(d, GTA_DL_ERROR, "Failed (Completed?)");
		return;
	}

	remains = sizeof(s->buffer) - s->pos;
	to_read = fi->size - d->pos;
	if (remains < to_read)
		to_read = remains;			/* Only read to fill buffer */

	r = bio_read(d->bio, s->buffer + s->pos, to_read);

	/*
	 * Don't hammer remote server if we get an EOF during data reception.
	 * The servent may have shutdown, or the user killed our download.
	 * The latter is not nice, but it's the user's choice.
	 *
	 * Therefore, we use the "busy" delay instead of the "stopped" delay.
	 */

	if (r <= 0) {
		if (r == 0) {
			socket_eof(s);
			download_queue_delay(d, download_retry_busy_delay,
				"Stopped data (EOF)");
		} else if (errno != EAGAIN) {
			socket_eof(s);
			if (errno == ECONNRESET)
				download_queue_delay(d, download_retry_busy_delay,
					"Stopped data (%s)", g_strerror(errno));
			else
				download_stop(d, GTA_DL_ERROR,
					"Failed (Read error: %s)", g_strerror(errno));
		}
		return;
	}

	s->pos += r;
	d->last_update = time((time_t *) 0);
	fi->recv_amount += r;

	g_assert(s->pos > 0);

	download_write_data(d);
}

/*
 * download_request_sent
 *
 * Called when the whole HTTP request has been sent out.
 */
static void download_request_sent(struct download *d)
{
	/*
	 * Update status and GUI.
	 */

	d->last_update = time((time_t *) 0);
	d->status = GTA_DL_REQ_SENT;
	tm_now(&d->header_sent);

	gui_update_download(d, TRUE);

	/*
	 * Now prepare to read the status line and the headers.
	 * XXX separate this to swallow 100 continuations?
	 */

	g_assert(d->io_opaque == NULL);

	io_get_header(d, &d->io_opaque, bws.in, d->socket, IO_SAVE_FIRST,
		call_download_request, download_start_reading, &download_io_error);
}

/*
 * download_write_request
 *
 * I/O callback invoked when we can write more data to the server to finish
 * sending the HTTP request.
 */
static void download_write_request(
	gpointer data, gint source, inputevt_cond_t cond)
{
	struct download *d = (struct download *) data;
	struct gnutella_socket *s = d->socket;
	http_buffer_t *r = d->req;
	gint sent;
	gint rw;
	gchar *base;

	g_assert(s->gdk_tag);		/* I/O callback still registered */
	g_assert(r != NULL);
	g_assert(d->status == GTA_DL_REQ_SENDING);

	if (cond & INPUT_EVENT_EXCEPTION) {
		/*
		 * If download is queued with PARQ, don't stop the download on a write
		 * error or we'd loose the PARQ ID, and the download entry.  If the
		 * server contacts us back with a QUEUE callback, we could be unable
		 * to resume!
		 *		--RAM, 14/07/2003
		 */

		static const gchar msg[] = "Could not send whole HTTP request";

		socket_eof(s);

		if (d->queue_status == NULL)
			download_stop(d, GTA_DL_ERROR, msg);
		else
			download_queue_delay(d, download_retry_busy_delay, msg);

		return;
	}

	rw = http_buffer_unread(r);			/* Data we still have to send */
	base = http_buffer_read_base(r);	/* And where unsent data start */

	if (-1 == (sent = bws_write(bws.out, s->file_desc, base, rw))) {
		/*
		 * If download is queued with PARQ, etc...  [Same as above]
		 */

		static const gchar msg[] = "Write failed: %s";

		if (d->queue_status == NULL)
			download_stop(d, GTA_DL_ERROR, msg, g_strerror(errno));
		else
			download_queue_delay(d, download_retry_busy_delay,
				msg, g_strerror(errno));
		return;
	} else if (sent < rw) {
		http_buffer_add_read(r, sent);
		return;
	} else if (dbg > 2) {
		printf("----Sent Request (%s) completely to %s (%d bytes):\n%.*s----\n",
			d->keep_alive ? "follow-up" : "initial",
			ip_port_to_gchar(download_ip(d), download_port(d)),
			http_buffer_length(r), http_buffer_length(r), http_buffer_base(r));
		fflush(stdout);
	}

	/*
	 * HTTP request was completely sent.
	 */

	if (dbg)
		g_warning("flushed partially written HTTP request to %s (%d bytes)",
			ip_port_to_gchar(download_ip(d), download_port(d)),
			http_buffer_length(r));
	 
	g_source_remove(s->gdk_tag);
	s->gdk_tag = 0;

	http_buffer_free(r);
	d->req = NULL;

	download_request_sent(d);
}

/*
 * download_send_request
 *
 * Send the HTTP request for a download, then prepare I/O reading callbacks
 * to read the incoming status line and following headers.
 *
 * NB: can stop the download, but does not return anything.
 */
void download_send_request(struct download *d)
{
	struct gnutella_socket *s = d->socket;
	struct dl_file_info *fi;
	gint rw;
	gint sent;
	gboolean n2r = FALSE;
	const gchar *sha1;

	g_assert(d);

	fi = d->file_info;

	g_assert(fi);
	g_assert(fi->lifecount > 0);
	g_assert(fi->lifecount <= fi->refcount);

	if (!s)
		g_error("download_send_request(): No socket for '%s'", d->file_name);

	/*
	 * If we have a hostname for this server, check the IP address of the
	 * socket with the one we have for this server: it may have changed if
	 * the remote server changed its IP address since last time we connected.
	 *		--RAM, 26/10/2003
	 */

	if (d->server->hostname != NULL && download_ip(d) != s->ip) {
		change_server_ip(d->server, s->ip);
		gui_update_download_host(d);
	}

	/*
	 * If we have d->always_push set, yet we did not use a Push, it means we
	 * finally tried to connect directly to this server.  And we succeeded!
	 *		-- RAM, 18/08/2002.
	 */

	if (d->always_push && !DOWNLOAD_IS_IN_PUSH_MODE(d)) {
		if (dbg > 2)
			printf("PUSH not necessary to reach %s\n",
				ip_port_to_gchar(download_ip(d), download_port(d)));
		d->server->attrs |= DLS_A_PUSH_IGN;
		d->always_push = FALSE;
	}

	/*
	 * If we're swarming, pick a free chunk.
	 * (will set d->skip and d->overlap_size).
	 */

	if (fi->use_swarming) {
		/*
		 * PFSP -- client side
		 *
		 * If we're retrying after a 503/416 reply from a servent
		 * supporting PFSP, then the chunk is already chosen.
		 */

		if (d->flags & DL_F_CHUNK_CHOSEN)
			d->flags &= ~DL_F_CHUNK_CHOSEN;
		else {
			if (d->ranges != NULL && download_pick_available(d))
				goto picked;

			http_range_free(d->ranges);		/* May have changed on server */
			d->ranges = NULL;				/* Request normally */

			if (!download_pick_chunk(d))
				return;
		}
	}

picked:

	g_assert(d->overlap_size <= sizeof(s->buffer));

	/*
	 * When we have a SHA1, the remote host normally supports HUGE, and
	 * therefore should understand our "GET /uri-res/N2R?" query.
	 * However, I'm suspicious, so we track our attempts and don't send
	 * the /uri-res when we have evidence the remote host does not support it.
	 *
	 * When we got a GIV request, don't take the chance that /uri-res be
	 * not understood and request the file.
	 *
	 *		--RAM, 14/06/2002
	 *
	 * If `record_index' is URN_INDEX, we only have the /uri-res/N2R? query,
	 * and therefore we don't flag the download with DL_F_URIRES: if the server
	 * does not understand those URLs, we won't have any fallback to use.
	 *
	 *		--RAM, 20/08/2002
	 */

	if (d->sha1) {
		if (d->record_index == URN_INDEX)
			n2r = TRUE;
		else if (!d->push && !(d->server->attrs & DLS_A_NO_URIRES)) {
			d->flags |= DL_F_URIRES;
			n2r = TRUE;
		}
	}

	if (!n2r)
		d->flags &= ~DL_F_URIRES;		/* Clear if not sending /uri-res/N2R? */

	d->flags &= ~DL_F_REPLIED;			/* Will be set if we get a reply */

	/*
	 * Tell GUI about the selected range, and that we're sending.
	 */

	d->status = GTA_DL_REQ_SENDING;
	d->last_update = time((time_t *) 0);

	gui_update_download_range(d);
	gui_update_download(d, TRUE);

	/*
	 * Build the HTTP request.
	 */

	if (n2r)
		rw = gm_snprintf(dl_tmp, sizeof(dl_tmp),
			"GET /uri-res/N2R?urn:sha1:%s HTTP/1.1\r\n",
			sha1_base32(d->sha1));
	else
		rw = gm_snprintf(dl_tmp, sizeof(dl_tmp),
			"GET /get/%u/%s HTTP/1.1\r\n",
			d->record_index, d->file_name);

	/*
	 * If URL is too large, abort.
	 */

	if (rw >= MAX_LINE_SIZE) {
		download_stop(d, GTA_DL_ERROR, "URL too large");
		return;
	}

	rw += gm_snprintf(&dl_tmp[rw], sizeof(dl_tmp)-rw,
		"Host: %s\r\n"
		"User-Agent: %s\r\n",
		ip_port_to_gchar(download_ip(d), download_port(d)),
		(d->server->attrs & DLS_A_BANNING) ?
			download_vendor_str(d) : version_string);

	header_features_generate(&xfeatures.downloads, dl_tmp, sizeof(dl_tmp), &rw);
	
	if (d->server->attrs & DLS_A_FAKE_G2)
		rw += gm_snprintf(&dl_tmp[rw], sizeof(dl_tmp)-rw,
			"X-Features: g2/1.0\r\n");

	if (!(d->server->attrs & DLS_A_BANNING))
		rw += gm_snprintf(&dl_tmp[rw], sizeof(dl_tmp)-rw,
			"X-Token: %s\r\n", tok_version());
	
	/*
	 * Add X-Queue / X-Queued information into the header
	 */
	parq_download_add_header(dl_tmp, sizeof(dl_tmp), &rw, d);

	/*
	 * If server is known to NOT support keepalives, then request only
	 * a range starting from d->skip.  Likewise if we don't know whether
	 * the server supports HTTP/1.1.
	 *
	 * Otherwise, we request a range and expect the server to keep the
	 * connection alive once the range has been fully served so that
	 * we may request the next chunk, if needed.
	 */

	g_assert(d->skip >= d->overlap_size);

	d->range_end = download_filesize(d);

	if (
		(d->server->attrs & (DLS_A_NO_KEEPALIVE|DLS_A_HTTP_1_1)) !=
			DLS_A_HTTP_1_1
	) {
		/* Request only a lower-bounded range, if needed */

		if (d->skip)
			rw += gm_snprintf(&dl_tmp[rw], sizeof(dl_tmp)-rw,
				"Range: bytes=%u-\r\n",
				d->skip - d->overlap_size);
	} else {
		/* Request exact range, unless we're asking for the full file */

		if (d->size != download_filesize(d)) {
			guint32 start = d->skip - d->overlap_size;

			d->range_end = d->skip + d->size;

			rw += gm_snprintf(&dl_tmp[rw], sizeof(dl_tmp)-rw,
				"Range: bytes=%u-%u\r\n",
				start, d->range_end - 1);
		}
	}

	g_assert(rw + 3 < sizeof(dl_tmp));		/* Should not have filled yet! */

	/*
	 * We can have a SHA1 for this download (information gathered from
	 * the query hit, or from a previous interaction with the server),
	 * or from the fileinfo metadata (if we don't have d->sha1 yet, it means
	 * we assigned the fileinfo based on name only).
	 *
	 * In any case, if we know a SHA1, we need to send it over.  If the server
	 * sees a mismatch, it will abort.
	 */

	sha1 = d->sha1;
	if (sha1 == NULL)
		sha1 = fi->sha1;

	if (sha1 != NULL) {
		gint wmesh;
		gint sha1_room;

		/*
		 * Leave room for the urn:sha1: possibly, plus final 2 * "\r\n".
		 */

		sha1_room = 33 + SHA1_BASE32_SIZE + 4;

		/*
		 * Send to the server any new alternate locations we may have
		 * learned about since the last time.
		 *
		 * Because the mesh header can be large, we use HTTP continuations
		 * to format it, but some broken servents do not know how to parse
		 * them.  Use minimal HTTP with those.
		 */

		if (d->server->attrs & DLS_A_MINIMAL_HTTP)
			wmesh = 0;
		else {
			gint altloc_size = sizeof(dl_tmp) - (rw + sha1_room);
			struct dl_file_info *file_info = d->file_info;

			/*
			 * If we're short on HTTP output bandwidth, limit the size of
			 * the alt-locs we send and don't provide our fileinfo, so that
			 * we don't generate an URL for ourselves (if PFSP-server is on)
			 * which would attract even more HTTP traffic.
			 *		--RAM, 12/10/2003
			 */

			if (bsched_saturated(bws.out)) {
				altloc_size = MIN(altloc_size, 160);
				file_info = NULL;
			}

			wmesh = dmesh_alternate_location(sha1,
				&dl_tmp[rw], altloc_size,
				download_ip(d), d->last_dmesh, download_vendor(d),
				file_info, TRUE);
			rw += wmesh;

			d->last_dmesh = (guint32) time(NULL);
		}

		/*
		 * HUGE specs says that the alternate locations are only defined
		 * when there is an X-Gnutella-Content-URN present.	When we use
		 * the N2R form to retrieve a resource by SHA1, that line is
		 * redundant.  We only send it if we sent mesh information.
		 */

		if (!n2r || wmesh)
			rw += gm_snprintf(&dl_tmp[rw], sizeof(dl_tmp)-rw,
				"X-Gnutella-Content-URN: urn:sha1:%s\r\n",
				sha1_base32(sha1));
	}

	rw += gm_snprintf(&dl_tmp[rw], sizeof(dl_tmp)-rw, "\r\n");

	/*
	 * Send the HTTP Request
	 */

	socket_tos_normal(s);

	if (-1 == (sent = bws_write(bws.out, s->file_desc, dl_tmp, rw))) {
		/*
		 * If the connection was flagged keep-alive, we were making
		 * a follow-up request but the server did not honour it and
		 * closed the connection (probably after serving the last byte
		 * of the previous request).
		 *		--RAM, 01/09/2002
		 */

		if (d->keep_alive)
			d->server->attrs |= DLS_A_NO_KEEPALIVE;

		/*
		 * If download is queued with PARQ, don't stop the download on a write
		 * error or we'd loose the PARQ ID, and the download entry.  If the
		 * server contacts us back with a QUEUE callback, we could be unable
		 * to resume!
		 *		--RAM, 17/05/2003
		 */

		if (d->queue_status == NULL)
			download_stop(d, GTA_DL_ERROR,
				"Write failed: %s", g_strerror(errno));
		else
			download_queue_delay(d, download_retry_busy_delay,
				"Write failed: %s", g_strerror(errno));
		return;
	} else if (sent < rw) {
		/*
		 * Could not send the whole request, probably because the TCP output
		 * path is clogged.
		 */

		g_warning("partial HTTP request write to %s: wrote %d out of %d bytes",
			ip_port_to_gchar(download_ip(d), download_port(d)),
			sent, rw);

		g_assert(d->req == NULL);

		d->req = http_buffer_alloc(dl_tmp, rw, sent);

		/*
		 * Install the writing callback.
		 */

		g_assert(s->gdk_tag == 0);

		s->gdk_tag = inputevt_add(s->file_desc,
			(inputevt_cond_t) INPUT_EVENT_WRITE | INPUT_EVENT_EXCEPTION,
			download_write_request, (gpointer) d);

		return;
	} else if (dbg > 2) {
		printf("----Sent Request (%s) to %s (%d bytes):\n%.*s----\n",
			d->keep_alive ? "follow-up" : "initial",
			ip_port_to_gchar(download_ip(d), download_port(d)),
			(int) rw, (int) rw, dl_tmp);
		fflush(stdout);
	}

	download_request_sent(d);
}

/*
 * download_push_ready
 *
 * Send download request on the opened connection.
 *
 * Header processing callback, invoked when we have read the second "\n" at
 * the end of the GIV string.
 */
static void download_push_ready(struct download *d, getline_t *empty)
{
	gint len = getline_length(empty);

	if (len != 0) {
		g_warning("File '%s': push reply was not followed by an empty line",
			d->file_name);
		dump_hex(stderr, "Extra GIV data", getline_str(empty), MIN(len, 80));
		download_stop(d, GTA_DL_ERROR, "Malformed push reply");
		return;
	}

	/*
	 * Free up the s->getline structure which holds the GIV line.
	 */

	g_assert(d->socket->getline);
	getline_free(d->socket->getline);
	d->socket->getline = NULL;

	io_free(d->io_opaque);
	download_send_request(d);		/* Will install new I/O data */
}

/*
 * select_push_download
 *
 * On reception of a "GIV index:GUID" string, select the appropriate download
 * to request.
 *
 * Returns the selected download, or NULL if we could not find one.
 */
static struct download *select_push_download(guint file_index, gchar *hex_guid)
{
	struct download *d = NULL;
	GSList *list;
	gchar rguid[16];		/* Remote GUID */
	gint i;
	time_t now;

	g_strdown(hex_guid);
	gm_snprintf(dl_tmp, sizeof(dl_tmp), "%u:%s", file_index, hex_guid);

	list = (GSList *) g_hash_table_lookup(pushed_downloads, (gpointer) dl_tmp);
	if (list) {
		d = (struct download *) list->data;			/* Take first entry */
		g_assert(d != NULL);
		g_assert(d->record_index == file_index);
	} else if (dbg > 3)
		printf("got unexpected GIV: nothing pending currently\n");

	/*
	 * We might get another GIV for the same download: we send two pushes
	 * in a row, and with the propagation delay, the first gets handled
	 * after we sent the second push.  We'll get a GIV for an already
	 * connected download.
	 *
	 * We check two things: that we're not already connected (has a socket)
	 * and that we're in a state where we can expect a GIV string.	Doing
	 * the two tests add robustness, since they are overlapping, but not
	 * completely equivalent (if we're in the queued state, for instance).
	 */

	if (d) {
		if (d->socket) {
			if (dbg > 3)
				printf("got concurrent GIV: download is connected, state %d\n",
					d->status);
			d = NULL;
		} else if (!DOWNLOAD_IS_EXPECTING_GIV(d)) {
			if (dbg > 3)
				printf("got GIV string for download in state %d\n",
					d->status);
			d = NULL;
		}
	}

	if (d) {
		g_assert(d->socket == NULL);
		return d;
	}

	/*
	 * Whilst we are connected to that servent, find a suitable download
	 * we could request.
	 */

	if (!hex_to_guid(hex_guid, rguid)) {
		g_warning("discarding GIV with malformed GUID %s", hex_guid);
		return NULL;
	}

	/*
	 * We do not limit by download slots for GIV... Indeed, pushes are
	 * precious little things.  We must peruse the connection we got
	 * because we don't know whether we'll be able to get another one.
	 * This is where it is nice that the remote end supports queuing... and
	 * PARQ will work either way (i.e. active or passive queuing, since
	 * then we'll get QUEUE callbacks).
	 *		--RAM, 19/07/2003
	 */

	/*
	 * Look for a queued download on this host that we could request.
	 */

	now = time(NULL);

	for (i = 0; i < DHASH_SIZE; i++) {
		GList *l;
		gint last_change;

	retry:
		l = dl_by_time.servers[i];
		last_change = dl_by_time.change[i];

		for (/* empty */; l; l = g_list_next(l)) {
			struct dl_server *server = (struct dl_server *) l->data;
			GList *w;

			g_assert(server != NULL);

			/*
			 * There might be several hosts with the same GUID (Mallory nodes).
			 */

			if (!guid_eq(rguid, server->key->guid))
				continue;

			/*
			 * Look for an active download for this host, expecting a GIV
			 * and not already gone through download_push_ack() i.e. not
			 * connected yet (downloads remain in the expecting state until
			 * they have read the trailing "\n" of the GIV, even though they
			 * are connected).
			 */

			for (w = server->list[DL_LIST_RUNNING]; w; w = g_list_next(w)) {
				struct download *d = (struct download *) w->data;

				g_assert(DOWNLOAD_IS_RUNNING(d));

				if (d->socket == NULL && DOWNLOAD_IS_EXPECTING_GIV(d)) {
					if (dbg > 2)
						printf("GIV: selected active download '%s' from %s\n",
							d->file_name, guid_hex_str(rguid));
					return d;
				}
			}

			/*
			 * No luck so far.  Look for waiting downloads for this host.
			 */

			if (count_running_on_server(server) >= max_host_downloads)
				continue;

			for (w = server->list[DL_LIST_WAITING]; w; w = g_list_next(w)) {
				struct download *d = (struct download *) w->data;

				g_assert(!DOWNLOAD_IS_RUNNING(d));

				if (
					!d->file_info->use_swarming &&
					count_running_downloads_with_name(download_outname(d)) != 0
				)
					continue;

				if (now < d->retry_after)
					break;		/* List is sorted */

				if (d->flags & DL_F_SUSPENDED)
					continue;

				if (dbg > 4)
					printf(
						"GIV: trying alternate download '%s' from %s at %s\n",
						d->file_name, guid_hex_str(rguid),
						ip_port_to_gchar(download_ip(d), download_port(d)));

				/*
				 * Only prepare the download, don't call download_start(): we
				 * already have the connection, and simply need to prepare the
				 * range offset.
				 */

				g_assert(d->socket == NULL);

				if (download_start_prepare(d)) {
					d->status = GTA_DL_CONNECTING;
					if (!DOWNLOAD_IS_VISIBLE(d))
						download_gui_add(d);

					gui_update_download(d, TRUE);
					gnet_prop_set_guint32_val(PROP_DL_ACTIVE_COUNT,
						dl_active);
					gnet_prop_set_guint32_val(PROP_DL_RUNNING_COUNT,
						count_running_downloads());

					if (dbg > 2)
						printf(
							"GIV: selected alternate download '%s' from %s\n",
							d->file_name, guid_hex_str(rguid));

					return d;
				}
			}

			/*
			 * If download_start_prepare() requeued something with a delay,
			 * the dl_by_time list has changed and we must restart the loop.
			 *		--RAM, 24/08/2002.
			 */

			if (last_change != dl_by_time.change[i])
				goto retry;
		}
	}

	g_warning("discarding GIV from %s: no suitable alternate found",
		guid_hex_str(rguid));

	return NULL;
}

/*
 * download_push_ack
 *
 * Initiate download on the remotely initiated connection.
 *
 * This is called when an incoming "GIV" request is received in answer to
 * some of our pushes.
 */
void download_push_ack(struct gnutella_socket *s)
{
	struct download *d = NULL;
	gchar *giv;
	guint file_index;		/* The requested file index */
	gchar hex_guid[33];		/* The hexadecimal GUID */

	g_assert(s->getline);
	giv = getline_str(s->getline);

	if (dbg > 4) {
		printf("----Got GIV from %s:\n", ip_to_gchar(s->ip));
		printf("%s\n", giv);
		printf("----\n");
		fflush(stdout);
	}

	/*
	 * To find out which download this is, we have to parse the incoming
	 * GIV request, which is stored in "s->getline".
	 */

	if (!sscanf(giv, "GIV %u:%32c/", &file_index, hex_guid)) {
		g_warning("malformed GIV string: %s", giv);
		g_assert(s->resource.download == NULL);	/* Hence socket_free() */
		socket_free(s);
		return;
	}

	/*
	 * Look for a recorded download.
	 */

	hex_guid[32] = '\0';
	d = select_push_download(file_index, hex_guid);
	if (!d) {
		g_warning("discarded GIV string: %s", giv);
		g_assert(s->resource.download == NULL);	/* Hence socket_free() */
		socket_free(s);
		return;
	}

	/*
	 * Install socket for the download.
	 */

	g_assert(d->socket == NULL);

	d->got_giv = TRUE;
	d->last_update = time((time_t *) NULL);
	d->socket = s;
	s->resource.download = d;

	/*
	 * Now we have to read that trailing "\n" which comes right afterwards.
	 */

	g_assert(NULL == d->io_opaque);
	io_get_header(d, &d->io_opaque, bws.in, s, IO_SINGLE_LINE,
		call_download_push_ready, NULL, &download_io_error);
}

void download_retry(struct download *d)
{
	g_assert(d != NULL);

	/* download_stop() sets the time, so all we need to do is set the delay */

	if (d->timeout_delay == 0)
		d->timeout_delay = download_retry_timeout_min;
	else {
		d->timeout_delay *= 2;
		if (d->start_date) {
			/* We forgive a little while the download is working */
			d->timeout_delay -=
				(time((time_t *) NULL) - d->start_date) / 10;
		}
	}

	if (d->timeout_delay < download_retry_timeout_min)
		d->timeout_delay = download_retry_timeout_min;
	if (d->timeout_delay > download_retry_timeout_max)
		d->timeout_delay = download_retry_timeout_max;

	if (d->push)
		d->timeout_delay = 1;	/* Must send pushes before route expires! */

	download_stop(d, GTA_DL_TIMEOUT_WAIT, NULL);
}

/*
 * download_find_waiting_unparq
 *
 * Find a waiting download on the specified server, identified by its IP:port
 * for which we have no PARQ information yet.
 *
 * Returns NULL if none, the download we found otherwise.
 */
struct download *download_find_waiting_unparq(guint32 ip, guint16 port)
{
	struct dl_server *server = get_server(blank_guid, ip, port);
	GList *w;

	if (server == NULL)
		return NULL;

	for (w = server->list[DL_LIST_WAITING]; w; w = g_list_next(w)) {
		struct download *d = (struct download *) w->data;

		g_assert(!DOWNLOAD_IS_RUNNING(d));

		if (d->flags & DL_F_SUSPENDED)		/* Suspended, cannot pick */
			continue;

		if (d->queue_status == NULL)		/* No PARQ information yet */
			return d;						/* Found it! */
	}

	return NULL;
}

/***
 *** Queue persistency routines
 ***/

static const gchar *download_file = "downloads";
static gboolean retrieving = FALSE;

/*
 * download_store
 *
 * Store all pending downloads that are not in PUSH mode (since we'll loose
 * routing information when we quit).
 *
 * The downloads are normally stored in ~/.gtk-gnutella/downloads.
 */
static void download_store(void)
{
	FILE *out;
	GSList *l;
	file_path_t fp;

	if (retrieving)
		return;

	file_path_set(&fp, settings_config_dir(), download_file);
	out = file_config_open_write(file_what, &fp);

	if (!out)
		return;

	file_config_preamble(out, "Downloads");

	fputs(	"#\n# Format is:\n"
			"#   File name\n"
			"#   size, index[:GUID], IP:port[, hostname]\n"
			"#   SHA1 or * if none\n"
			"#   PARQ id or * if none\n"
			"#   <blank line>\n"
			"#\n\n" 
			"RECLINES=4\n\n", out);

	for (l = sl_downloads; l; l = g_slist_next(l)) {
		struct download *d = (struct download *) l->data;
		gchar *id;
		gchar *guid;
		const gchar *hostname;

		g_assert(d != NULL);

		if (d->status == GTA_DL_DONE || d->status == GTA_DL_REMOVED)
			continue;
		if (d->always_push)
			continue;

		id = get_parq_dl_id(d);
		guid = has_blank_guid(d) ? NULL : download_guid(d);
		hostname = d->server->hostname;

		fprintf(out,
			"%s\n"
			"%u, %u%s%s, %s%s%s\n"
			"%s\n"
			"%s\n\n",
			d->escaped_name,
			d->file_info->size, d->record_index,
			guid == NULL ? "" : ":", guid == NULL ? "" : guid_hex_str(guid),
			ip_port_to_gchar(download_ip(d), download_port(d)),
			hostname == NULL ? "" : ", ", hostname == NULL ? "" : hostname,
			d->file_info->sha1 ? sha1_base32(d->file_info->sha1) : "*",
			id != NULL ? id : "*"
		);
	}

	file_config_close(out, &fp);
	download_dirty = FALSE;
}

/*
 * download_store_if_dirty
 *
 * Store pending download if needed.
 *
 * The fileinfo database is also flushed if dirty, but only when the
 * downloads themselves are stored.  Since both are linked via SHA1 and name,
 * it's best to try to keep them in sync.
 */
void download_store_if_dirty(void)
{
	if (download_dirty) {
		download_store();
		file_info_store_if_dirty();
	}
}

/*
 * download_retrieve
 *
 * Retrieve download list and requeue each download.
 * The downloads are normally retrieved from ~/.gtk-gnutella/downloads.
 */
static void download_retrieve(void)
{
	FILE *in;
	gchar d_guid[16];		/* The d_ vars are what we deserialize */
	guint32 d_size;
	gchar *d_name;
	guint32 d_ip;
	guint16 d_port;
	guint32 d_index;
	gchar d_hexguid[33];
	gchar d_ipport[23];		/* IP:port + possible hostname */
	gchar d_hostname[256];	/* Server hostname */
	gint recline;			/* Record line number */
	gint line;				/* File line number */
	gchar sha1_digest[SHA1_RAW_SIZE];
	gboolean has_sha1 = FALSE;
	gint maxlines = -1;
	file_path_t fp;
	gboolean allow_comments = TRUE;
	gchar *parq_id = NULL;
	struct download *d;

	file_path_set(&fp, settings_config_dir(), download_file);
	in = file_config_open_read(file_what, &fp, 1);

	if (!in)
		return;

	/*
	 * Retrieval algorithm:
	 *
	 * Lines starting with a # are skipped.
	 *
	 * We read the ines that make up each serialized record, and
	 * recreate the download.  We stop as soon as we encounter an
	 * error.
	 */

	retrieving = TRUE;			/* Prevent download_store() runs */

	line = recline = 0;
	d_name = NULL;

	while (fgets(dl_tmp, sizeof(dl_tmp) - 1, in)) { /* Room for trailing NUL */
		line++;

		if (dl_tmp[0] == '#' && allow_comments)
			continue;				/* Skip comments */

		/*
		 * We emitted a "RECLINES=x" at store time to indicate the amount of
		 * lines each record takes.  This also signals that we can no longer
		 * accept comments.
		 */

		if (maxlines < 0 && dl_tmp[0] == 'R') {
			if (1 == sscanf(dl_tmp, "RECLINES=%d", &maxlines)) {
				allow_comments = FALSE;
				continue;
			}
		}

		if (dl_tmp[0] == '\n') {
			if (recline == 0)
				continue;			/* Allow arbitrary blank lines */

			g_warning("download_retrieve: "
				"Unexpected empty line #%d, aborting", line);
			goto out;
		}

		recline++;					/* We're in a record */

		switch (recline) {
		case 1:						/* The file name */
			(void) str_chomp(dl_tmp, 0);
			(void) url_unescape(dl_tmp, TRUE);	/* Un-escape in place */
			d_name = atom_str_get(dl_tmp);

			/*
			 * Backward compatibility with 0.85, which did not have the
			 * "RECLINE=x" line.  If we reached the first record line, then
			 * either we saw that line in recent versions, or we did not and
			 * we know we had only 2 lines per record.
			 */

			if (maxlines < 0)
				maxlines = 2;

			continue;
		case 2:						/* Other information */
			g_assert(d_name);
			d_hostname[0] = '\0';

			if (
				sscanf(dl_tmp, "%u, %u, %22s %255s",
					&d_size, &d_index, d_ipport, d_hostname) == 4
			) {
				memset(d_hexguid, '0', 32);		/* GUID missing -> blank */
				d_hexguid[32] = '\0';
			}
			else if (
				sscanf(dl_tmp, "%u, %u, %22s",
					&d_size, &d_index, d_ipport) == 3
			) {
				memset(d_hexguid, '0', 32);		/* GUID missing -> blank */
				d_hexguid[32] = '\0';
			}
			else if (
				sscanf(dl_tmp, "%u, %u:%32c, %22s %255s",
					&d_size, &d_index, d_hexguid, d_ipport, d_hostname) == 5
			) {
				/* empty */
			}
			else if (
				sscanf(dl_tmp, "%u, %u:%32c, %22s",
					&d_size, &d_index, d_hexguid, d_ipport) < 4
			) {
				(void) str_chomp(dl_tmp, 0);
				g_warning("download_retrieve: "
					"cannot parse line #%d: %s", line, dl_tmp);
				goto out;
			}

			d_ipport[22] = '\0';

			if (!gchar_to_ip_port(d_ipport, &d_ip, &d_port)) {
				g_warning("download_retrieve: "
					"bad IP:port '%s' at line #%d, aborting", d_ipport, line);
				goto out;
			}

			if (maxlines == 2)
				break;
			continue;
		case 3:						/* SHA1 hash, or "*" if none */
			if (dl_tmp[0] == '*')
				goto no_sha1;
			if (
				strlen(dl_tmp) != (1+SHA1_BASE32_SIZE) ||	/* Final "\n" */
				!base32_decode_into(dl_tmp, SHA1_BASE32_SIZE,
					sha1_digest, sizeof(sha1_digest))
			) {
				g_warning("download_retrieve: "
					"bad base32 SHA1 '%32s' at line #%d, ignoring",
					dl_tmp, line);
			} else
				has_sha1 = TRUE;
		no_sha1:
			if (maxlines == 3)
				break;
			continue;
		case 4:						/* PARQ id, or "*" if none */
			if (maxlines != 4) {
				g_warning("download_retrieve: "
					"can't handle %d lines in records, aborting", maxlines);
				goto out;
			}
			if (dl_tmp[0] != '*') {
				(void) str_chomp(dl_tmp, 0);		/* Strip final "\n" */
				parq_id = g_strdup(dl_tmp);
			}
			break;
		default:
			g_warning("download_retrieve: "
				"Too many lines for record at line #%d, aborting", line);
			goto out;
		}

		/*
		 * At the last line of the record.
		 */

		if (!hex_to_guid(d_hexguid, d_guid))
			g_warning("download_rerieve: malformed GUID %s near line #%d",
				d_hexguid, line);

		/*
		 * Download is created with a timestamp of `1' so that it is very
		 * old and the entry does not get added to the download mesh yet.
		 */

		d = create_download(d_name, d_size, d_index, d_ip, d_port, d_guid,
			d_hostname, has_sha1 ? sha1_digest : NULL, 1, FALSE, FALSE,
			NULL, NULL);

		if (d == NULL) {
			g_warning("ignored dup download at line #%d (server %s)",
				line - maxlines + 1, ip_port_to_gchar(d_ip, d_port));
			goto next_entry;
		}

		/*
		 * Record PARQ id if present, so we may answer QUEUE callbacks.
		 */

		if (parq_id != NULL) {
			d->queue_status = parq_dl_create(d);
			parq_dl_add_id(d, parq_id);
		}

		/*
		 * Don't free `d_name', we gave it to create_download()!
		 */

	next_entry:
		d_name = NULL;
		recline = 0;				/* Mark the end */
		has_sha1 = FALSE;
		if (parq_id != NULL) {
			G_FREE_NULL(parq_id);
		}
	}

out:
	retrieving = FALSE;			/* Re-enable download_store() runs */

	if (d_name)
		atom_str_free(d_name);

	fclose(in);
	download_store();			/* Persist what we have retrieved */
}

/*
 * download_moved_with_bad_sha1
 *
 * Post renaming/moving routine called when download had a bad SHA1.
 */
static void download_moved_with_bad_sha1(struct download *d)
{
	g_assert(d);
	g_assert(d->status == GTA_DL_DONE);
	g_assert(!has_good_sha1(d));

	queue_suspend_downloads_with_file(d->file_info, FALSE);

	/*
	 * If it was a faked download, we cannot resume.
	 */

	if (is_faked_download(d)) {
		g_warning("SHA1 mismatch for \"%s\", and cannot restart download",
			download_outname(d));
	} else {
		g_warning("SHA1 mismatch for \"%s\", will be restarting download",
			download_outname(d));

		d->file_info->lifecount++;				/* Reactivate download */
		file_info_reset(d->file_info);
		download_queue(d, _("SHA1 mismatch detected"));
	}
}

/***
 *** Download moving routines.
 ***/

/*
 * download_move
 *
 * Main entry point to move the completed file `d' to target directory `dir'.
 *
 * In case the target directory is the same as the source, the file is
 * simply renamed with the extension `ext' appended to it.
 */
static void download_move(
	struct download *d, const gchar *dir, const gchar *ext)
{
	struct dl_file_info *fi;
	gchar *dest = NULL;
	gchar *src = NULL;
	gboolean common_dir;
	gchar *name;

	g_assert(d);
	g_assert(FILE_INFO_COMPLETE(d->file_info));
	g_assert(DOWNLOAD_IS_STOPPED(d));

	d->status = GTA_DL_MOVING;
	fi = d->file_info;

	src = g_strdup_printf("%s/%s", fi->path, fi->file_name);
	if (NULL == src)
		goto error;

	/*
	 * Don't keep an URN-like name when the file is done, if possible.
	 */

	name = file_info_readable_filename(fi);

	/*
	 * If the target directory is the same as the source directory, we'll
	 * use the supplied extension and simply rename the file.
	 */

	if (0 == strcmp(dir, fi->path)) {
		dest = unique_filename(dir, name, ext);
		if (NULL == dest || -1 == rename(src, dest))
			goto error;
		goto renamed;
	}

	/*
	 * Try to rename() the file, in case both the source and the target
	 * directory are on the same filesystem.  We usually ignore `ext' at
	 * this point since we know the target directory is distinct from the
	 * source, unless the good/bad directories are identical.
	 */

	common_dir = (0 == strcmp(move_file_path, bad_file_path));

	dest = unique_filename(dir, name, common_dir ? ext : "");
	if (NULL == dest)
		goto error;

	if (-1 != rename(src, dest))
		goto renamed;

	/*
	 * The only error we allow is EXDEV, meaning the source and the
	 * target are not on the same file system.
	 */

	if (errno != EXDEV)
		goto error;

	/*
	 * Have to move the file asynchronously.
	 */

	d->status = GTA_DL_MOVE_WAIT;
	move_queue(d, dir, common_dir ? ext : "");

	if (!DOWNLOAD_IS_VISIBLE(d))
		download_gui_add(d);

	gui_update_download(d, TRUE);

	goto cleanup;

error:
	g_warning("cannot rename %s as %s: %s", src, dest, g_strerror(errno));
	download_move_error(d);
	goto cleanup;

renamed:

	file_info_strip_binary_from_file(fi, dest);
	download_move_done(d, 0);
	goto cleanup;

cleanup:

	if (NULL != src)
		G_FREE_NULL(src);
	if (NULL != dest)
		G_FREE_NULL(dest);
	return;
}

/*
 * download_move_start
 *
 * Called when the moving daemon task starts processing a download.
 */
void download_move_start(struct download *d)
{
	g_assert(d->status == GTA_DL_MOVE_WAIT);

	d->status = GTA_DL_MOVING;
	d->file_info->copied = 0;

	gui_update_download(d, TRUE);
}

/*
 * download_move_progress
 *
 * Called to register the current moving progress.
 */
void download_move_progress(struct download *d, guint32 copied)
{
	g_assert(d->status == GTA_DL_MOVING);

	d->file_info->copied = copied;
}

/*
 * download_move_done
 *
 * Called when file has been moved/renamed with its fileinfo trailer stripped.
 */
void download_move_done(struct download *d, time_t elapsed)
{
	struct dl_file_info *fi = d->file_info;

	g_assert(d->status == GTA_DL_MOVING);

	d->status = GTA_DL_DONE;
	fi->copy_elapsed = elapsed;
	gui_update_download(d, TRUE);

	/*
	 * File was unlinked by rename() if we were on the same filesystem,
	 * or by the moving daemon task upon success.
	 */

	if (!has_good_sha1(d))
		download_moved_with_bad_sha1(d);
}

/*
 * download_move_error
 *
 * Called when we cannot move the file (I/O error, etc...).
 */
void download_move_error(struct download *d)
{
	struct dl_file_info *fi = d->file_info;
	const gchar *ext;
	gchar *src;
	gchar *dest;
	gchar *name;

	g_assert(d->status == GTA_DL_MOVING);

	/*
	 * If download is "good", rename it inplace as DL_OK_EXT, otherwise
	 * rename it as DL_BAD_EXT.
	 *
	 * Don't keep an URN-like name when the file is done, if possible.
	 */

	name = file_info_readable_filename(fi);

	src = g_strdup_printf("%s/%s", fi->path, fi->file_name);
	ext = has_good_sha1(d) ? DL_OK_EXT : DL_BAD_EXT;
	dest = unique_filename(fi->path, name, ext);

	file_info_strip_binary(fi);

	if (NULL == src || NULL == dest || -1 == rename(src, dest)) {
		g_warning("could not rename \"%s\" as \"%s\": %s",
			src, dest, g_strerror(errno));
		d->status = GTA_DL_DONE;
	} else {
		g_warning("completed \"%s\" left at \"%s\"", name, dest);
		download_move_done(d, 0);
	}
	if (NULL != src)
		G_FREE_NULL(src);
	if (NULL != dest)
		G_FREE_NULL(dest);
}

/***
 *** SHA1 verification routines.
 ***/

/*
 * download_verify_sha1
 *
 * Main entry point for verifying the SHA1 of a completed download.
 */
static void download_verify_sha1(struct download *d)
{
	g_assert(d);
	g_assert(FILE_INFO_COMPLETE(d->file_info));
	g_assert(DOWNLOAD_IS_STOPPED(d));
	g_assert(!DOWNLOAD_IS_VERIFYING(d));
	g_assert(!(d->flags & DL_F_SUSPENDED));
	g_assert(d->list_idx == DL_LIST_STOPPED);

	/*
	 * Even if download was aborted or in error, we have a complete file
	 * anyway, so start verifying its SHA1.
	 */

	d->status = GTA_DL_VERIFY_WAIT;

	queue_suspend_downloads_with_file(d->file_info, TRUE);
	verify_queue(d);

	if (!DOWNLOAD_IS_VISIBLE(d))
		download_gui_add(d);

	gui_update_download(d, TRUE);
}

/*
 * download_verify_start
 *
 * Called when the verification daemon task starts processing a download.
 */
void download_verify_start(struct download *d)
{
	g_assert(d->status == GTA_DL_VERIFY_WAIT);
	g_assert(d->list_idx == DL_LIST_STOPPED);

	d->status = GTA_DL_VERIFYING;
	d->file_info->cha1_hashed = 0;

	gui_update_download(d, TRUE);
}

/*
 * download_verify_progress
 *
 * Called to register the current verification progress.
 */
void download_verify_progress(struct download *d, guint32 hashed)
{
	g_assert(d->status == GTA_DL_VERIFYING);
	g_assert(d->list_idx == DL_LIST_STOPPED);

	d->file_info->cha1_hashed = hashed;
}

/*
 * download_verify_done
 *
 * Called when download verification is finished and digest is known.
 */
void download_verify_done(struct download *d, gchar *digest, time_t elapsed)
{
	struct dl_file_info *fi = d->file_info;
	gchar *name = file_info_readable_filename(fi);

	g_assert(d->status == GTA_DL_VERIFYING);
	g_assert(d->list_idx == DL_LIST_STOPPED);

	fi->cha1 = atom_sha1_get(digest);
	fi->cha1_elapsed = elapsed;
	file_info_store_binary(fi);		/* Resync with computed SHA1 */

	d->status = GTA_DL_VERIFIED;
	gui_update_download(d, TRUE);

	ignore_add_sha1(name, fi->cha1);

	if (has_good_sha1(d)) {
		ignore_add_filesize(name, d->file_info->size);
		queue_remove_downloads_with_file(d->file_info, d);
		download_move(d, move_file_path, DL_OK_EXT);
	} else {
		download_move(d, bad_file_path, DL_BAD_EXT);
		/* Will go to download_moved_with_bad_sha1() upon completion */
	}
}

/*
 * download_verify_error
 *
 * Called when we cannot verify the SHA1 for the file (I/O error, etc...).
 */
void download_verify_error(struct download *d)
{
	struct dl_file_info *fi = d->file_info;
	gchar *name = file_info_readable_filename(fi);

	g_assert(d->status == GTA_DL_VERIFYING);

	if (0 == strcmp(fi->file_name, name))
		g_warning("error while verifying SHA1 for \"%s\"", fi->file_name);
	else
		g_warning("error while verifying SHA1 for \"%s\" (aka \"%s\")",
			fi->file_name, name);

	d->status = GTA_DL_VERIFIED;

	ignore_add_filesize(name, fi->size);
	queue_remove_downloads_with_file(fi, d);
	download_move(d, move_file_path, DL_UNKN_EXT);
	gui_update_download(d, TRUE);
}

/*
 * download_resume_bg_tasks
 *
 * Go through the downloads and check the completed ones that should
 * be either moved to the "done" directory, or which should have their
 * SHA1 computed/verified.
 */
static void download_resume_bg_tasks(void)
{
	GSList *l;
	GSList *to_remove = NULL;			/* List of fileinfos to remove */

	for (l = sl_downloads; l; l = g_slist_next(l)) {
		struct download *d = (struct download *) l->data;
		struct dl_file_info *fi = d->file_info;

		g_assert(d != NULL);

		if (d->status == GTA_DL_REMOVED)	/* Pending free, ignore it! */
			continue;

		if (fi->flags & FI_F_MARK)		/* Already processed */
			continue;

		fi->flags |= FI_F_MARK;

		if (!FILE_INFO_COMPLETE(fi))	/* Not complete */
			continue;

		/*
		 * Found a complete download.
		 *
		 * More than one download may reference this fileinfo if we crashed
		 * and many such downloads were in the queue at that time.
		 */

		g_assert(fi->refcount >= 1);

		/*
		 * It is possible that the faked download was scheduled to run, and
		 * the fact that it was complete was trapped, and the computing of
		 * its SHA1 started.
		 *
		 * In that case, the fileinfo of the file is marked as "suspended".
		 */

		if (fi->flags & FI_F_SUSPEND)	/* Already computing SHA1 */
			continue;

		if (DOWNLOAD_IS_QUEUED(d))
			download_unqueue(d);

		if (!DOWNLOAD_IS_STOPPED(d))
			download_stop(d, GTA_DL_COMPLETED, NULL);

		/*
		 * If we don't have the computed SHA1 yet, queue it for SHA1
		 * computation, and we'll proceed from there.
		 *
		 * If the file is still in the "tmp" directory, schedule its
		 * moving to the done/bad directory.
		 */

		if (fi->cha1 == NULL)
			download_verify_sha1(d);
		else {
			d->status = GTA_DL_VERIFIED;		/* Does not mean good SHA1 */
			if (has_good_sha1(d))
				download_move(d, move_file_path, DL_OK_EXT);
			else
				download_move(d, bad_file_path, DL_BAD_EXT);
			to_remove = g_slist_prepend(to_remove, d->file_info);
		}

		gui_update_download(d, TRUE);
	}

	/*
	 * Remove queued downloads referencing a complete file.
	 */

	for (l = to_remove; l; l = l->next) {
		struct dl_file_info *fi = (struct dl_file_info *) l->data;
		g_assert(FILE_INFO_COMPLETE(fi));
		queue_remove_downloads_with_file(fi, NULL);
	}

	g_slist_free(to_remove);

	/*
	 * Clear the marks.
	 */

	for (l = sl_downloads; l; l = g_slist_next(l)) {
		struct download *d = (struct download *) l->data;
		struct dl_file_info *fi = d->file_info;

		if (d->status == GTA_DL_REMOVED)	/* Pending free, ignore it! */
			continue;

		fi->flags &= ~FI_F_MARK;
	}
}

void download_close(void)
{
	GSList *l;

	download_store();			/* Save latest copy */
	file_info_store();
	download_freeze_queue();

	download_free_removed();

	for (l = sl_downloads; l; l = g_slist_next(l)) {
		struct download *d = (struct download *) l->data;
		g_assert(d != NULL);
		if (d->socket)
			socket_free(d->socket);
		if (d->push)
			download_push_remove(d);
		if (d->io_opaque)
			io_free(d->io_opaque);
		if (d->bio)
			bsched_source_remove(d->bio);
		if (d->sha1)
			atom_sha1_free(d->sha1);
		if (d->ranges)
			http_range_free(d->ranges);
		if (d->req)
			http_buffer_free(d->req);
		if (d->cproxy)
			cproxy_free(d->cproxy);
		if (d->escaped_name != d->file_name)
			g_free(d->escaped_name);

		file_info_remove_source(d->file_info, d, TRUE);
		parq_dl_remove(d);
		download_remove_from_server(d, TRUE);
		atom_str_free(d->file_name);

		wfree(d, sizeof(*d));
	}

	/* 
	 * FIXME:
	 * It would be much cleaner if all downloads would be properly freed
	 * by calling download_free because thier handles would then be
	 * freed and we can assert that the src_handle_map is empty when
	 * src_close is called. (see src_close)
	 * -- Richard, 24 Mar 2003
	 */

	src_close();

	g_slist_free(sl_downloads);
	sl_downloads = NULL;
	g_slist_free(sl_unqueued);
	sl_unqueued = NULL;
	g_hash_table_destroy(pushed_downloads);
	pushed_downloads = NULL;

	/* XXX free & check other hash tables as well.
	 * dl_by_ip, dl_by_host
	 */
}

/* 
 * build_url_from_download:
 *
 * creates a url which points to a downloads (e.g. you can move this to a
 * browser and download the file there with this url
 */
const gchar *build_url_from_download(struct download *d) 
{
	static gchar url_tmp[1024];
	gchar *buf = NULL;

	if (d == NULL)
		return NULL;
   
	buf = url_escape(d->file_name);

	gm_snprintf(url_tmp, sizeof(url_tmp),
			   "http://%s/get/%u/%s",
			   ip_port_to_gchar(download_ip(d), download_port(d)),
			   d->record_index, buf);

	/*
	 * Since url_escape() creates a new string ONLY if
	 * escaping is necessary, we have to check this and
	 * free memory accordingly.
	 * -- Richard, 30 Apr 2002
	 */

	if (buf != d->file_name) {
		G_FREE_NULL(buf);
	}
	
	return url_tmp;
}

/* vi: set ts=4: */
