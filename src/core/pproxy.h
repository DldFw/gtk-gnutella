/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Raphael Manfredi
 *
 * Push proxy HTTP management.
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

#ifndef _core_pproxy_h_
#define _core_pproxy_h_

#include <glib.h>

#include "if/core/pproxy.h"

/***
 *** Server side
 ***/

/*
 * A push proxy request we received.
 */
struct pproxy {
	struct gnutella_socket *socket;
	gint error_sent;		/* HTTP error code sent back */
	time_t last_update;

	guint32 ip;				/* IP of the requesting servent */
	guint16 port;			/* Port where GIV should be sent back */
	gchar *user_agent;		/* User-Agent string */
	gchar *guid;			/* GUID (atom) to which push should be sent */
	guint32 file_idx;		/* File index to request (0 if none supplied) */
	gpointer io_opaque;		/* Opaque I/O callback information */
};

#define pproxy_vendor_str(p)	((p)->user_agent ? (p)->user_agent : "")

void pproxy_add(struct gnutella_socket *s);
void pproxy_remove(struct pproxy *pp,
	const gchar *reason, ...) G_GNUC_PRINTF(2, 3);
void pproxy_timer(time_t now);
void pproxy_close(void);

/***
 *** Client side
 ***/

struct cproxy *cproxy_create(struct download *d,
	guint32 ip, guint16 port, gchar *guid, guint32 file_idx);
void cproxy_free(struct cproxy *cp);
void cproxy_reparent(struct download *d, struct download *cd);
	
#endif	/* _core_pproxy_h_ */

/* vi: set ts=4: */
