/*
 * Geeqie
 * (C) 2004 John Ellis
 *
 * Author: John Ellis
 *
 * This software is released under the GNU General Public License (GNU GPL).
 * Please read the included file COPYING for more information.
 * This software comes with no warranty of any kind, use at your own risk!
 */

#include "main.h"
#include "view_file_list.h"

#include "cache_maint.h"
#include "dnd.h"
#include "editors.h"
#include "img-view.h"
#include "info.h"
#include "layout.h"
#include "layout_image.h"
#include "menu.h"
#include "thumb.h"
#include "utilops.h"
#include "ui_bookmark.h"
#include "ui_fileops.h"
#include "ui_menu.h"
#include "ui_tree_edit.h"
#include "typedefs.h"

#include <gdk/gdkkeysyms.h> /* for keyboard values */


enum {
	FILE_COLUMN_POINTER = 0,
	FILE_COLUMN_THUMB,
	FILE_COLUMN_NAME,
	FILE_COLUMN_SIDECARS,
	FILE_COLUMN_SIZE,
	FILE_COLUMN_DATE,
	FILE_COLUMN_COLOR,
	FILE_COLUMN_MARKS,
	FILE_COLUMN_MARKS_LAST = FILE_COLUMN_MARKS + FILEDATA_MARKS_SIZE - 1,
	FILE_COLUMN_COUNT
};


static gint vflist_row_is_selected(ViewFileList *vfl, FileData *fd);
static gint vflist_row_rename_cb(TreeEditData *td, const gchar *old, const gchar *new, gpointer data);
static void vflist_populate_view(ViewFileList *vfl);

/*
 *-----------------------------------------------------------------------------
 * signals
 *-----------------------------------------------------------------------------
 */

static void vflist_send_update(ViewFileList *vfl)
{
	if (vfl->func_status) vfl->func_status(vfl, vfl->data_status);
}

/*
 *-----------------------------------------------------------------------------
 * misc
 *-----------------------------------------------------------------------------
 */
typedef struct {
	FileData *fd;
	GtkTreeIter *iter;
	gint found;
	gint row;
} ViewFileListFindRowData;

static gboolean vflist_find_row_cb(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	ViewFileListFindRowData *find = data;
	FileData *fd;
	gtk_tree_model_get(model, iter, FILE_COLUMN_POINTER, &fd, -1);
	if (fd == find->fd)
		{
		*find->iter = *iter;
		find->found = 1;
		return TRUE;
		}
	find->row++;
	return FALSE;
}

static gint vflist_find_row(ViewFileList *vfl, FileData *fd, GtkTreeIter *iter)
{
	GtkTreeModel *store;
	ViewFileListFindRowData data = {fd, iter, 0, 0};

	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vfl->listview));
	gtk_tree_model_foreach(store, vflist_find_row_cb, &data);

	if (data.found)
		{
		return data.row;
		}

	return -1;
}


/*
static gint vflist_find_sidecar_list_idx(GList *work, FileData *fd)
{
	gint i = 0;
	while (work)
		{
		FileData *fd_p = work->data;
		if (fd == fd_p) return i;

		i++;

		GList *work2 = fd_p->sidecar_files;
		while (work2)
			{
			fd_p = work2->data;
			if (fd == fd_p) return i;

			i++;
			work2 = work2->next;
			}
		work = work->next;
		}
	return -1;
}
*/

static gint vflist_sidecar_list_count(GList *work)
{
	gint i = 0;
	while (work)
		{
		FileData *fd = work->data;
		i++;

		GList *work2 = fd->sidecar_files;
		while (work2)
			{
			i++;
			work2 = work2->next;
			}
		work = work->next;
		}
	return i;
}


static void vflist_color_set(ViewFileList *vfl, FileData *fd, gint color_set)
{
	GtkTreeModel *store;
	GtkTreeIter iter;

	if (vflist_find_row(vfl, fd, &iter) < 0) return;
	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vfl->listview));
	gtk_tree_store_set(GTK_TREE_STORE(store), &iter, FILE_COLUMN_COLOR, color_set, -1);
}

static void vflist_move_cursor(ViewFileList *vfl, GtkTreeIter *iter)
{
	GtkTreeModel *store;
	GtkTreePath *tpath;

	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vfl->listview));

	tpath = gtk_tree_model_get_path(store, iter);
	gtk_tree_view_set_cursor(GTK_TREE_VIEW(vfl->listview), tpath, NULL, FALSE);
	gtk_tree_path_free(tpath);
}


static gint vflist_column_idx(ViewFileList *vfl, gint store_idx)
{
	GList *columns, *work;
	gint i = 0;

	columns = gtk_tree_view_get_columns(GTK_TREE_VIEW(vfl->listview));
	work = columns;
	while (work)
		{
		GtkTreeViewColumn *column = work->data;
		if (store_idx == GPOINTER_TO_INT(g_object_get_data (G_OBJECT(column), "column_store_idx")))
			break;
		work = work->next;
		i++;
		}

	g_list_free(columns);
	return i;
}


/*
 *-----------------------------------------------------------------------------
 * dnd
 *-----------------------------------------------------------------------------
 */

static void vflist_dnd_get(GtkWidget *widget, GdkDragContext *context,
			   GtkSelectionData *selection_data, guint info,
			   guint time, gpointer data)
{
	ViewFileList *vfl = data;
	GList *list = NULL;
	gchar *uri_text = NULL;
	gint total;

	if (!vfl->click_fd) return;

	if (vflist_row_is_selected(vfl, vfl->click_fd))
		{
		list = vflist_selection_get_list(vfl);
		}
	else
		{
		list = g_list_append(NULL, file_data_ref(vfl->click_fd));
		}

	if (!list) return;

	uri_text = uri_text_from_filelist(list, &total, (info == TARGET_TEXT_PLAIN));
	filelist_free(list);

	if (debug) printf(uri_text);

	gtk_selection_data_set(selection_data, selection_data->target,
			       8, (guchar *)uri_text, total);
	g_free(uri_text);
}

static void vflist_dnd_begin(GtkWidget *widget, GdkDragContext *context, gpointer data)
{
	ViewFileList *vfl = data;

	vflist_color_set(vfl, vfl->click_fd, TRUE);

	if (vfl->thumbs_enabled &&
	    vfl->click_fd && vfl->click_fd->pixbuf)
		{
		gint items;

		if (vflist_row_is_selected(vfl, vfl->click_fd))
			items = vflist_selection_count(vfl, NULL);
		else
			items = 1;

		dnd_set_drag_icon(widget, context, vfl->click_fd->pixbuf, items);
		}
}

static void vflist_dnd_end(GtkWidget *widget, GdkDragContext *context, gpointer data)
{
	ViewFileList *vfl = data;

	vflist_color_set(vfl, vfl->click_fd, FALSE);

	if (context->action == GDK_ACTION_MOVE)
		{
		vflist_refresh(vfl);
		}
}

static void vflist_dnd_init(ViewFileList *vfl)
{
	gtk_drag_source_set(vfl->listview, GDK_BUTTON1_MASK | GDK_BUTTON2_MASK,
			    dnd_file_drag_types, dnd_file_drag_types_count,
			    GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK);
	g_signal_connect(G_OBJECT(vfl->listview), "drag_data_get",
			 G_CALLBACK(vflist_dnd_get), vfl);
	g_signal_connect(G_OBJECT(vfl->listview), "drag_begin",
			 G_CALLBACK(vflist_dnd_begin), vfl);
	g_signal_connect(G_OBJECT(vfl->listview), "drag_end",
			 G_CALLBACK(vflist_dnd_end), vfl);
}

/*
 *-----------------------------------------------------------------------------
 * pop-up menu
 *-----------------------------------------------------------------------------
 */

static GList *vflist_pop_menu_file_list(ViewFileList *vfl)
{
	if (!vfl->click_fd) return NULL;

	if (vflist_row_is_selected(vfl, vfl->click_fd))
		{
		return vflist_selection_get_list(vfl);
		}

	return g_list_append(NULL, file_data_ref(vfl->click_fd));
}

static void vflist_pop_menu_edit_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl;
	gint n;
	GList *list;

	vfl = submenu_item_get_data(widget);
	n = GPOINTER_TO_INT(data);

	if (!vfl) return;

	list = vflist_pop_menu_file_list(vfl);
	start_editor_from_filelist(n, list);
	filelist_free(list);
}

static void vflist_pop_menu_info_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;

	info_window_new(NULL, vflist_pop_menu_file_list(vfl));
}

static void vflist_pop_menu_view_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;

	if (vflist_row_is_selected(vfl, vfl->click_fd))
		{
		GList *list;

		list = vflist_selection_get_list(vfl);
		view_window_new_from_list(list);
		filelist_free(list);
		}
	else
		{
		view_window_new(vfl->click_fd);
		}
}

static void vflist_pop_menu_copy_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;

	file_util_copy(NULL, vflist_pop_menu_file_list(vfl), NULL, vfl->listview);
}

static void vflist_pop_menu_move_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;

	file_util_move(NULL, vflist_pop_menu_file_list(vfl), NULL, vfl->listview);
}

static void vflist_pop_menu_rename_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;
	GList *list;

	list = vflist_pop_menu_file_list(vfl);
	if (options->file_ops.enable_in_place_rename &&
	    list && !list->next && vfl->click_fd)
		{
		GtkTreeModel *store;
		GtkTreeIter iter;

		filelist_free(list);

		store = gtk_tree_view_get_model(GTK_TREE_VIEW(vfl->listview));
		if (vflist_find_row(vfl, vfl->click_fd, &iter) >= 0)
			{
			GtkTreePath *tpath;

			tpath = gtk_tree_model_get_path(store, &iter);
			tree_edit_by_path(GTK_TREE_VIEW(vfl->listview), tpath,
					  vflist_column_idx(vfl, FILE_COLUMN_NAME), vfl->click_fd->name,
					  vflist_row_rename_cb, vfl);
			gtk_tree_path_free(tpath);
			}
		return;
		}

	file_util_rename(NULL, list, vfl->listview);
}

static void vflist_pop_menu_delete_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;

	file_util_delete(NULL, vflist_pop_menu_file_list(vfl), vfl->listview);
}

static void vflist_pop_menu_sort_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl;
	SortType type;

	if (!gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(widget))) return;

	vfl = submenu_item_get_data(widget);
	if (!vfl) return;

	type = (SortType)GPOINTER_TO_INT(data);

	if (vfl->layout)
		{
		layout_sort_set(vfl->layout, type, vfl->sort_ascend);
		}
	else
		{
		vflist_sort_set(vfl, type, vfl->sort_ascend);
		}
}

static void vflist_pop_menu_sort_ascend_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;

	if (vfl->layout)
		{
		layout_sort_set(vfl->layout, vfl->sort_method, !vfl->sort_ascend);
		}
	else
		{
		vflist_sort_set(vfl, vfl->sort_method, !vfl->sort_ascend);
		}
}

static void vflist_pop_menu_icons_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;

	if (vfl->layout) layout_views_set(vfl->layout, vfl->layout->dir_view_type, TRUE);
}

static void vflist_pop_menu_thumbs_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;

	vflist_color_set(vfl, vfl->click_fd, FALSE);
	if (vfl->layout)
		{
		layout_thumb_set(vfl->layout, !vfl->thumbs_enabled);
		}
	else
		{
		vflist_thumb_set(vfl, !vfl->thumbs_enabled);
		}
}

static void vflist_pop_menu_refresh_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;

	vflist_color_set(vfl, vfl->click_fd, FALSE);
	vflist_refresh(vfl);
}

static void vflist_pop_menu_sel_mark_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;
	vflist_mark_to_selection(vfl, vfl->active_mark, MTS_MODE_SET);
}

static void vflist_pop_menu_sel_mark_and_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;
	vflist_mark_to_selection(vfl, vfl->active_mark, MTS_MODE_AND);
}

static void vflist_pop_menu_sel_mark_or_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;
	vflist_mark_to_selection(vfl, vfl->active_mark, MTS_MODE_OR);
}

static void vflist_pop_menu_sel_mark_minus_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;
	vflist_mark_to_selection(vfl, vfl->active_mark, MTS_MODE_MINUS);
}

static void vflist_pop_menu_set_mark_sel_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;
	vflist_selection_to_mark(vfl, vfl->active_mark, STM_MODE_SET);
}

static void vflist_pop_menu_res_mark_sel_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;
	vflist_selection_to_mark(vfl, vfl->active_mark, STM_MODE_RESET);
}

static void vflist_pop_menu_toggle_mark_sel_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;
	vflist_selection_to_mark(vfl, vfl->active_mark, STM_MODE_TOGGLE);
}


static void vflist_popup_destroy_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;
	vflist_color_set(vfl, vfl->click_fd, FALSE);
	vfl->click_fd = NULL;
	vfl->popup = NULL;
}


static GtkWidget *vflist_pop_menu(ViewFileList *vfl, FileData *fd, gint col_idx)
{
	GtkWidget *menu;
	GtkWidget *item;
	GtkWidget *submenu;
	gint active;

	vflist_color_set(vfl, fd, TRUE);
	active = (fd != NULL);

	menu = popup_menu_short_lived();
	g_signal_connect(G_OBJECT(menu), "destroy",
			 G_CALLBACK(vflist_popup_destroy_cb), vfl);

	if (col_idx >= FILE_COLUMN_MARKS && col_idx <= FILE_COLUMN_MARKS_LAST)
		{
		gint mark = col_idx - FILE_COLUMN_MARKS;
		gchar *str_set_mark = g_strdup_printf(_("_Set mark %d"), mark + 1);
		gchar *str_res_mark = g_strdup_printf(_("_Reset mark %d"), mark + 1);
		gchar *str_toggle_mark = g_strdup_printf(_("_Toggle mark %d"), mark + 1);
		gchar *str_sel_mark = g_strdup_printf(_("_Select mark %d"), mark + 1);
		gchar *str_sel_mark_or = g_strdup_printf(_("_Add mark %d"), mark + 1);
		gchar *str_sel_mark_and = g_strdup_printf(_("_Intersection with mark %d"), mark + 1);
		gchar *str_sel_mark_minus = g_strdup_printf(_("_Unselect mark %d"), mark + 1);


		vfl->active_mark = mark;
		menu_item_add_sensitive(menu, str_set_mark, active,
					G_CALLBACK(vflist_pop_menu_set_mark_sel_cb), vfl);

		menu_item_add_sensitive(menu, str_res_mark, active,
					G_CALLBACK(vflist_pop_menu_res_mark_sel_cb), vfl);

		menu_item_add_sensitive(menu, str_toggle_mark, active,
					G_CALLBACK(vflist_pop_menu_toggle_mark_sel_cb), vfl);

		menu_item_add_divider(menu);

		menu_item_add_sensitive(menu, str_sel_mark, active,
					G_CALLBACK(vflist_pop_menu_sel_mark_cb), vfl);
		menu_item_add_sensitive(menu, str_sel_mark_or, active,
					G_CALLBACK(vflist_pop_menu_sel_mark_or_cb), vfl);
		menu_item_add_sensitive(menu, str_sel_mark_and, active,
					G_CALLBACK(vflist_pop_menu_sel_mark_and_cb), vfl);
		menu_item_add_sensitive(menu, str_sel_mark_minus, active,
					G_CALLBACK(vflist_pop_menu_sel_mark_minus_cb), vfl);

		menu_item_add_divider(menu);

		g_free(str_set_mark);
		g_free(str_res_mark);
		g_free(str_toggle_mark);
		g_free(str_sel_mark);
		g_free(str_sel_mark_and);
		g_free(str_sel_mark_or);
		g_free(str_sel_mark_minus);
		}

	submenu_add_edit(menu, &item, G_CALLBACK(vflist_pop_menu_edit_cb), vfl);
	gtk_widget_set_sensitive(item, active);

	menu_item_add_stock_sensitive(menu, _("_Properties"), GTK_STOCK_PROPERTIES, active,
				      G_CALLBACK(vflist_pop_menu_info_cb), vfl);
	menu_item_add_stock_sensitive(menu, _("View in _new window"), GTK_STOCK_NEW, active,
				      G_CALLBACK(vflist_pop_menu_view_cb), vfl);

	menu_item_add_divider(menu);
	menu_item_add_stock_sensitive(menu, _("_Copy..."), GTK_STOCK_COPY, active,
				      G_CALLBACK(vflist_pop_menu_copy_cb), vfl);
	menu_item_add_sensitive(menu, _("_Move..."), active,
				G_CALLBACK(vflist_pop_menu_move_cb), vfl);
	menu_item_add_sensitive(menu, _("_Rename..."), active,
				G_CALLBACK(vflist_pop_menu_rename_cb), vfl);
	menu_item_add_stock_sensitive(menu, _("_Delete..."), GTK_STOCK_DELETE, active,
				G_CALLBACK(vflist_pop_menu_delete_cb), vfl);

	menu_item_add_divider(menu);

	submenu = submenu_add_sort(NULL, G_CALLBACK(vflist_pop_menu_sort_cb), vfl,
				   FALSE, FALSE, TRUE, vfl->sort_method);
	menu_item_add_divider(submenu);
	menu_item_add_check(submenu, _("Ascending"), vfl->sort_ascend,
			    G_CALLBACK(vflist_pop_menu_sort_ascend_cb), vfl);

	item = menu_item_add(menu, _("_Sort"), NULL, NULL);
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);

	menu_item_add_check(menu, _("View as _icons"), FALSE,
			    G_CALLBACK(vflist_pop_menu_icons_cb), vfl);
	menu_item_add_check(menu, _("Show _thumbnails"), vfl->thumbs_enabled,
			    G_CALLBACK(vflist_pop_menu_thumbs_cb), vfl);
	menu_item_add_stock(menu, _("Re_fresh"), GTK_STOCK_REFRESH, G_CALLBACK(vflist_pop_menu_refresh_cb), vfl);

	return menu;
}

/*
 *-----------------------------------------------------------------------------
 * callbacks
 *-----------------------------------------------------------------------------
 */

static gint vflist_row_rename_cb(TreeEditData *td, const gchar *old, const gchar *new, gpointer data)
{
	ViewFileList *vfl = data;
	gchar *old_path;
	gchar *new_path;

	if (strlen(new) == 0) return FALSE;

	old_path = concat_dir_and_file(vfl->path, old);
	new_path = concat_dir_and_file(vfl->path, new);

	if (strchr(new, '/') != NULL)
		{
		gchar *text = g_strdup_printf(_("Invalid file name:\n%s"), new);
		file_util_warning_dialog(_("Error renaming file"), text, GTK_STOCK_DIALOG_ERROR, vfl->listview);
		g_free(text);
		}
	else if (isfile(new_path))
		{
		gchar *text = g_strdup_printf(_("A file with name %s already exists."), new);
		file_util_warning_dialog(_("Error renaming file"), text, GTK_STOCK_DIALOG_ERROR, vfl->listview);
		g_free(text);
		}
	else
		{
		gint row = vflist_index_by_path(vfl, old_path);
		if (row >= 0)
			{
			GList *work = g_list_nth(vfl->list, row);
			FileData *fd = work->data;

			if (!file_data_add_change_info(fd, FILEDATA_CHANGE_RENAME, old_path, new_path) || !rename_file_ext(fd))
				{
				gchar *text = g_strdup_printf(_("Unable to rename file:\n%s\nto:\n%s"), old, new);
				file_util_warning_dialog(_("Error renaming file"), text, GTK_STOCK_DIALOG_ERROR, vfl->listview);
				g_free(text);
				}
			}

		}
	g_free(old_path);
	g_free(new_path);

	return FALSE;
}

static void vflist_menu_position_cb(GtkMenu *menu, gint *x, gint *y, gboolean *push_in, gpointer data)
{
	ViewFileList *vfl = data;
	GtkTreeModel *store;
	GtkTreeIter iter;
	GtkTreePath *tpath;
	gint cw, ch;

	if (vflist_find_row(vfl, vfl->click_fd, &iter) < 0) return;
	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vfl->listview));
	tpath = gtk_tree_model_get_path(store, &iter);
	tree_view_get_cell_clamped(GTK_TREE_VIEW(vfl->listview), tpath, FILE_COLUMN_NAME - 1, TRUE, x, y, &cw, &ch);
	gtk_tree_path_free(tpath);
	*y += ch;
	popup_menu_position_clamp(menu, x, y, 0);
}

static gint vflist_press_key_cb(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
	ViewFileList *vfl = data;
	GtkTreePath *tpath;

	if (event->keyval != GDK_Menu) return FALSE;

	gtk_tree_view_get_cursor(GTK_TREE_VIEW(vfl->listview), &tpath, NULL);
	if (tpath)
		{
		GtkTreeModel *store;
		GtkTreeIter iter;

		store = gtk_tree_view_get_model(GTK_TREE_VIEW(widget));
		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &vfl->click_fd, -1);
		gtk_tree_path_free(tpath);
		}
	else
		{
		vfl->click_fd = NULL;
		}

	vfl->popup = vflist_pop_menu(vfl, vfl->click_fd, 0);
	gtk_menu_popup(GTK_MENU(vfl->popup), NULL, NULL, vflist_menu_position_cb, vfl, 0, GDK_CURRENT_TIME);

	return TRUE;
}

static gint vflist_press_cb(GtkWidget *widget, GdkEventButton *bevent, gpointer data)
{
	ViewFileList *vfl = data;
	GtkTreePath *tpath;
	GtkTreeIter iter;
	FileData *fd = NULL;
	GtkTreeViewColumn *column;
	gint col_idx = 0;

	if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), bevent->x, bevent->y,
					  &tpath, &column, NULL, NULL))
		{
		GtkTreeModel *store;
		col_idx = GPOINTER_TO_INT(g_object_get_data (G_OBJECT(column), "column_store_idx"));

		if (bevent->button == 1 &&
		    col_idx >= FILE_COLUMN_MARKS && col_idx <= FILE_COLUMN_MARKS_LAST)
			return FALSE;

		store = gtk_tree_view_get_model(GTK_TREE_VIEW(widget));

		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &fd, -1);
#if 0
		gtk_tree_view_set_cursor(GTK_TREE_VIEW(widget), tpath, NULL, FALSE);
#endif
		gtk_tree_path_free(tpath);
		}

	vfl->click_fd = fd;

	if (bevent->button == 3)
		{
		vfl->popup = vflist_pop_menu(vfl, vfl->click_fd, col_idx);
		gtk_menu_popup(GTK_MENU(vfl->popup), NULL, NULL, NULL, NULL,
				bevent->button, bevent->time);
		return TRUE;
		}

	if (!fd) return FALSE;

	if (bevent->button == 2)
		{
		if (!vflist_row_is_selected(vfl, fd))
			{
			vflist_color_set(vfl, fd, TRUE);
			}
		return TRUE;
		}


	if (bevent->button == 1 && bevent->type == GDK_BUTTON_PRESS &&
	    !(bevent->state & GDK_SHIFT_MASK ) &&
	    !(bevent->state & GDK_CONTROL_MASK ) &&
	    vflist_row_is_selected(vfl, fd))
		{
		gtk_widget_grab_focus(widget);
//		return TRUE; // FIXME - expand
		}

#if 0
	if (bevent->button == 1 && bevent->type == GDK_2BUTTON_PRESS)
		{
		if (vfl->layout) layout_image_full_screen_start(vfl->layout);
		}
#endif

	return FALSE;
}

static gint vflist_release_cb(GtkWidget *widget, GdkEventButton *bevent, gpointer data)
{
	ViewFileList *vfl = data;
	GtkTreePath *tpath;
	GtkTreeIter iter;
	FileData *fd = NULL;

	if (bevent->button == 2)
		{
		vflist_color_set(vfl, vfl->click_fd, FALSE);
		}

	if (bevent->button != 1 && bevent->button != 2)
		{
		return TRUE;
		}

	if ((bevent->x != 0 || bevent->y != 0) &&
	    gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), bevent->x, bevent->y,
					  &tpath, NULL, NULL, NULL))
		{
		GtkTreeModel *store;

		store = gtk_tree_view_get_model(GTK_TREE_VIEW(widget));
		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &fd, -1);
		gtk_tree_path_free(tpath);
		}

	if (bevent->button == 2)
		{
		if (fd && vfl->click_fd == fd)
			{
			GtkTreeSelection *selection;

			selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
			if (vflist_row_is_selected(vfl, fd))
				{
				gtk_tree_selection_unselect_iter(selection, &iter);
				}
			else
				{
				gtk_tree_selection_select_iter(selection, &iter);
				}
			}
		return TRUE;
		}

	if (fd && vfl->click_fd == fd &&
	    !(bevent->state & GDK_SHIFT_MASK ) &&
	    !(bevent->state & GDK_CONTROL_MASK ) &&
	    vflist_row_is_selected(vfl, fd))
		{
		GtkTreeSelection *selection;

		selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
		gtk_tree_selection_unselect_all(selection);
		gtk_tree_selection_select_iter(selection, &iter);
		vflist_move_cursor(vfl, &iter);
//		return TRUE;// FIXME - expand
		}

	return FALSE;
}

static void vflist_select_image(ViewFileList *vfl, FileData *sel_fd)
{
	FileData *read_ahead_fd = NULL;
	gint row;
	FileData *cur_fd;
	if (!sel_fd) return;

	cur_fd = layout_image_get_fd(vfl->layout);
	if (sel_fd == cur_fd) return; /* no change */

	row = g_list_index(vfl->list, sel_fd);
	// FIXME sidecar data

	if (sel_fd && options->image.enable_read_ahead && row >= 0)
		{
		if (row > g_list_index(vfl->list, cur_fd) &&
		    row + 1 < vflist_count(vfl, NULL))
			{
			read_ahead_fd = vflist_index_get_data(vfl, row + 1);
			}
		else if (row > 0)
			{
			read_ahead_fd = vflist_index_get_data(vfl, row - 1);
			}
		}

	layout_image_set_with_ahead(vfl->layout, sel_fd, read_ahead_fd);
}

static gint vflist_select_idle_cb(gpointer data)
{
	ViewFileList *vfl = data;

	if (!vfl->layout)
		{
		vfl->select_idle_id = -1;
		return FALSE;
		}

	vflist_send_update(vfl);

	if (vfl->select_fd)
		{
		vflist_select_image(vfl, vfl->select_fd);
		vfl->select_fd = NULL;
		}

	vfl->select_idle_id = -1;
	return FALSE;
}

static void vflist_select_idle_cancel(ViewFileList *vfl)
{
	if (vfl->select_idle_id != -1) g_source_remove(vfl->select_idle_id);
	vfl->select_idle_id = -1;
}

static gboolean vflist_select_cb(GtkTreeSelection *selection, GtkTreeModel *store, GtkTreePath *tpath,
				 gboolean path_currently_selected, gpointer data)
{
	ViewFileList *vfl = data;
	GtkTreeIter iter;

	if (!path_currently_selected &&
	    gtk_tree_model_get_iter(store, &iter, tpath))
		{
		gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &vfl->select_fd, -1);
		}
	else
		{
		vfl->select_fd = NULL;
		}

	if (vfl->layout &&
	    vfl->select_idle_id == -1)
		{
		vfl->select_idle_id = g_idle_add(vflist_select_idle_cb, vfl);
		}

	return TRUE;
}

/*
 *-----------------------------------------------------------------------------
 * misc
 *-----------------------------------------------------------------------------
 */

/*
static gboolean vflist_dummy_select_cb(GtkTreeSelection *selection, GtkTreeModel *store, GtkTreePath *tpath,
					gboolean path_currently_selected, gpointer data)
{
	return TRUE;
}
*/


static void vflist_setup_iter(ViewFileList *vfl, GtkTreeStore *store, GtkTreeIter *iter, FileData *fd)
{
	int i;
	gchar *size;
	gchar *sidecars = NULL;

	if (fd->sidecar_files)
		sidecars = sidecar_file_data_list_to_string(fd);
	size = text_from_size(fd->size);

	gtk_tree_store_set(store, iter, FILE_COLUMN_POINTER, fd,
					FILE_COLUMN_THUMB, (vfl->thumbs_enabled) ? fd->pixbuf : NULL,
					FILE_COLUMN_NAME, fd->name,
					FILE_COLUMN_SIDECARS, sidecars,
					FILE_COLUMN_SIZE, size,
					FILE_COLUMN_DATE, text_from_time(fd->date),
					FILE_COLUMN_COLOR, FALSE, -1);
	for (i = 0; i < FILEDATA_MARKS_SIZE; i++)
		gtk_tree_store_set(store, iter, FILE_COLUMN_MARKS + i, fd->marks[i], -1);

	g_free(size);
	if (sidecars)
		g_free(sidecars);
}

static void vflist_setup_iter_with_sidecars(ViewFileList *vfl, GtkTreeStore *store, GtkTreeIter *iter, FileData *fd)
{
	GList *work;
	GtkTreeIter s_iter;
	gint valid;

	vflist_setup_iter(vfl, store, iter, fd);


	/* this is almost the same code as in vflist_populate_view
	   maybe it should be made more generic and used in both places */


	valid = gtk_tree_model_iter_children(GTK_TREE_MODEL(store), &s_iter, iter);

	work = fd->sidecar_files;
	while (work)
		{
		gint match;
		FileData *sfd = work->data;
		gint done = FALSE;

		while (!done)
			{
			FileData *old_sfd = NULL;

			if (valid)
				{
				gtk_tree_model_get(GTK_TREE_MODEL(store), &s_iter, FILE_COLUMN_POINTER, &old_sfd, -1);

				if (sfd == old_sfd)
					{
					match = 0;
					}
				else
					{
					match = filelist_sort_compare_filedata_full(sfd, old_sfd, SORT_NAME, TRUE);
					}
				}

			else
				{
				match = -1;
				}

			if (match < 0)
				{
				GtkTreeIter new;

				if (valid)
					{
					gtk_tree_store_insert_before(store, &new, iter, &s_iter);
					}
				else
					{
					gtk_tree_store_append(store, &new, iter);
					}

				vflist_setup_iter(vfl, store, &new, sfd);

				done = TRUE;
				}
			else if (match > 0)
				{
				valid = gtk_tree_store_remove(store, &s_iter);
				}
			else
				{
				vflist_setup_iter(vfl, store, &s_iter, sfd);

				if (valid) valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &s_iter);

				done = TRUE;
				}
			}
		work = work->next;
		}

	while (valid)
		{
		valid = gtk_tree_store_remove(store, &s_iter);
		}
}

void vflist_sort_set(ViewFileList *vfl, SortType type, gint ascend)
{
	gint i;
	GHashTable *fd_idx_hash = g_hash_table_new(NULL, NULL);
	gint *new_order;
	GtkTreeStore *store;
	GList *work;

	if (vfl->sort_method == type && vfl->sort_ascend == ascend) return;
	if (!vfl->list) return;

	work = vfl->list;
	i = 0;
	while (work)
		{
		FileData *fd = work->data;
		g_hash_table_insert(fd_idx_hash, fd, GINT_TO_POINTER(i));
		i++;
		work = work->next;
		}

	vfl->sort_method = type;
	vfl->sort_ascend = ascend;

	vfl->list = filelist_sort(vfl->list, vfl->sort_method, vfl->sort_ascend);

	new_order = g_malloc(i * sizeof(gint));

	work = vfl->list;
	i = 0;
	while (work)
		{
		FileData *fd = work->data;
		new_order[i] = GPOINTER_TO_INT(g_hash_table_lookup(fd_idx_hash, fd));
		i++;
		work = work->next;
		}

	store = GTK_TREE_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(vfl->listview)));
	gtk_tree_store_reorder(store, NULL, new_order);

	g_free(new_order);
	g_hash_table_destroy(fd_idx_hash);
}

/*
 *-----------------------------------------------------------------------------
 * thumb updates
 *-----------------------------------------------------------------------------
 */

static gint vflist_thumb_next(ViewFileList *vfl);

static void vflist_thumb_status(ViewFileList *vfl, gdouble val, const gchar *text)
{
	if (vfl->func_thumb_status)
		{
		vfl->func_thumb_status(vfl, val, text, vfl->data_thumb_status);
		}
}

static void vflist_thumb_cleanup(ViewFileList *vfl)
{
	vflist_thumb_status(vfl, 0.0, NULL);

	vfl->thumbs_count = 0;
	vfl->thumbs_running = FALSE;

	thumb_loader_free(vfl->thumbs_loader);
	vfl->thumbs_loader = NULL;

	vfl->thumbs_filedata = NULL;
}

static void vflist_thumb_stop(ViewFileList *vfl)
{
	if (vfl->thumbs_running) vflist_thumb_cleanup(vfl);
}

static void vflist_thumb_do(ViewFileList *vfl, ThumbLoader *tl, FileData *fd)
{
	GtkTreeStore *store;
	GtkTreeIter iter;

	if (!fd || vflist_find_row(vfl, fd, &iter) < 0) return;

	if (fd->pixbuf) g_object_unref(fd->pixbuf);
	fd->pixbuf = thumb_loader_get_pixbuf(tl, TRUE);

	store = GTK_TREE_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(vfl->listview)));
	gtk_tree_store_set(store, &iter, FILE_COLUMN_THUMB, fd->pixbuf, -1);

	vflist_thumb_status(vfl, (gdouble)(vfl->thumbs_count) / vflist_sidecar_list_count(vfl->list), _("Loading thumbs..."));
}

static void vflist_thumb_error_cb(ThumbLoader *tl, gpointer data)
{
	ViewFileList *vfl = data;

	if (vfl->thumbs_filedata && vfl->thumbs_loader == tl)
		{
		vflist_thumb_do(vfl, tl, vfl->thumbs_filedata);
		}

	while (vflist_thumb_next(vfl));
}

static void vflist_thumb_done_cb(ThumbLoader *tl, gpointer data)
{
	ViewFileList *vfl = data;

	if (vfl->thumbs_filedata && vfl->thumbs_loader == tl)
		{
		vflist_thumb_do(vfl, tl, vfl->thumbs_filedata);
		}

	while (vflist_thumb_next(vfl));
}

static gint vflist_thumb_next(ViewFileList *vfl)
{
	GtkTreePath *tpath;
	FileData *fd = NULL;

	/* first check the visible files */

	if (GTK_WIDGET_REALIZED(vfl->listview) &&
	    gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(vfl->listview), 0, 0, &tpath, NULL, NULL, NULL))
		{
		GtkTreeModel *store;
		GtkTreeIter iter;
		gint valid = TRUE;

		store = gtk_tree_view_get_model(GTK_TREE_VIEW(vfl->listview));
		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_path_free(tpath);

		while (!fd && valid && tree_view_row_get_visibility(GTK_TREE_VIEW(vfl->listview), &iter, FALSE) == 0)
			{
			gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &fd, -1);
			if (fd->pixbuf) fd = NULL;

			valid = gtk_tree_model_iter_next(store, &iter);
			}
		}

	/* then find first undone */

	if (!fd)
		{
		GList *work = vfl->list;
		while (work && !fd)
			{
			FileData *fd_p = work->data;
			if (!fd_p->pixbuf)
				fd = fd_p;
			else
				{
				GList *work2 = fd_p->sidecar_files;

				while (work2 && !fd)
					{
					fd_p = work2->data;
					if (!fd_p->pixbuf) fd = fd_p;
					work2 = work2->next;
					}
				}
			work = work->next;
			}
		}

	if (!fd)
		{
		/* done */
		vflist_thumb_cleanup(vfl);
		return FALSE;
		}

	vfl->thumbs_count++;

	vfl->thumbs_filedata = fd;

	thumb_loader_free(vfl->thumbs_loader);

	vfl->thumbs_loader = thumb_loader_new(options->thumbnails.max_width, options->thumbnails.max_height);
	thumb_loader_set_callbacks(vfl->thumbs_loader,
				   vflist_thumb_done_cb,
				   vflist_thumb_error_cb,
				   NULL,
				   vfl);

	if (!thumb_loader_start(vfl->thumbs_loader, fd->path))
		{
		/* set icon to unknown, continue */
		if (debug) printf("thumb loader start failed %s\n", vfl->thumbs_loader->path);
		vflist_thumb_do(vfl, vfl->thumbs_loader, fd);

		return TRUE;
		}

	return FALSE;
}

static void vflist_thumb_update(ViewFileList *vfl)
{
	vflist_thumb_stop(vfl);
	if (!vfl->thumbs_enabled) return;

	vflist_thumb_status(vfl, 0.0, _("Loading thumbs..."));
	vfl->thumbs_running = TRUE;

	while (vflist_thumb_next(vfl));
}

/*
 *-----------------------------------------------------------------------------
 * row stuff
 *-----------------------------------------------------------------------------
 */

FileData *vflist_index_get_data(ViewFileList *vfl, gint row)
{
	return g_list_nth_data(vfl->list, row);
}

gchar *vflist_index_get_path(ViewFileList *vfl, gint row)
{
	FileData *fd;

	fd = g_list_nth_data(vfl->list, row);

	return (fd ? fd->path : NULL);
}

static gint vflist_row_by_path(ViewFileList *vfl, const gchar *path, FileData **fd)
{
	gint p = 0;
	GList *work;

	if (!path) return -1;

	work = vfl->list;
	while (work)
		{
		FileData *fd_n = work->data;
		if (strcmp(path, fd_n->path) == 0)
			{
			if (fd) *fd = fd_n;
			return p;
			}
		work = work->next;
		p++;
		}

	if (fd) *fd = NULL;
	return -1;
}

gint vflist_index_by_path(ViewFileList *vfl, const gchar *path)
{
	return vflist_row_by_path(vfl, path, NULL);
}

gint vflist_count(ViewFileList *vfl, gint64 *bytes)
{
	if (bytes)
		{
		gint64 b = 0;
		GList *work;

		work = vfl->list;
		while (work)
			{
			FileData *fd = work->data;
			work = work->next;
			b += fd->size;
			}

		*bytes = b;
		}

	return g_list_length(vfl->list);
}

GList *vflist_get_list(ViewFileList *vfl)
{
	GList *list = NULL;
	GList *work;

	work = vfl->list;
	while (work)
		{
		FileData *fd = work->data;
		work = work->next;

		list = g_list_prepend(list, file_data_ref(fd));
		}

	return g_list_reverse(list);
}

/*
 *-----------------------------------------------------------------------------
 * selections
 *-----------------------------------------------------------------------------
 */

static gint vflist_row_is_selected(ViewFileList *vfl, FileData *fd)
{
	GtkTreeModel *store;
	GtkTreeSelection *selection;
	GList *slist;
	GList *work;
	gint found = FALSE;

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vfl->listview));
	slist = gtk_tree_selection_get_selected_rows(selection, &store);
	work = slist;
	while (!found && work)
		{
		GtkTreePath *tpath = work->data;
		FileData *fd_n;
		GtkTreeIter iter;

		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &fd_n, -1);
		if (fd_n == fd) found = TRUE;
		work = work->next;
		}
	g_list_foreach(slist, (GFunc)gtk_tree_path_free, NULL);
	g_list_free(slist);

	return found;
}

gint vflist_index_is_selected(ViewFileList *vfl, gint row)
{
	FileData *fd;

	fd = vflist_index_get_data(vfl, row);
	return vflist_row_is_selected(vfl, fd);
}

gint vflist_selection_count(ViewFileList *vfl, gint64 *bytes)
{
	GtkTreeModel *store;
	GtkTreeSelection *selection;
	GList *slist;
	gint count;

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vfl->listview));
	slist = gtk_tree_selection_get_selected_rows(selection, &store);

	if (bytes)
		{
		gint64 b = 0;
		GList *work;

		work = slist;
		while (work)
			{
			GtkTreePath *tpath = work->data;
			GtkTreeIter iter;
			FileData *fd;

			gtk_tree_model_get_iter(store, &iter, tpath);
			gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &fd, -1);
			b += fd->size;

			work = work->next;
			}

		*bytes = b;
		}

	count = g_list_length(slist);
	g_list_foreach(slist, (GFunc)gtk_tree_path_free, NULL);
	g_list_free(slist);

	return count;
}

GList *vflist_selection_get_list(ViewFileList *vfl)
{
	GtkTreeModel *store;
	GtkTreeSelection *selection;
	GList *slist;
	GList *list = NULL;
	GList *work;

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vfl->listview));
	slist = gtk_tree_selection_get_selected_rows(selection, &store);
	work = slist;
	while (work)
		{
		GtkTreePath *tpath = work->data;
		FileData *fd;
		GtkTreeIter iter;

		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &fd, -1);

		list = g_list_prepend(list, file_data_ref(fd));

		work = work->next;
		}
	g_list_foreach(slist, (GFunc)gtk_tree_path_free, NULL);
	g_list_free(slist);

	return g_list_reverse(list);
}

GList *vflist_selection_get_list_by_index(ViewFileList *vfl)
{
	GtkTreeModel *store;
	GtkTreeSelection *selection;
	GList *slist;
	GList *list = NULL;
	GList *work;

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vfl->listview));
	slist = gtk_tree_selection_get_selected_rows(selection, &store);
	work = slist;
	while (work)
		{
		GtkTreePath *tpath = work->data;
		FileData *fd;
		GtkTreeIter iter;

		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &fd, -1);

		list = g_list_prepend(list, GINT_TO_POINTER(g_list_index(vfl->list, fd)));

		work = work->next;
		}
	g_list_foreach(slist, (GFunc)gtk_tree_path_free, NULL);
	g_list_free(slist);

	return g_list_reverse(list);
}

void vflist_select_all(ViewFileList *vfl)
{
	GtkTreeSelection *selection;

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vfl->listview));
	gtk_tree_selection_select_all(selection);

	vfl->select_fd = NULL;
}

void vflist_select_none(ViewFileList *vfl)
{
	GtkTreeSelection *selection;

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vfl->listview));
	gtk_tree_selection_unselect_all(selection);
}

void vflist_select_by_path(ViewFileList *vfl, const gchar *path)
{
	FileData *fd;

	if (vflist_row_by_path(vfl, path, &fd) < 0) return;

	vflist_select_by_fd(vfl, fd);
}

void vflist_select_by_fd(ViewFileList *vfl, FileData *fd)
{
	GtkTreeIter iter;

	if (vflist_find_row(vfl, fd, &iter) < 0) return;

	tree_view_row_make_visible(GTK_TREE_VIEW(vfl->listview), &iter, TRUE);

	if (!vflist_row_is_selected(vfl, fd))
		{
		GtkTreeSelection *selection;
		GtkTreeModel *store;
		GtkTreePath *tpath;

		selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vfl->listview));
		gtk_tree_selection_unselect_all(selection);
		gtk_tree_selection_select_iter(selection, &iter);
		vflist_move_cursor(vfl, &iter);

		store = gtk_tree_view_get_model(GTK_TREE_VIEW(vfl->listview));
		tpath = gtk_tree_model_get_path(store, &iter);
		gtk_tree_view_set_cursor(GTK_TREE_VIEW(vfl->listview), tpath, NULL, FALSE);
		gtk_tree_path_free(tpath);
		}
}

void vflist_mark_to_selection(ViewFileList *vfl, gint mark, MarkToSelectionMode mode)
{
	GtkTreeModel *store;
	GtkTreeIter iter;
	GtkTreeSelection *selection;
	gint valid;

	g_assert(mark >= 0 && mark < FILEDATA_MARKS_SIZE);

	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vfl->listview));
	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vfl->listview));

	valid = gtk_tree_model_get_iter_first(store, &iter);
	while (valid)
		{
		FileData *fd;
		gboolean mark_val, selected;
		gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, FILE_COLUMN_POINTER, &fd, -1);

		mark_val = fd->marks[mark];
		selected = gtk_tree_selection_iter_is_selected(selection, &iter);

		switch (mode)
			{
			case MTS_MODE_SET: selected = mark_val;
				break;
			case MTS_MODE_OR: selected = mark_val | selected;
				break;
			case MTS_MODE_AND: selected = mark_val & selected;
				break;
			case MTS_MODE_MINUS: selected = !mark_val & selected;
				break;
			}

		if (selected)
			gtk_tree_selection_select_iter(selection, &iter);
		else
			gtk_tree_selection_unselect_iter(selection, &iter);

		valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter);
		}
}

void vflist_selection_to_mark(ViewFileList *vfl, gint mark, SelectionToMarkMode mode)
{
	GtkTreeModel *store;
	GtkTreeSelection *selection;
	GList *slist;
	GList *work;

	g_assert(mark >= 0 && mark < FILEDATA_MARKS_SIZE);

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vfl->listview));
	slist = gtk_tree_selection_get_selected_rows(selection, &store);
	work = slist;
	while (work)
		{
		GtkTreePath *tpath = work->data;
		FileData *fd;
		GtkTreeIter iter;

		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &fd, -1);

		switch (mode)
			{
			case STM_MODE_SET: fd->marks[mark] = 1;
				break;
			case STM_MODE_RESET: fd->marks[mark] = 0;
				break;
			case STM_MODE_TOGGLE: fd->marks[mark] = !fd->marks[mark];
				break;
			}

		gtk_tree_store_set(GTK_TREE_STORE(store), &iter, FILE_COLUMN_MARKS + mark, fd->marks[mark], -1);

		work = work->next;
		}
	g_list_foreach(slist, (GFunc)gtk_tree_path_free, NULL);
	g_list_free(slist);
}

/*
 *-----------------------------------------------------------------------------
 * core (population)
 *-----------------------------------------------------------------------------
 */

static void vflist_listview_set_height(GtkWidget *listview, gint thumb)
{
	GtkTreeViewColumn *column;
	GtkCellRenderer *cell;
	GList *list;

	column = gtk_tree_view_get_column(GTK_TREE_VIEW(listview), FILE_COLUMN_THUMB - 1);
	if (!column) return;

	gtk_tree_view_column_set_fixed_width(column, ((thumb) ? options->thumbnails.max_width : 4) + 10);

	list = gtk_tree_view_column_get_cell_renderers(column);
	if (!list) return;
	cell = list->data;
	g_list_free(list);

	g_object_set(G_OBJECT(cell), "height", (thumb) ? options->thumbnails.max_height : -1, NULL);
	gtk_tree_view_columns_autosize(GTK_TREE_VIEW(listview));
}

static void vflist_populate_view(ViewFileList *vfl)
{
	GtkTreeStore *store;
	GtkTreeIter iter;
	gint thumbs;
	GList *work;
	GtkTreeRowReference *visible_row = NULL;
	GtkTreePath *tpath;
	gint valid;

	store = GTK_TREE_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(vfl->listview)));
	thumbs = vfl->thumbs_enabled;

	vflist_thumb_stop(vfl);

	if (!vfl->list)
		{
		gtk_tree_store_clear(store);
		vflist_send_update(vfl);
		return;
		}

	if (GTK_WIDGET_REALIZED(vfl->listview) &&
	    gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(vfl->listview), 0, 0, &tpath, NULL, NULL, NULL))
		{
		visible_row = gtk_tree_row_reference_new(GTK_TREE_MODEL(store), tpath);
		gtk_tree_path_free(tpath);
		}

	vflist_listview_set_height(vfl->listview, thumbs);

	valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter);

	work = vfl->list;
	while (work)
		{
		gint match;
		FileData *fd = work->data;
		gint done = FALSE;

		while (!done)
			{
			FileData *old_fd = NULL;

			if (valid)
				{
				gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, FILE_COLUMN_POINTER, &old_fd, -1);

				if (fd == old_fd)
					{
					match = 0;
					}
				else
					{
					match = filelist_sort_compare_filedata_full(fd, old_fd, vfl->sort_method, vfl->sort_ascend);
					if (match == 0)
						match = -1; /* probably should not happen*/
					}
				}

			else
				{
				match = -1;
				}

			if (match < 0)
				{
				GtkTreeIter new;

				if (valid)
					{
					gtk_tree_store_insert_before(store, &new, NULL, &iter);
					}
				else
					{
					gtk_tree_store_append(store, &new, NULL);
					}
				vflist_setup_iter_with_sidecars(vfl, store, &new, fd);

				done = TRUE;
				}
			else if (match > 0)
				{
				valid = gtk_tree_store_remove(store, &iter);
				}
			else
				{
				vflist_setup_iter_with_sidecars(vfl, store, &iter, fd);

				if (valid) valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter);

				done = TRUE;
				}
			}
		work = work->next;
		}

	while (valid)
		{
		valid = gtk_tree_store_remove(store, &iter);
		}

	if (visible_row)
		{
		if (gtk_tree_row_reference_valid(visible_row))
			{
			tpath = gtk_tree_row_reference_get_path(visible_row);
			gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(vfl->listview), tpath, NULL, TRUE, 0.0, 0.0);
			gtk_tree_path_free(tpath);
			}
		gtk_tree_row_reference_free(visible_row);
		}

	vflist_send_update(vfl);
	vflist_thumb_update(vfl);
}

gint vflist_refresh(ViewFileList *vfl)
{
	GList *old_list;
	gint ret = TRUE;

	old_list = vfl->list;
	vfl->list = NULL;

	if (vfl->path)
		{
		ret = filelist_read(vfl->path, &vfl->list, NULL);
		}

	vfl->list = filelist_sort(vfl->list, vfl->sort_method, vfl->sort_ascend);
	vflist_populate_view(vfl);

	filelist_free(old_list);

	return ret;
}

/* this overrides the low default of a GtkCellRenderer from 100 to CELL_HEIGHT_OVERRIDE, something sane for our purposes */

#define CELL_HEIGHT_OVERRIDE 512

static void cell_renderer_height_override(GtkCellRenderer *renderer)
{
	GParamSpec *spec;

	spec = g_object_class_find_property(G_OBJECT_GET_CLASS(G_OBJECT(renderer)), "height");
	if (spec && G_IS_PARAM_SPEC_INT(spec))
		{
		GParamSpecInt *spec_int;

		spec_int = G_PARAM_SPEC_INT(spec);
		if (spec_int->maximum < CELL_HEIGHT_OVERRIDE) spec_int->maximum = CELL_HEIGHT_OVERRIDE;
		}
}

static GdkColor *vflist_listview_color_shifted(GtkWidget *widget)
{
	static GdkColor color;
	static GtkWidget *done = NULL;

	if (done != widget)
		{
		GtkStyle *style;

		style = gtk_widget_get_style(widget);
		memcpy(&color, &style->base[GTK_STATE_NORMAL], sizeof(color));
		shift_color(&color, -1, 0);
		done = widget;
		}

	return &color;
}

static void vflist_listview_color_cb(GtkTreeViewColumn *tree_column, GtkCellRenderer *cell,
				     GtkTreeModel *tree_model, GtkTreeIter *iter, gpointer data)
{
	ViewFileList *vfl = data;
	gboolean set;

	gtk_tree_model_get(tree_model, iter, FILE_COLUMN_COLOR, &set, -1);
	g_object_set(G_OBJECT(cell),
		     "cell-background-gdk", vflist_listview_color_shifted(vfl->listview),
		     "cell-background-set", set, NULL);
}

static void vflist_listview_add_column(ViewFileList *vfl, gint n, const gchar *title, gint image, gint right_justify, gint expand)
{
	GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;

	column = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(column, title);
	gtk_tree_view_column_set_min_width(column, 4);

	if (!image)
		{
		gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_GROW_ONLY);
		renderer = gtk_cell_renderer_text_new();
		if (right_justify)
			{
			g_object_set(G_OBJECT(renderer), "xalign", 1.0, NULL);
			}
		gtk_tree_view_column_pack_start(column, renderer, TRUE);
		gtk_tree_view_column_add_attribute(column, renderer, "text", n);
		if (expand)
			gtk_tree_view_column_set_expand(column, TRUE);
		}
	else
		{
		gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
		renderer = gtk_cell_renderer_pixbuf_new();
		cell_renderer_height_override(renderer);
		gtk_tree_view_column_pack_start(column, renderer, TRUE);
		gtk_tree_view_column_add_attribute(column, renderer, "pixbuf", n);
		}

	gtk_tree_view_column_set_cell_data_func(column, renderer, vflist_listview_color_cb, vfl, NULL);
	g_object_set_data (G_OBJECT (column), "column_store_idx", GUINT_TO_POINTER(n));
	g_object_set_data (G_OBJECT (renderer), "column_store_idx", GUINT_TO_POINTER(n));

	gtk_tree_view_append_column(GTK_TREE_VIEW(vfl->listview), column);
}

static void vflist_listview_mark_toggled(GtkCellRendererToggle *cell, gchar *path_str, GtkTreeStore *store)
{
	GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
	GtkTreeIter iter;
	FileData *fd;
	gboolean mark;
	guint col_idx;

	if (!path || !gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &iter, path))
    		return;

	col_idx = GPOINTER_TO_INT(g_object_get_data (G_OBJECT(cell), "column_store_idx"));

	g_assert(col_idx >= FILE_COLUMN_MARKS && col_idx <= FILE_COLUMN_MARKS_LAST);

	gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, FILE_COLUMN_POINTER, &fd, col_idx, &mark, -1);
	mark = !mark;
	fd->marks[col_idx - FILE_COLUMN_MARKS] = mark;

	gtk_tree_store_set(store, &iter, col_idx, mark, -1);
	gtk_tree_path_free(path);
}

static void vflist_listview_add_column_toggle(ViewFileList *vfl, gint n, const gchar *title)
{
	GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;
	GtkTreeStore *store;
	gint index;

	store = GTK_TREE_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(vfl->listview)));

	renderer = gtk_cell_renderer_toggle_new();
	column = gtk_tree_view_column_new_with_attributes(title, renderer, "active", n, NULL);

	gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
	g_object_set_data (G_OBJECT (column), "column_store_idx", GUINT_TO_POINTER(n));
	g_object_set_data (G_OBJECT (renderer), "column_store_idx", GUINT_TO_POINTER(n));

	index = gtk_tree_view_append_column(GTK_TREE_VIEW(vfl->listview), column);
	gtk_tree_view_column_set_fixed_width(column, 16);
	gtk_tree_view_column_set_visible(column, vfl->marks_enabled);


	g_signal_connect(G_OBJECT(renderer), "toggled", G_CALLBACK(vflist_listview_mark_toggled), store);
}

/*
 *-----------------------------------------------------------------------------
 * base
 *-----------------------------------------------------------------------------
 */

gint vflist_set_path(ViewFileList *vfl, const gchar *path)
{
	GtkTreeStore *store;

	if (!path) return FALSE;
	if (vfl->path && strcmp(path, vfl->path) == 0) return TRUE;

	g_free(vfl->path);
	vfl->path = g_strdup(path);

	/* force complete reload */
	store = GTK_TREE_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(vfl->listview)));
	gtk_tree_store_clear(store);

	filelist_free(vfl->list);
	vfl->list = NULL;

	return vflist_refresh(vfl);
}

static void vflist_destroy_cb(GtkWidget *widget, gpointer data)
{
	ViewFileList *vfl = data;

	if (vfl->popup)
		{
		g_signal_handlers_disconnect_matched(G_OBJECT(vfl->popup), G_SIGNAL_MATCH_DATA,
						     0, 0, 0, NULL, vfl);
		gtk_widget_destroy(vfl->popup);
		}

	vflist_select_idle_cancel(vfl);
	vflist_thumb_stop(vfl);

	g_free(vfl->path);
	filelist_free(vfl->list);
	g_free(vfl);
}

ViewFileList *vflist_new(const gchar *path, gint thumbs)
{
	ViewFileList *vfl;
	GtkTreeStore *store;
	GtkTreeSelection *selection;

	GType flist_types[FILE_COLUMN_COUNT];
	int i;

	vfl = g_new0(ViewFileList, 1);

	vfl->path = NULL;
	vfl->list = NULL;
	vfl->click_fd = NULL;
	vfl->select_fd = NULL;
	vfl->sort_method = SORT_NAME;
	vfl->sort_ascend = TRUE;
	vfl->thumbs_enabled = thumbs;

	vfl->thumbs_running = FALSE;
	vfl->thumbs_count = 0;
	vfl->thumbs_loader = NULL;
	vfl->thumbs_filedata = NULL;

	vfl->select_idle_id = -1;

	vfl->popup = NULL;

	vfl->widget = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(vfl->widget), GTK_SHADOW_IN);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(vfl->widget),
				       GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
	g_signal_connect(G_OBJECT(vfl->widget), "destroy",
			 G_CALLBACK(vflist_destroy_cb), vfl);

	flist_types[FILE_COLUMN_POINTER] = G_TYPE_POINTER;
	flist_types[FILE_COLUMN_THUMB] = GDK_TYPE_PIXBUF;
	flist_types[FILE_COLUMN_NAME] = G_TYPE_STRING;
	flist_types[FILE_COLUMN_SIDECARS] = G_TYPE_STRING;
	flist_types[FILE_COLUMN_SIZE] = G_TYPE_STRING;
	flist_types[FILE_COLUMN_DATE] = G_TYPE_STRING;
	flist_types[FILE_COLUMN_COLOR] = G_TYPE_BOOLEAN;
	for (i = FILE_COLUMN_MARKS; i < FILE_COLUMN_MARKS + FILEDATA_MARKS_SIZE; i++)
		flist_types[i] = G_TYPE_BOOLEAN;

	store = gtk_tree_store_newv(FILE_COLUMN_COUNT, flist_types);

	vfl->listview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
	g_object_unref(store);

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vfl->listview));
	gtk_tree_selection_set_mode(GTK_TREE_SELECTION(selection), GTK_SELECTION_MULTIPLE);
	gtk_tree_selection_set_select_function(selection, vflist_select_cb, vfl, NULL);

	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(vfl->listview), FALSE);
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(vfl->listview), FALSE);

	vflist_listview_add_column(vfl, FILE_COLUMN_THUMB, "", TRUE, FALSE, FALSE);

	for(i = 0; i < FILEDATA_MARKS_SIZE;i++)
    		vflist_listview_add_column_toggle(vfl, i + FILE_COLUMN_MARKS, "");

	vflist_listview_add_column(vfl, FILE_COLUMN_NAME, _("Name"), FALSE, FALSE, FALSE);
	vflist_listview_add_column(vfl, FILE_COLUMN_SIDECARS, _("SC"), FALSE, FALSE, FALSE);

	vflist_listview_add_column(vfl, FILE_COLUMN_SIZE, _("Size"), FALSE, TRUE, FALSE);
	vflist_listview_add_column(vfl, FILE_COLUMN_DATE, _("Date"), FALSE, TRUE, FALSE);


	g_signal_connect(G_OBJECT(vfl->listview), "key_press_event",
			 G_CALLBACK(vflist_press_key_cb), vfl);

	gtk_container_add (GTK_CONTAINER(vfl->widget), vfl->listview);
	gtk_widget_show(vfl->listview);

	vflist_dnd_init(vfl);

	g_signal_connect(G_OBJECT(vfl->listview), "button_press_event",
			 G_CALLBACK(vflist_press_cb), vfl);
	g_signal_connect(G_OBJECT(vfl->listview), "button_release_event",
			 G_CALLBACK(vflist_release_cb), vfl);


	if (path) vflist_set_path(vfl, path);

	return vfl;
}

void vflist_set_status_func(ViewFileList *vfl,
			    void (*func)(ViewFileList *vfl, gpointer data), gpointer data)
{
	vfl->func_status = func;
	vfl->data_status = data;
}

void vflist_set_thumb_status_func(ViewFileList *vfl,
				  void (*func)(ViewFileList *vfl, gdouble val, const gchar *text, gpointer data),
				  gpointer data)
{
	vfl->func_thumb_status = func;
	vfl->data_thumb_status = data;
}

void vflist_thumb_set(ViewFileList *vfl, gint enable)
{
	if (vfl->thumbs_enabled == enable) return;

	vfl->thumbs_enabled = enable;
	vflist_refresh(vfl);
}

void vflist_marks_set(ViewFileList *vfl, gint enable)
{
	GList *columns, *work;

	if (vfl->marks_enabled == enable) return;

	vfl->marks_enabled = enable;

	columns = gtk_tree_view_get_columns(GTK_TREE_VIEW(vfl->listview));

	work = columns;
	while (work)
		{
		GtkTreeViewColumn *column = work->data;
		gint col_idx = GPOINTER_TO_INT(g_object_get_data (G_OBJECT(column), "column_store_idx"));
		work = work->next;

		if (col_idx <= FILE_COLUMN_MARKS_LAST && col_idx >= FILE_COLUMN_MARKS)
			gtk_tree_view_column_set_visible(column, enable);
		}

	g_list_free(columns);
	//vflist_refresh(vfl);
}

void vflist_set_layout(ViewFileList *vfl, LayoutWindow *layout)
{
	vfl->layout = layout;
}

/*
 *-----------------------------------------------------------------------------
 * maintenance (for rename, move, remove)
 *-----------------------------------------------------------------------------
 */

static gint vflist_maint_find_closest(ViewFileList *vfl, gint row, gint count, GList *ignore_list)
{
	GList *list = NULL;
	GList *work;
	gint rev = row - 1;
	row ++;

	work = ignore_list;
	while (work)
		{
		gint f = vflist_index_by_path(vfl, work->data);
		if (f >= 0) list = g_list_prepend(list, GINT_TO_POINTER(f));
		work = work->next;
		}

	while (list)
		{
		gint c = TRUE;
		work = list;
		while (work && c)
			{
			gpointer p = work->data;
			work = work->next;
			if (row == GPOINTER_TO_INT(p))
				{
				row++;
				c = FALSE;
				}
			if (rev == GPOINTER_TO_INT(p))
				{
				rev--;
				c = FALSE;
				}
			if (!c) list = g_list_remove(list, p);
			}
		if (c && list)
			{
			g_list_free(list);
			list = NULL;
			}
		}
	if (row > count - 1)
		{
		if (rev < 0)
			return -1;
		else
			return rev;
		}
	else
		{
		return row;
		}
}

gint vflist_maint_renamed(ViewFileList *vfl, FileData *fd)
{
	gint ret = FALSE;
	gchar *source_base;
	gchar *dest_base;

	if (g_list_index(vfl->list, fd) < 0) return FALSE;

	source_base = remove_level_from_path(fd->change->source);
	dest_base = remove_level_from_path(fd->change->dest);


	if (strcmp(source_base, dest_base) == 0)
		{
		GtkTreeStore *store;
		GtkTreeIter iter;
		GtkTreeIter position;
		gint old_row;
		gint n;

		old_row = g_list_index(vfl->list, fd);

		vfl->list = g_list_remove(vfl->list, fd);

		vfl->list = filelist_insert_sort(vfl->list, fd, vfl->sort_method, vfl->sort_ascend);
		n = g_list_index(vfl->list, fd);

		store = GTK_TREE_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(vfl->listview)));
		if (vflist_find_row(vfl, fd, &iter) >= 0 &&
		    gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(store), &position, NULL, n))
			{
			if (old_row >= n)
				{
				gtk_tree_store_move_before(store, &iter, &position);
				}
			else
				{
				gtk_tree_store_move_after(store, &iter, &position);
				}
			}
		gtk_tree_store_set(store, &iter, FILE_COLUMN_NAME, fd->name, -1);

		ret = TRUE;
		}
	else
		{
		ret = vflist_maint_removed(vfl, fd, NULL);
		}

	g_free(source_base);
	g_free(dest_base);

	return ret;
}

gint vflist_maint_removed(ViewFileList *vfl, FileData *fd, GList *ignore_list)
{
	GtkTreeIter iter;
	GList *list;
	gint row;
	gint new_row = -1;

	row = g_list_index(vfl->list, fd);
	if (row < 0) return FALSE;

	if (vflist_index_is_selected(vfl, row) &&
	    layout_image_get_collection(vfl->layout, NULL) == NULL)
		{
		gint n;

		n = vflist_count(vfl, NULL);
		if (ignore_list)
			{
			new_row = vflist_maint_find_closest(vfl, row, n, ignore_list);
			if (debug) printf("row = %d, closest is %d\n", row, new_row);
			}
		else
			{
			if (row + 1 < n)
				{
				new_row = row + 1;
				}
			else if (row > 0)
				{
				new_row = row - 1;
				}
			}
		vflist_select_none(vfl);
		if (new_row >= 0)
			{
			fd = vflist_index_get_data(vfl, new_row);
			if (vflist_find_row(vfl, fd, &iter) >= 0)
				{
				GtkTreeSelection *selection;

				selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vfl->listview));
				gtk_tree_selection_select_iter(selection, &iter);
				vflist_move_cursor(vfl, &iter);
				}
			}
		}

	fd = vflist_index_get_data(vfl, row);
	if (vflist_find_row(vfl, fd, &iter) >= 0)
		{
		GtkTreeStore *store;
		store = GTK_TREE_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(vfl->listview)));
		gtk_tree_store_remove(store, &iter);
		}
	list = g_list_nth(vfl->list, row);
	fd = list->data;

	/* thumbnail loader check */
	if (fd == vfl->thumbs_filedata) vfl->thumbs_filedata = NULL;
	if (vfl->thumbs_count > 0) vfl->thumbs_count--;

	vfl->list = g_list_remove(vfl->list, fd);
	file_data_unref(fd);

	vflist_send_update(vfl);

	return TRUE;
}

gint vflist_maint_moved(ViewFileList *vfl, FileData *fd, GList *ignore_list)
{
	gint ret = FALSE;
	gchar *buf;

	if (!fd->change->source || !vfl->path) return FALSE;

	buf = remove_level_from_path(fd->change->source);

	if (strcmp(buf, vfl->path) == 0)
		{
		ret = vflist_maint_removed(vfl, fd, ignore_list);
		}

	g_free(buf);

	return ret;
}
