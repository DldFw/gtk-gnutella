/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi & Richard Eckart
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

#include "gtk/gui.h"

RCSID("$Id$");

#include "gtk/misc.h"
#include "gtk/statusbar.h"
#include "gtk/search_stats.h"

#include "if/bridge/ui2c.h"
#include "if/gui_property.h"
#include "if/gui_property_priv.h"

#include "lib/override.h"		/* Must be the last header included */

/* 
 * Create a function for the focus out signal and make it call
 * the callback for the activate signal.
 */
#define FOCUS_TO_ACTIVATE(a)                                    \
    gboolean CAT3(on_,a,_focus_out_event) (GtkWidget *widget,	\
			GdkEventFocus *unused_event, gpointer unused_udata)	\
    {                                                           \
        (void) unused_event;                                    \
        (void) unused_udata;                                    \
        CAT3(on_,a,_activate)(GTK_EDITABLE(widget), NULL);      \
        return FALSE;                                           \
    }


static GtkWidget *add_dir_filesel = NULL;



/***
 *** Left panel (selection tree)
 ***/

void
on_ctree_menu_tree_select_row(GtkCTree *ctree, GList *node,
	gint unused_column, gpointer unused_udata)
{
    gint tab;

	(void) unused_column;
	(void) unused_udata;

    tab = GPOINTER_TO_INT(
		gtk_ctree_node_get_row_data(ctree, GTK_CTREE_NODE(node)));

	gtk_notebook_set_page
        (GTK_NOTEBOOK(lookup_widget(main_window, "notebook_main")), tab);
}


gboolean
on_progressbar_bws_in_button_press_event(GtkWidget *unused_widget,
		GdkEventButton *unused_event, gpointer unused_udata)
{
    gboolean val;
    
	(void) unused_widget;
	(void) unused_event;
	(void) unused_udata;
    gui_prop_get_boolean_val(PROP_PROGRESSBAR_BWS_IN_AVG, &val);
    gui_prop_set_boolean_val(PROP_PROGRESSBAR_BWS_IN_AVG, !val);
	return TRUE;
}

gboolean
on_progressbar_bws_out_button_press_event(GtkWidget *unused_widget,
	GdkEventButton *unused_event, gpointer unused_udata)
{
    gboolean val;
    
	(void) unused_widget;
	(void) unused_event;
	(void) unused_udata;
    gui_prop_get_boolean_val(PROP_PROGRESSBAR_BWS_OUT_AVG, &val);
    gui_prop_set_boolean_val(PROP_PROGRESSBAR_BWS_OUT_AVG, !val);
	return TRUE;
}

gboolean
on_progressbar_bws_gin_button_press_event(GtkWidget *unused_widget, 
	GdkEventButton *unused_event, gpointer unused_udata)
{
    gboolean val;
    
	(void) unused_widget;
	(void) unused_event;
	(void) unused_udata;
    gui_prop_get_boolean_val(PROP_PROGRESSBAR_BWS_GIN_AVG, &val);
    gui_prop_set_boolean_val(PROP_PROGRESSBAR_BWS_GIN_AVG, !val);
	return TRUE;
}

gboolean
on_progressbar_bws_gout_button_press_event(GtkWidget *unused_widget, 
	GdkEventButton *unused_event, gpointer unused_udata)
{
    gboolean val;
    
	(void) unused_widget;
	(void) unused_event;
	(void) unused_udata;
    gui_prop_get_boolean_val(PROP_PROGRESSBAR_BWS_GOUT_AVG, &val);
    gui_prop_set_boolean_val(PROP_PROGRESSBAR_BWS_GOUT_AVG, !val);
	return TRUE;
}

gboolean
on_progressbar_bws_lin_button_press_event(GtkWidget *unused_widget, 
	GdkEventButton *unused_event, gpointer unused_udata)
{
    gboolean val;
    
	(void) unused_widget;
	(void) unused_event;
	(void) unused_udata;
    gui_prop_get_boolean_val(PROP_PROGRESSBAR_BWS_GLIN_AVG, &val);
    gui_prop_set_boolean_val(PROP_PROGRESSBAR_BWS_GLIN_AVG, !val);
	return TRUE;
}

gboolean
on_progressbar_bws_lout_button_press_event(GtkWidget *unused_widget,
		GdkEventButton *unused_event, gpointer unused_udata)
{
    gboolean val;
    
	(void) unused_widget;
	(void) unused_event;
	(void) unused_udata;
    gui_prop_get_boolean_val(PROP_PROGRESSBAR_BWS_GLOUT_AVG, &val);
    gui_prop_set_boolean_val(PROP_PROGRESSBAR_BWS_GLOUT_AVG, !val);
	return TRUE;
}

/***
 *** GnutellaNet pane
 ***/

/* minimum connections up */

void
on_button_host_catcher_clear_clicked(GtkButton *unused_button,
	gpointer unused_udata)
{
	(void) unused_button;
	(void) unused_udata;
	guc_hcache_clear_host_type(HOST_ANY);
}

void
on_button_ultra_catcher_clear_clicked(GtkButton *unused_button,
	gpointer unused_udata)
{
	(void) unused_button;
	(void) unused_udata;
	guc_hcache_clear_host_type(HOST_ULTRA);
}

void
on_button_hostcache_clear_bad_clicked(GtkButton *unused_button,
	gpointer unused_udata)
{
	(void) unused_button;
	(void) unused_udata;
    guc_hcache_clear(HCACHE_TIMEOUT);
    guc_hcache_clear(HCACHE_BUSY);
    guc_hcache_clear(HCACHE_UNSTABLE);
}

/***
 *** Search Stats
 ***/ 

void
on_button_search_stats_reset_clicked(GtkButton *unused_button,
	gpointer unused_data)
{
	(void) unused_button;
	(void) unused_data;
	search_stats_gui_reset();
}

void
on_clist_search_stats_resize_column(GtkCList *unused_clist, gint column,
	gint width, gpointer unused_udata)
{
	(void) unused_clist;
	(void) unused_udata;
	*(gint *) &search_stats_col_widths[column] = width;
}

/***
 *** Config pane
 ***/ 


/* While downloading, store files to */

GtkWidget *save_path_filesel = NULL;

gboolean
fs_save_path_delete_event(GtkWidget *unused_widget,
	GdkEvent *unused_event, gpointer unused_udata)
{
	(void) unused_widget;
	(void) unused_event;
	(void) unused_udata;
	gtk_widget_destroy(save_path_filesel);
	save_path_filesel = NULL;
	return TRUE;
}

void
button_fs_save_path_clicked(GtkButton *unused_button, gpointer user_data)
{
	(void) unused_button;

	if (user_data) {
		gchar *name;

        name = g_strdup(gtk_file_selection_get_filename
            (GTK_FILE_SELECTION(save_path_filesel)));

		if (is_directory(name)) {
            gnet_prop_set_string(PROP_SAVE_FILE_PATH, name);
        } else {
            G_FREE_NULL(name);
        }
	}

	gtk_widget_destroy(save_path_filesel);
	save_path_filesel = NULL;
}

void
on_button_config_save_path_clicked(GtkButton *unused_button,
	gpointer unused_udata)
{
	(void) unused_button;
	(void) unused_udata;

	if (!save_path_filesel) {
		save_path_filesel =
			gtk_file_selection_new(
				_("Please choose where to store files while downloading"));

		gtk_signal_connect(GTK_OBJECT
						   (GTK_FILE_SELECTION(save_path_filesel)->
							ok_button), "clicked",
						   GTK_SIGNAL_FUNC(button_fs_save_path_clicked),
						   (gpointer) 1);
		gtk_signal_connect(GTK_OBJECT
						   (GTK_FILE_SELECTION(save_path_filesel)->
							cancel_button), "clicked",
						   GTK_SIGNAL_FUNC(button_fs_save_path_clicked),
						   NULL);
		gtk_signal_connect(GTK_OBJECT(save_path_filesel), "delete_event",
						   GTK_SIGNAL_FUNC(fs_save_path_delete_event),
						   NULL);

		gtk_widget_show(save_path_filesel);
	}
}

/* Move downloaded files to */

GtkWidget *move_path_filesel = (GtkWidget *) NULL;

gboolean
fs_save_move_delete_event(GtkWidget *unused_widget, GdkEvent *unused_event,
	gpointer unused_udata)
{
	(void) unused_widget;
	(void) unused_event;
	(void) unused_udata;
	gtk_widget_destroy(move_path_filesel);
	move_path_filesel = (GtkWidget *) NULL;
	return TRUE;
}

void
button_fs_move_path_clicked(GtkButton *unused_button, gpointer user_data)
{
	(void) unused_button;

	if (user_data) {
		gchar *name;

        name = g_strdup(gtk_file_selection_get_filename
            (GTK_FILE_SELECTION(move_path_filesel)));

		if (is_directory(name)) {
            gnet_prop_set_string(PROP_MOVE_FILE_PATH, name);
        } else {
            G_FREE_NULL(name);
        }
	}

	gtk_widget_destroy(move_path_filesel);
	move_path_filesel = (GtkWidget *) NULL;
}

void
on_button_config_move_path_clicked(GtkButton *unused_button,
	gpointer unused_udata)
{
	(void) unused_button;
	(void) unused_udata;

	if (!move_path_filesel) {
		move_path_filesel = gtk_file_selection_new(
			_("Please choose where to move files after successful download"));

		gtk_signal_connect(GTK_OBJECT
						   (GTK_FILE_SELECTION(move_path_filesel)->
							ok_button), "clicked",
						   GTK_SIGNAL_FUNC(button_fs_move_path_clicked),
						   GINT_TO_POINTER(1));
		gtk_signal_connect(GTK_OBJECT
						   (GTK_FILE_SELECTION(move_path_filesel)->
							cancel_button), "clicked",
						   GTK_SIGNAL_FUNC(button_fs_move_path_clicked),
						   NULL);
		gtk_signal_connect(GTK_OBJECT(move_path_filesel), "delete_event",
						   GTK_SIGNAL_FUNC(fs_save_move_delete_event),
						   NULL);

		gtk_widget_show(move_path_filesel);
	}
}

/* Move bad files to */

GtkWidget *bad_path_filesel = (GtkWidget *) NULL;

gboolean
fs_save_bad_delete_event(GtkWidget *unused_widget, GdkEvent *unused_event,
	gpointer unused_udata)
{
	(void) unused_widget;
	(void) unused_event;
	(void) unused_udata;
	gtk_widget_destroy(bad_path_filesel);
	bad_path_filesel = (GtkWidget *) NULL;
	return TRUE;
}

void
button_fs_bad_path_clicked(GtkButton *unused_button, gpointer user_data)
{
	(void) unused_button;
	
	if (user_data) {
		gchar *name;

        name = g_strdup(gtk_file_selection_get_filename
            (GTK_FILE_SELECTION(bad_path_filesel)));

		if (is_directory(name)) {
            gnet_prop_set_string(PROP_BAD_FILE_PATH, name);
        } else {
            G_FREE_NULL(name);
        }
	}

	gtk_widget_destroy(bad_path_filesel);
	bad_path_filesel = (GtkWidget *) NULL;
}

void
on_button_config_bad_path_clicked(GtkButton *unused_button,
	gpointer unused_udata)
{
	(void) unused_button;
	(void) unused_udata;
	
	if (!bad_path_filesel) {
		bad_path_filesel =
			gtk_file_selection_new(
				_("Please choose where to move corrupted files"));

		gtk_signal_connect(GTK_OBJECT
						   (GTK_FILE_SELECTION(bad_path_filesel)->
							ok_button), "clicked",
						   GTK_SIGNAL_FUNC(button_fs_bad_path_clicked),
						   GINT_TO_POINTER(1));
		gtk_signal_connect(GTK_OBJECT
						   (GTK_FILE_SELECTION(bad_path_filesel)->
							cancel_button), "clicked",
						   GTK_SIGNAL_FUNC(button_fs_bad_path_clicked),
						   NULL);
		gtk_signal_connect(GTK_OBJECT(bad_path_filesel), "delete_event",
						   GTK_SIGNAL_FUNC(fs_save_bad_delete_event),
						   NULL);

		gtk_widget_show(bad_path_filesel);
	}
}

/* Local File DB Managment */

gboolean
fs_add_dir_delete_event(GtkWidget *unused_widget, GdkEvent *unused_event,
	gpointer unused_udata)
{
	(void) unused_widget;
	(void) unused_event;
	(void) unused_udata;
	gtk_widget_destroy(add_dir_filesel);
	add_dir_filesel = NULL;
	return TRUE;
}

void
button_fs_add_dir_clicked(GtkButton *unused_button, gpointer user_data)
{
	(void) unused_button;
	
	if (user_data) {
		gchar *name;
     
        name = g_strdup(gtk_file_selection_get_filename
            (GTK_FILE_SELECTION(add_dir_filesel)));

		if (is_directory(name))
			guc_shared_dir_add(name);

        G_FREE_NULL(name);
	}

	gtk_widget_destroy(add_dir_filesel);
	add_dir_filesel = NULL;
}

void
on_button_config_add_dir_clicked(GtkButton *unused_button,
	gpointer unused_udata)
{
	(void) unused_button;
	(void) unused_udata;
	
	if (!add_dir_filesel) {
		add_dir_filesel =
			gtk_file_selection_new(_("Please choose a directory to share"));

		gtk_signal_connect(GTK_OBJECT
						   (GTK_FILE_SELECTION(add_dir_filesel)->
							ok_button), "clicked",
						   GTK_SIGNAL_FUNC(button_fs_add_dir_clicked),
						   GINT_TO_POINTER(1));
		gtk_signal_connect(GTK_OBJECT
						   (GTK_FILE_SELECTION(add_dir_filesel)->
							cancel_button), "clicked",
						   GTK_SIGNAL_FUNC(button_fs_add_dir_clicked),
						   NULL);
		gtk_signal_connect(GTK_OBJECT(add_dir_filesel), "delete_event",
						   GTK_SIGNAL_FUNC(fs_add_dir_delete_event), NULL);

		gtk_widget_show(add_dir_filesel);
	}
}

void
on_button_config_rescan_dir_clicked(GtkButton *unused_button,
	gpointer unused_udata)
{
	(void) unused_button;
	(void) unused_udata;
	
	gui_allow_rescan_dir(FALSE);
	guc_share_scan();
	gui_allow_rescan_dir(TRUE);
}


void
on_entry_config_netmask_activate(GtkEditable *editable, gpointer unused_data)
{
    gchar *buf;
    
	(void) unused_data;
    buf = STRTRACK(gtk_editable_get_chars(editable, 0, -1));
    gnet_prop_set_string(PROP_LOCAL_NETMASKS_STRING, buf);
    G_FREE_NULL(buf);
}
FOCUS_TO_ACTIVATE(entry_config_netmask)

/***
 *** search list (sidebar)
 ***/
void
on_clist_search_resize_column(GtkCList *unused_clist, gint column,
	gint width, gpointer unused_udata)
{
	(void) unused_clist;
	(void) unused_udata;
    *(gint *) &search_list_col_widths[column] = width;
}

#ifdef USE_GTK2

void
on_hb_searches_child_detached(GtkHandleBox *unused_handlebox,
	GtkWidget *widget, gpointer unused_udata)
{
	gint width, height;

	(void) unused_handlebox;
	(void) unused_udata;
	
	/*
	 * Before the GtkHandleBox is detached, request the current size
	 * of the widget inside to preserve dimensions. Otherwise, the
	 * detached GtkHandleBox becomes pretty small.
	 */

	width = widget->allocation.width;
	height = widget->allocation.height;
	gtk_widget_set_size_request(widget, width, height);
}

void
on_menu_wizard_connection_unknown_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	/* TODO: Implement */
	(void) unused_menuitem;
	(void) unused_udata;
}


void
on_menu_wizard_connection_t3_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	/* TODO: Implement */
	(void) unused_menuitem;
	(void) unused_udata;
}


void
on_menu_wizard_connection_t1_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	/* TODO: Implement */
	(void) unused_menuitem;
	(void) unused_udata;
}


void
on_menu_wizard_connection_sdsl_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	/* TODO: Implement */
	(void) unused_menuitem;
	(void) unused_udata;
}


void
on_menu_wizard_connection_adsl_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{

	/* TODO: Implement */
	(void) unused_menuitem;
	(void) unused_udata;
}


void
on_menu_wizard_connection_isdn128_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	/* TODO: Implement */
	(void) unused_menuitem;
	(void) unused_udata;
}


void
on_menu_wizard_connection_isdn64_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	/* TODO: Implement */
	(void) unused_menuitem;
	(void) unused_udata;
}


void
on_menu_wizard_connection_modem_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	/* TODO: Implement */
	(void) unused_menuitem;
	(void) unused_udata;
}


#endif /* USE_GTK2 */
/* vi: set ts=4 sw=4 cindent: */
