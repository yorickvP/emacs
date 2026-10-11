/* Functions for creating and updating GTK 4 widgets.

Copyright (C) 2026 Free Software Foundation, Inc.

This file is part of GNU Emacs.

GNU Emacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.  */

/* This file implements the toolkit interface declared in gtkutil.h
   for the pure GTK (pgtk) port built with GTK 4; it takes the place of
   gtkutil.c in that configuration.  See the comments in gtkutil.c for
   the meaning of functions that work the same way here.

   Emacs frame pixels are device pixels, while GTK lays out widgets in
   logical pixels.  The ratio between the two is the scale of the
   surface a frame is displayed on, FRAME_GTK_SCALE, which can be
   fractional.  Every size and position passed between Emacs and GTK
   must therefore be converted.  */

#include <config.h>

#include <stdlib.h>
#include <math.h>
#include <c-ctype.h>

#include "lisp.h"
#include "dispextern.h"
#include "frame.h"
#include "systime.h"
#include "pgtkterm.h"
#include "blockinput.h"
#include "window.h"
#include "gtkutil.h"
#include "termhooks.h"
#include "keyboard.h"
#include "coding.h"
#include "font.h"
#include "menu.h"
#include "xsettings.h"
#include "emacsgtkfixed.h"

#include <gdk/gdkkeysyms.h>

#define TB_INFO_KEY "xg_frame_tb_info"
struct xg_frame_tb_info
{
  Lisp_Object last_tool_bar;
  Lisp_Object style;
  int n_last_items;
  int hmargin, vmargin;
  GtkTextDirection dir;
};

/* The key for the data we put in the GtkPopoverMenuBar to remember
   its model.  */
#define XG_MENUBAR_MODEL "emacs-menubar-model"

bool xg_ignore_gtk_scrollbar;

#ifdef HAVE_XWIDGETS
bool xg_gtk_initialized;
#endif

/* The width and height of scroll bars for the current theme, in
   logical pixels.  */
static int scroll_bar_width_for_theme;
static int scroll_bar_height_for_theme;

/* The minimum length of vertical and horizontal scroll bars for the
   current theme, in logical pixels.  */
static int scroll_bar_min_length_for_theme;
static int scroll_bar_min_width_for_theme;

static void update_theme_scrollbar_width (void);
static void update_theme_scrollbar_height (void);
static GtkWidget *xg_get_widget_from_map (ptrdiff_t idx);


/***********************************************************************
                      Display handling functions
 ***********************************************************************/

/* Keep track of the default display, or NULL if there is none.  Emacs
   may close all its displays.  */

static GdkDisplay *gdpy_def;

/* Open a display named by DISPLAY_NAME.  The display is returned in
   *DPY, or NULL if it could not be opened.  */

void
xg_display_open (char *display_name, GdkDisplay **dpy)
{
  GdkDisplay *gdpy;

  unrequest_sigio ();  /* See comment in x_display_ok, xterm.c.  */
  gdpy = gdk_display_open (strlen (display_name) == 0 ? NULL : display_name);
  request_sigio ();
  if (!gdpy_def && gdpy)
    {
      gdpy_def = gdpy;
      gdk_display_manager_set_default_display (gdk_display_manager_get (),
					       gdpy);
    }

  *dpy = gdpy;
}

/* Return the integer scale used by functions shared with the X port.
   Emacs pixels are device pixels under GTK 4, so there is nothing to
   scale here; see FRAME_GTK_SCALE for the real scale factor.  */

int
xg_get_scale (struct frame *f)
{
  return 1;
}

/* Close display GDPY.  */

void
xg_display_close (GdkDisplay *gdpy)
{
  /* If this is the default display, try to change it before closing.
     If there is no other display to use, gdpy_def is set to NULL, and
     the next call to xg_display_open resets the default display.  */
  if (gdk_display_get_default () == gdpy)
    {
      struct pgtk_display_info *dpyinfo;
      GdkDisplay *gdpy_new = NULL;

      /* Find another display.  */
      for (dpyinfo = x_display_list; dpyinfo; dpyinfo = dpyinfo->next)
	if (dpyinfo->gdpy != gdpy)
	  {
	    gdpy_new = dpyinfo->gdpy;
	    gdk_display_manager_set_default_display (gdk_display_manager_get (),
						     gdpy_new);
	    break;
	  }
      gdpy_def = gdpy_new;
    }

  gdk_display_close (gdpy);
}


/***********************************************************************
                      Utility functions
 ***********************************************************************/

/* Create and return the cursor to be used for popup menus and scroll
   bars on display GDPY.  */

GdkCursor *
xg_create_default_cursor (GdkDisplay *gdpy)
{
  return gdk_cursor_new_from_name ("default", NULL);
}

/* Return a texture holding a copy of the contents of SURFACE.  */

GdkTexture *
xg_texture_from_surface (cairo_surface_t *surface)
{
  cairo_surface_t *image = NULL;
  int width, height, stride;
  GBytes *bytes;
  GdkTexture *texture;

  if (cairo_surface_get_type (surface) != CAIRO_SURFACE_TYPE_IMAGE
      || cairo_image_surface_get_format (surface) != CAIRO_FORMAT_ARGB32)
    {
      double x1, y1, x2, y2;
      cairo_t *cr;

      /* Convert SURFACE to an ARGB32 image surface first.  */
      cr = cairo_create (surface);
      cairo_clip_extents (cr, &x1, &y1, &x2, &y2);
      cairo_destroy (cr);

      if (cairo_surface_get_type (surface) == CAIRO_SURFACE_TYPE_IMAGE)
	{
	  x1 = y1 = 0;
	  x2 = cairo_image_surface_get_width (surface);
	  y2 = cairo_image_surface_get_height (surface);
	}

      image = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
					  max (1, x2 - x1), max (1, y2 - y1));
      cr = cairo_create (image);
      cairo_set_source_surface (cr, surface, -x1, -y1);
      cairo_paint (cr);
      cairo_destroy (cr);
      surface = image;
    }

  cairo_surface_flush (surface);
  width = cairo_image_surface_get_width (surface);
  height = cairo_image_surface_get_height (surface);
  stride = cairo_image_surface_get_stride (surface);

  /* GDK_MEMORY_DEFAULT is the same format as CAIRO_FORMAT_ARGB32,
     regardless of byte order.  */
  bytes = g_bytes_new (cairo_image_surface_get_data (surface),
		       (gsize) stride * height);
  texture = gdk_memory_texture_new (width, height, GDK_MEMORY_DEFAULT,
				    bytes, stride);
  g_bytes_unref (bytes);

  if (image)
    cairo_surface_destroy (image);

  return texture;
}

/* Insert NODE into linked LIST.  */

static void
xg_list_insert (xg_list_node *list, xg_list_node *node)
{
  xg_list_node *list_start = list->next;

  if (list_start) list_start->prev = node;
  node->next = list_start;
  node->prev = 0;
  list->next = node;
}

/* Remove NODE from linked LIST.  */

static void
xg_list_remove (xg_list_node *list, xg_list_node *node)
{
  xg_list_node *list_start = list->next;
  if (node == list_start)
    {
      list->next = node->next;
      if (list->next) list->next->prev = 0;
    }
  else
    {
      node->prev->next = node->next;
      if (node->next) node->next->prev = node->prev;
    }
}

/* Allocate and return a utf8 version of STR.  If STR is already
   utf8 or NULL, just return a copy of STR.
   A new string is allocated and the caller must free the result
   with g_free.  */

static char *
get_utf8_string (const char *str)
{
  char *utf8_str;

  if (!str) return NULL;

  /* If not UTF-8, try current locale.  */
  if (!g_utf8_validate (str, -1, NULL))
    utf8_str = g_locale_to_utf8 (str, -1, 0, 0, 0);
  else
    return g_strdup (str);

  if (!utf8_str)
    {
      /* Probably some control characters in str.  Escape them. */
      ptrdiff_t len;
      ptrdiff_t nr_bad = 0;
      gsize bytes_read;
      gsize bytes_written;
      unsigned char *p = (unsigned char *)str;
      char *cp, *up;
      GError *err = NULL;

      while (! (cp = g_locale_to_utf8 ((char *)p, -1, &bytes_read,
                                       &bytes_written, &err))
             && err->code == G_CONVERT_ERROR_ILLEGAL_SEQUENCE)
        {
          ++nr_bad;
          p += bytes_written+1;
          g_error_free (err);
          err = NULL;
        }

      if (err)
        {
          g_error_free (err);
          err = NULL;
        }
      if (cp) g_free (cp);

      len = strlen (str);
      ptrdiff_t alloc;
      if (ckd_mul (&alloc, nr_bad, 4)
	  || ckd_add (&alloc, alloc, len + 1)
	  || SIZE_MAX < alloc)
	memory_full (SIZE_MAX);
      up = utf8_str = xmalloc (alloc);
      p = (unsigned char *)str;

      while (! (cp = g_locale_to_utf8 ((char *)p, -1, &bytes_read,
                                       &bytes_written, &err))
             && err->code == G_CONVERT_ERROR_ILLEGAL_SEQUENCE)
        {
          memcpy (up, p, bytes_written);
          up += bytes_written;
          up += sprintf (up, "\\%03o", p[bytes_written]);
          p += bytes_written + 1;
          g_error_free (err);
          err = NULL;
        }

      if (cp)
        {
          strcpy (up, cp);
          g_free (cp);
        }
      if (err)
        {
          g_error_free (err);
          err = NULL;
        }
    }
  return utf8_str;
}

/* Return a copy of LABEL, a UTF-8 string, with underscores doubled so
   GTK does not interpret them as mnemonics.  Free the result with
   g_free.  */

static char *
xg_escape_mnemonics (const char *label)
{
  GString *s;

  if (!label)
    return g_strdup ("");

  s = g_string_sized_new (strlen (label) + 4);
  for (; *label; label++)
    {
      if (*label == '_')
	g_string_append_c (s, '_');
      g_string_append_c (s, *label);
    }

  return g_string_free (s, FALSE);
}

static Lisp_Object
file_for_image (Lisp_Object image)
{
  Lisp_Object specified_file = Qnil;
  Lisp_Object tail;

  for (tail = XCDR (image);
       NILP (specified_file) && CONSP (tail) && CONSP (XCDR (tail));
       tail = XCDR (XCDR (tail)))
    if (EQ (XCAR (tail), QCfile))
      specified_file = XCAR (XCDR (tail));

  return specified_file;
}

/* Check for special colors used in face spec for region face.
   The colors are fetched from the GTK theme.
   Return true if color was found, false if not.  */

bool
xg_check_special_colors (struct frame *f,
                         const char *color_name,
                         Emacs_Color *color)
{
  bool success_p = false;
  bool get_bg, get_fg;
  GtkStyleContext *gsty;
  GdkRGBA col;
  char buf[sizeof "#rrrrggggbbbb"];
  static const char *bg_names[] = { "theme_selected_bg_color",
				    "accent_bg_color", NULL };
  static const char *fg_names[] = { "theme_selected_fg_color",
				    "accent_fg_color", NULL };
  const char **names;

  get_bg = !strcmp ("gtk_selection_bg_color", color_name);
  get_fg = !get_bg && !strcmp ("gtk_selection_fg_color", color_name);

  while (FRAME_PARENT_FRAME (f))
    f = FRAME_PARENT_FRAME (f);

  if (!FRAME_GTK_WIDGET (f) || !(get_bg || get_fg))
    return success_p;

  block_input ();
  gsty = gtk_widget_get_style_context (FRAME_GTK_WIDGET (f));

  for (names = get_bg ? bg_names : fg_names; *names; names++)
    if (gtk_style_context_lookup_color (gsty, *names, &col))
      {
	sprintf (buf, "#%04x%04x%04x",
		 (unsigned) (col.red * 65535),
		 (unsigned) (col.green * 65535),
		 (unsigned) (col.blue * 65535));
	success_p = pgtk_parse_color (f, buf, color) != 0;
	break;
      }

  unblock_input ();
  return success_p;
}


/***********************************************************************
                              Tooltips
 ***********************************************************************/

/* GTK tooltips are only used when `use-system-tooltips' is non-nil;
   otherwise, Emacs displays tooltip frames itself.  */

bool
xg_prepare_tooltip (struct frame *f, Lisp_Object string,
		    int *width, int *height)
{
  return false;
}

void
xg_show_tooltip (struct frame *f, Lisp_Object string)
{
  Lisp_Object encoded_string = ENCODE_UTF_8 (string);
  GtkWidget *widget = FRAME_GTK_WIDGET (f);

  gtk_widget_set_tooltip_text (widget, SSDATA (encoded_string));
  gtk_widget_trigger_tooltip_query (widget);
}

bool
xg_hide_tooltip (struct frame *f)
{
  if (FRAME_GTK_WIDGET (f))
    gtk_widget_set_tooltip_text (FRAME_GTK_WIDGET (f), NULL);
  return true;
}


/***********************************************************************
    General functions for creating widgets, resizing, events, e.t.c.
 ***********************************************************************/

/** Function to handle resize of native frame F to WIDTH and HEIGHT
    pixels after the edit widget has been allocated.  */

void
xg_frame_resized (struct frame *f, int width, int height)
{
  /* Ignore case where size of native rectangle didn't change, unless
     the window manager refused to resize the window after the font
     changed, which leaves the numbers of columns and lines to be
     recomputed.  */
  if (width != FRAME_PIXEL_WIDTH (f)
      || height != FRAME_PIXEL_HEIGHT (f)
      || (FRAME_COLS (f)
	  != FRAME_PIXEL_TO_TEXT_WIDTH (f, width) / FRAME_COLUMN_WIDTH (f))
      || (FRAME_LINES (f)
	  != FRAME_PIXEL_TO_TEXT_HEIGHT (f, height) / FRAME_LINE_HEIGHT (f))
      || (f->new_size_p
	  && ((f->new_width >= 0 && width != f->new_width)
	      || (f->new_height >= 0 && height != f->new_height))))
    {
      if (CONSP (frame_size_history))
	frame_size_history_extra
	  (f, build_string ("xg_frame_resized, changed"),
	   FRAME_PIXEL_WIDTH (f), FRAME_PIXEL_HEIGHT (f), width, height,
	   f->new_size_p ? f->new_width : -1,
	   f->new_size_p ? f->new_height : -1);

      FRAME_RIF (f)->clear_under_internal_border (f);
      change_frame_size (f, width, height, false, true, false);
      SET_FRAME_GARBAGED (f);
      cancel_mouse_face (f);
    }
  else if (CONSP (frame_size_history))
    frame_size_history_extra
      (f, build_string ("xg_frame_resized, unchanged"),
       FRAME_PIXEL_WIDTH (f), FRAME_PIXEL_HEIGHT (f), width, height,
       f->new_size_p ? f->new_width : -1,
       f->new_size_p ? f->new_height : -1);
}

/* Return in *WIDTH and *HEIGHT the logical size the menu bar and tool
   bars of frame F want.  This is known before the window has been
   laid out again after one of them changed, unlike their allocations.  */

static void
xg_frame_bars_size (struct frame *f, int *width, int *height)
{
  struct pgtk_output *x = f->output_data.pgtk;
  GtkWidget *child;
  int nat;

  *width = *height = 0;

  for (child = gtk_widget_get_first_child (x->vbox_widget); child;
       child = gtk_widget_get_next_sibling (child))
    if (child != x->hbox_widget && gtk_widget_get_visible (child))
      {
	gtk_widget_measure (child, GTK_ORIENTATION_VERTICAL, -1,
			    NULL, &nat, NULL, NULL);
	*height += nat;
      }

  for (child = gtk_widget_get_first_child (x->hbox_widget); child;
       child = gtk_widget_get_next_sibling (child))
    if (child != FRAME_GTK_WIDGET (f) && gtk_widget_get_visible (child))
      {
	gtk_widget_measure (child, GTK_ORIENTATION_HORIZONTAL, -1,
			    NULL, &nat, NULL, NULL);
	*width += nat;
      }
}

/* Return the logical height of the title bar GTK draws itself in the
   window of frame F, or 0 if there is none.  */

static int
xg_frame_title_bar_height (struct frame *f)
{
  GtkWidget *window = FRAME_GTK_OUTER_WIDGET (f);
  GtkWidget *child;
  int height = 0, nat;

  if (!gtk_window_get_decorated (GTK_WINDOW (window))
      || gtk_window_is_fullscreen (GTK_WINDOW (window)))
    return 0;

  /* The title bar is the child of the window besides its contents;
     popovers and other native children don't count.  */
  for (child = gtk_widget_get_first_child (window); child;
       child = gtk_widget_get_next_sibling (child))
    if (child != f->output_data.pgtk->vbox_widget
	&& !GTK_IS_NATIVE (child)
	&& gtk_widget_get_visible (child)
	&& gtk_widget_get_child_visible (child))
      {
	gtk_widget_measure (child, GTK_ORIENTATION_VERTICAL, -1,
			    NULL, &nat, NULL, NULL);
	height += nat;
      }

  return height;
}

/* Ask GTK to make the edit widget of frame F WIDTH by HEIGHT device
   pixels.  Return true if the request was made through the toplevel
   window, which might not be honored by the window manager.  */

static bool
xg_request_frame_size (struct frame *f, int width, int height)
{
  Lisp_Object fullscreen = get_frame_param (f, Qfullscreen);
  double scale = FRAME_GTK_SCALE (f);
  /* Round up, lest the edit widget end up a device pixel short of
     what was asked for and lose a column or line.  */
  int lwidth = max (1, (int) ceil (width / scale - 0.001));
  int lheight = max (1, (int) ceil (height / scale - 0.001));
  GtkWidget *fixed = FRAME_GTK_WIDGET (f);

  /* The natural size of the edit widget is the size Emacs wants.  It
     determines the initial size of a toplevel window, and the size of
     a child frame inside its parent.  */
  f->output_data.pgtk->preferred_width = lwidth;
  f->output_data.pgtk->preferred_height = lheight;
  gtk_widget_queue_resize (fixed);

  if (FRAME_GTK_OUTER_WIDGET (f))
    {
      struct pgtk_output *x = f->output_data.pgtk;
      GtkWindow *window = GTK_WINDOW (FRAME_GTK_OUTER_WIDGET (f));
      int dwidth, dheight, bars_width, bars_height;

      /* Once the window has been laid out, set its default size,
	 which applies to it from then on, to that of its contents,
	 the edit widget and the bars around it, plus any title bar GTK
	 draws.  Before that, the natural size set above is used.  */
      if (gtk_widget_get_width (x->vbox_widget) > 0)
	{
	  gtk_window_get_default_size (window, &dwidth, &dheight);
	  xg_frame_bars_size (f, &bars_width, &bars_height);

	  if (!(EQ (fullscreen, Qfullwidth)
		&& width == FRAME_PIXEL_WIDTH (f)))
	    dwidth = lwidth + bars_width;
	  if (!(EQ (fullscreen, Qfullheight)
		&& height == FRAME_PIXEL_HEIGHT (f)))
	    dheight = lheight + bars_height + xg_frame_title_bar_height (f);

	  gtk_window_set_default_size (window, max (1, dwidth),
				       max (1, dheight));
	}

      return true;
    }

  return false;
}

/* Return whether the window of frame F gets the size it asks for,
   instead of one the compositor chooses, as for maximized and tiled
   windows.  */

static bool
xg_frame_sizes_window_p (struct frame *f)
{
  GdkSurface *surface = FRAME_GDK_SURFACE (f);

  return (surface && GDK_IS_TOPLEVEL (surface)
	  && !(gdk_toplevel_get_state (GDK_TOPLEVEL (surface))
	       & (GDK_TOPLEVEL_STATE_MAXIMIZED
		  | GDK_TOPLEVEL_STATE_FULLSCREEN
		  | GDK_TOPLEVEL_STATE_TILED
		  | GDK_TOPLEVEL_STATE_TOP_TILED
		  | GDK_TOPLEVEL_STATE_RIGHT_TILED
		  | GDK_TOPLEVEL_STATE_BOTTOM_TILED
		  | GDK_TOPLEVEL_STATE_LEFT_TILED)));
}

/** Resize the outer window of frame F.  WIDTH and HEIGHT are the new
    native pixel sizes of F.  */

void
xg_frame_set_char_size (struct frame *f, int width, int height)
{
  xg_wm_set_size_hint (f, 0, 0);
  xg_request_frame_size (f, width, height);

  SET_FRAME_GARBAGED (f);
  cancel_mouse_face (f);

  /* The window manager may override our resize request for a mapped
     frame, so its size is only known once the edit widget has been
     allocated.  Until then, assume a window whose size is ours to
     choose gets the size asked for, so that changes made to the frame
     meanwhile, like adding a tool bar, build on that size instead of
     undoing the request.  The allocation corrects any mistake.  For
     unmapped windows and child frames, set rows/cols now.  */
  if (FRAME_VISIBLE_P (f) && !FRAME_PARENT_FRAME (f))
    {
      if (CONSP (frame_size_history))
	frame_size_history_extra
	  (f, build_string ("xg_frame_set_char_size, visible"),
	   FRAME_PIXEL_WIDTH (f), FRAME_PIXEL_HEIGHT (f), width, height,
	   f->new_width, f->new_height);

      if (xg_frame_sizes_window_p (f))
	adjust_frame_size (f, FRAME_PIXEL_TO_TEXT_WIDTH (f, width),
			   FRAME_PIXEL_TO_TEXT_HEIGHT (f, height),
			   5, 0, Qxg_frame_set_char_size);
    }
  else
    {
      if (CONSP (frame_size_history))
	frame_size_history_extra
	  (f, build_string ("xg_frame_set_char_size, invisible"),
	   FRAME_PIXEL_WIDTH (f), FRAME_PIXEL_HEIGHT (f), width, height,
	   f->new_width, f->new_height);

      adjust_frame_size (f, FRAME_PIXEL_TO_TEXT_WIDTH (f, width),
			 FRAME_PIXEL_TO_TEXT_HEIGHT (f, height),
			 5, 0, Qxg_frame_set_char_size);
    }
}

/** Move and resize the outer window of frame F.  WIDTH and HEIGHT are
    the new native pixel sizes of F.  Only child frames can be moved
    under GTK 4.  */

void
xg_frame_set_size_and_position (struct frame *f, int width, int height)
{
  xg_wm_set_size_hint (f, 0, 0);
  xg_request_frame_size (f, width, height);

  if (FRAME_PARENT_FRAME (f))
    {
      struct frame *p = FRAME_PARENT_FRAME (f);
      GtkWidget *fixed = FRAME_GTK_WIDGET (f);

      if (gtk_widget_get_parent (fixed) == FRAME_GTK_WIDGET (p))
	emacs_fixed_move (FRAME_GTK_WIDGET (p), fixed,
			  PGTK_TO_LOGICAL (p, f->left_pos),
			  PGTK_TO_LOGICAL (p, f->top_pos));
    }

  SET_FRAME_GARBAGED (f);
  cancel_mouse_face (f);

  if (!FRAME_VISIBLE_P (f) || FRAME_PARENT_FRAME (f))
    adjust_frame_size (f, FRAME_PIXEL_TO_TEXT_WIDTH (f, width),
		       FRAME_PIXEL_TO_TEXT_HEIGHT (f, height),
		       5, 0, Qxg_frame_set_char_size);
}

/* Set the background of widget W to PIXEL.  */

static void
xg_set_widget_bg (struct frame *f, GtkWidget *w, unsigned long pixel)
{
  GtkCssProvider *provider;
  char buf[sizeof "* { background-color: #rrggbb; }"];

  provider = g_object_get_data (G_OBJECT (w), "emacs-bg-provider");
  if (!provider)
    {
      provider = gtk_css_provider_new ();
      gtk_style_context_add_provider (gtk_widget_get_style_context (w),
				      GTK_STYLE_PROVIDER (provider),
				      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      g_object_set_data_full (G_OBJECT (w), "emacs-bg-provider",
			      provider, g_object_unref);
    }

  sprintf (buf, "* { background-color: #%06x; }",
	   (unsigned int) (pixel & 0xffffff));
  gtk_css_provider_load_from_string (provider, buf);
}

/* Apply dark mode preference to GTK window decorations.  */

#ifdef HAVE_GSETTINGS
void
xg_set_gtk_theme_dark_mode (bool dark_mode_p, GtkSettings *settings)
{
  g_object_set (settings, "gtk-application-prefer-dark-theme",
                dark_mode_p ? TRUE : FALSE, NULL);
}

/* Update all frames' dark mode based on system setting.  */

void
xg_update_dark_mode_for_all_displays (bool dark_mode_p)
{
   struct pgtk_display_info *dpyinfo;
   for (dpyinfo = x_display_list; dpyinfo; dpyinfo = dpyinfo->next)
     xg_set_gtk_theme_dark_mode (dark_mode_p,
				 gtk_settings_get_for_display (dpyinfo->gdpy));

   {
     struct input_event inev;
     EVENT_INIT (inev);
     inev.kind = TOOLKIT_THEME_CHANGED_EVENT;
     inev.arg = dark_mode_p ? Qdark : Qlight;
     kbd_buffer_store_event (&inev);
   }
}

/* Set initial dark mode for a new frame (called during frame
   creation).  */

void
xg_set_initial_dark_mode (struct frame *f)
{
  bool dark_mode_p = xg_get_system_dark_mode ();
  xg_update_dark_mode_for_all_displays (dark_mode_p);
}
#endif	/* HAVE_GSETTINGS */

/* Callback called when the gtk theme changes.
   We notify lisp code so it can fix faces used for region for example.  */

static void
style_changed_cb (GObject *go,
                  GParamSpec *spec,
                  gpointer user_data)
{
  struct input_event event;
  GdkDisplay *gdpy = user_data;
  const char *display_name = gdk_display_get_name (gdpy);
  Lisp_Object rest, frame;

  EVENT_INIT (event);
  event.kind = CONFIG_CHANGED_EVENT;
  event.frame_or_window = build_string (display_name);
  /* Theme doesn't change often, so intern is called seldom.  */
  event.arg = Qtheme_name;
  kbd_buffer_store_event (&event);

  update_theme_scrollbar_width ();
  update_theme_scrollbar_height ();

  /* If scroll bar width changed, we need set the new size on all frames
     on this display.  */
  FOR_EACH_FRAME (rest, frame)
    {
      struct frame *f = XFRAME (frame);

      if (FRAME_LIVE_P (f)
	  && FRAME_PGTK_P (f)
	  && FRAME_X_DISPLAY (f) == gdpy)
	{
	  FRAME_TERMINAL (f)->set_scroll_bar_default_width_hook (f);
	  FRAME_TERMINAL (f)->set_scroll_bar_default_height_hook (f);
	  xg_frame_set_char_size (f, FRAME_PIXEL_WIDTH (f),
				  FRAME_PIXEL_HEIGHT (f));
	}
    }
}

/* Connect style_changed_cb to the settings of DISPLAY, unless that has
   already been done.  */

static void
xg_watch_style (GdkDisplay *display)
{
  GtkSettings *gs = gtk_settings_get_for_display (display);

  if (! g_signal_handler_find (G_OBJECT (gs),
			       G_SIGNAL_MATCH_FUNC,
			       0, 0, 0,
			       (gpointer) G_CALLBACK (style_changed_cb),
			       0))
    g_signal_connect (G_OBJECT (gs), "notify::gtk-theme-name",
		      G_CALLBACK (style_changed_cb), display);
}

/* Create the toplevel window, and the boxes inside it, for frame F.
   The edit widget must be packed into the horizontal box by the
   caller.  */

static void
xg_create_toplevel_widgets (struct frame *f)
{
  GtkWidget *wtop, *wvbox, *whbox;
  char *title = 0;

  wtop = gtk_window_new ();
  wvbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  whbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_vexpand (whbox, TRUE);

  /* Use same names as the Xt port does.  I.e. Emacs.pane.emacs by
     default.  */
  gtk_widget_set_name (wtop, EMACS_CLASS);
  gtk_widget_set_name (wvbox, "pane");

  /* If this frame has a title or name, set it in the title bar.  */
  if (! NILP (f->title))
    title = SSDATA (ENCODE_UTF_8 (f->title));
  else if (! NILP (f->name))
    title = SSDATA (ENCODE_UTF_8 (f->name));

  if (title)
    gtk_window_set_title (GTK_WINDOW (wtop), title);

  if (FRAME_UNDECORATED (f))
    {
      gtk_window_set_decorated (GTK_WINDOW (wtop), FALSE);
      store_frame_param (f, Qundecorated, Qt);
    }

  gtk_window_set_icon_name (GTK_WINDOW (wtop), "emacs");

  /* Emacs binds F10 itself, to `menu-bar-open'.  */
  gtk_window_set_handle_menubar_accel (GTK_WINDOW (wtop), FALSE);
  gtk_window_set_resizable (GTK_WINDOW (wtop), TRUE);

  FRAME_GTK_OUTER_WIDGET (f) = wtop;
  f->output_data.pgtk->vbox_widget = wvbox;
  f->output_data.pgtk->hbox_widget = whbox;

  gtk_window_set_child (GTK_WINDOW (wtop), wvbox);
  gtk_box_append (GTK_BOX (wvbox), whbox);

  /* GTK 4 cannot position toplevel windows.  */
  f->win_gravity = NorthWestGravity;

  /* Steal a tool tip window we can move ourselves.  */
  f->output_data.pgtk->ttip_widget = 0;
  f->output_data.pgtk->ttip_lbl = 0;
  f->output_data.pgtk->ttip_window = 0;

  xg_watch_style (gtk_widget_get_display (wtop));
}

/* Create and set up the GTK widgets for frame F.
   Return true if creation succeeded.  */

bool
xg_create_frame_widgets (struct frame *f)
{
  GtkWidget *wfixed;

  block_input ();

  wfixed = emacs_fixed_new (f);
  gtk_widget_set_name (wfixed, SSDATA (Vx_resource_name));
  FRAME_GTK_WIDGET (f) = wfixed;

  xg_create_toplevel_widgets (f);
  gtk_box_append (GTK_BOX (f->output_data.pgtk->hbox_widget), wfixed);

  if (FRAME_EXTERNAL_TOOL_BAR (f))
    update_frame_tool_bar (f);

  /* Since GTK clears its window by filling with the background color,
     we must keep the Emacs and GTK background in sync.  */
  xg_set_widget_bg (f, wfixed, FRAME_BACKGROUND_PIXEL (f));
  gtk_window_set_focus (GTK_WINDOW (FRAME_GTK_OUTER_WIDGET (f)), wfixed);

#ifdef HAVE_GSETTINGS
  xg_set_initial_dark_mode (f);
#endif

  unblock_input ();
  return true;
}

/* Create the outer widgets of frame F, a child frame that is becoming
   a toplevel frame.  */

void
xg_create_frame_outer_widgets (struct frame *f)
{
  block_input ();
  xg_create_toplevel_widgets (f);

  if (FRAME_EXTERNAL_TOOL_BAR (f))
    update_frame_tool_bar (f);

#ifdef HAVE_GSETTINGS
  xg_set_initial_dark_mode (f);
#endif
  unblock_input ();
}

void
xg_free_frame_widgets (struct frame *f)
{
  if (FRAME_GTK_OUTER_WIDGET (f))
    {
      struct pgtk_output *x = f->output_data.pgtk;
      struct xg_frame_tb_info *tbinfo
        = g_object_get_data (G_OBJECT (FRAME_GTK_OUTER_WIDGET (f)),
                             TB_INFO_KEY);
      if (tbinfo)
        xfree (tbinfo);

      /* We hold a reference to the tool bar; see xg_create_tool_bar.  */
      g_clear_object (&x->toolbar_widget);
      x->toolbar_is_packed = false;

      gtk_window_destroy (GTK_WINDOW (FRAME_GTK_OUTER_WIDGET (f)));
      FRAME_GTK_OUTER_WIDGET (f) = 0;
    }
}

/* Set the normal size hints for the window manager, for frame F.
   GTK 4 only lets us set the minimum size of the frame, which is done
   through the minimum size of the edit widget.  FLAGS and
   USER_POSITION are ignored.  */

void
xg_wm_set_size_hint (struct frame *f, long int flags, bool user_position)
{
  double scale = FRAME_GTK_SCALE (f);
  int min_width, min_height;

  /* Don't set size hints during initialization; that apparently leads
     to a race condition.  See the thread at
     https://lists.gnu.org/r/emacs-devel/2008-10/msg00033.html  */
  if (NILP (Vafter_init_time)
      || !FRAME_GTK_WIDGET (f)
      || FRAME_PARENT_FRAME (f))
    return;

  /* Use one row/col here so the minimum size does not become zero.  */
  min_width = ceil (FRAME_TEXT_COLS_TO_PIXEL_WIDTH (f, 1) / scale);
  min_height = ceil (FRAME_TEXT_LINES_TO_PIXEL_HEIGHT (f, 1) / scale);

  if (min_width != f->output_data.pgtk->size_hints.min_width
      || min_height != f->output_data.pgtk->size_hints.min_height)
    {
      f->output_data.pgtk->size_hints.min_width = min_width;
      f->output_data.pgtk->size_hints.min_height = min_height;
      gtk_widget_queue_resize (FRAME_GTK_WIDGET (f));
    }
}

/* Change background color of a frame.
   Since GTK uses the background color to clear the window, we must
   keep the GTK and Emacs colors in sync.
   F is the frame to change,
   BG is the pixel value to change to.  */

void
xg_set_background_color (struct frame *f, unsigned long bg)
{
  if (FRAME_GTK_WIDGET (f))
    {
      block_input ();
      xg_set_widget_bg (f, FRAME_GTK_WIDGET (f), FRAME_BACKGROUND_PIXEL (f));
      unblock_input ();
    }
}

/* Change the frame's decoration (title bar + resize borders).  This
   might not work with all window managers.  */

void
xg_set_undecorated (struct frame *f, Lisp_Object undecorated)
{
  if (!FRAME_GTK_OUTER_WIDGET (f))
    return;

  block_input ();
  gtk_window_set_decorated (GTK_WINDOW (FRAME_GTK_OUTER_WIDGET (f)),
			    NILP (undecorated) ? TRUE : FALSE);
  unblock_input ();
}

/* Restack F1 below F2, above if ABOVE_FLAG is true.  Only child
   frames of the same parent can be restacked under GTK 4.  */

void
xg_frame_restack (struct frame *f1, struct frame *f2, bool above_flag)
{
  GtkWidget *w1 = FRAME_GTK_WIDGET (f1);
  GtkWidget *w2 = FRAME_GTK_WIDGET (f2);
  GtkWidget *parent;

  block_input ();
  if (!FRAME_GTK_OUTER_WIDGET (f1) && !FRAME_GTK_OUTER_WIDGET (f2)
      && (parent = gtk_widget_get_parent (w1))
      && parent == gtk_widget_get_parent (w2))
    {
      /* Children of an EmacsFixed are drawn in order, so the later a
	 child, the higher it is in the stacking order.  */
      if (above_flag)
	gtk_widget_insert_after (w1, parent, w2);
      else
	gtk_widget_insert_before (w1, parent, w2);
    }
  unblock_input ();
}

/* Don't show frame in taskbar, don't ALT-TAB to it.  Not supported by
   GTK 4.  */

void
xg_set_skip_taskbar (struct frame *f, Lisp_Object skip_taskbar)
{
}

/* Don't give frame focus when it is mapped.  Not supported by GTK
   4.  */

void
xg_set_no_focus_on_map (struct frame *f, Lisp_Object no_focus_on_map)
{
}

void
xg_set_no_accept_focus (struct frame *f, Lisp_Object no_accept_focus)
{
  /* Only child frames, which are ordinary widgets, can refuse the
     focus.  */
  if (!FRAME_GTK_OUTER_WIDGET (f) && FRAME_GTK_WIDGET (f))
    gtk_widget_set_focusable (FRAME_GTK_WIDGET (f),
			      NILP (no_accept_focus));
}

/* Override redirect windows do not exist under GTK 4.  */

void
xg_set_override_redirect (struct frame *f, Lisp_Object override_redirect)
{
}

/* Set the icon of frame F from `icon-name'.  */

void
xg_set_frame_icon_name (struct frame *f)
{
  if (FRAME_GTK_OUTER_WIDGET (f))
    gtk_window_set_icon_name (GTK_WINDOW (FRAME_GTK_OUTER_WIDGET (f)),
			      "emacs");
}


/***********************************************************************
                      Dialog functions
 ***********************************************************************/

/* State for running the main loop while a dialog is shown.  */

struct xg_dialog_data
{
  /* Set to true when the dialog has been popped down.  */
  bool *done;

  /* The dialog window, destroyed on exit if non-NULL.  */
  GtkWidget *w;

  /* A GLib timer used to run Emacs timers, or 0.  */
  guint timerid;
};

/* If there are any Emacs timers pending, add a timeout to the main
   loop for the dialog in DATA.  Pass DATA as gpointer so we can use
   this as a callback.  */

static gboolean
xg_maybe_add_timer (gpointer data)
{
  struct xg_dialog_data *dd = data;
  struct timespec next_time = timer_check ();

  dd->timerid = 0;

  if (timespec_valid_p (next_time))
    {
      time_t s = next_time.tv_sec;
      int per_ms = TIMESPEC_HZ / 1000;
      int ms = (next_time.tv_nsec + per_ms - 1) / per_ms;
      if (s <= ((guint) -1 - ms) / 1000)
	dd->timerid = g_timeout_add (s * 1000 + ms, xg_maybe_add_timer, dd);
    }
  return FALSE;
}

static void
xg_pop_down_dialog (void *arg)
{
  struct xg_dialog_data *dd = arg;

  block_input ();
  if (dd->w)
    gtk_window_destroy (GTK_WINDOW (dd->w));
  if (dd->timerid != 0)
    g_source_remove (dd->timerid);
  unblock_input ();
}

/* Run the GLib main loop until *DONE becomes true, running Emacs
   timers meanwhile.  If W is non-NULL, it is a window destroyed if
   this is exited non-locally.  */

static void
xg_run_dialog_loop (bool *done, GtkWidget *w)
{
  specpdl_ref count = SPECPDL_INDEX ();
  struct xg_dialog_data dd;

  dd.done = done;
  dd.w = w;
  dd.timerid = 0;

  record_unwind_protect_ptr (xg_pop_down_dialog, &dd);
  xg_maybe_add_timer (&dd);

  while (!*done)
    g_main_context_iteration (NULL, TRUE);

  dd.w = NULL;
  unbind_to (count, Qnil);
}

/* Process GLib events until *DONE becomes true, or TIMEOUT seconds
   have elapsed if TIMEOUT is not negative.  */

static gboolean
xg_wait_timeout (gpointer data)
{
  *(bool *) data = true;
  return G_SOURCE_REMOVE;
}

void
pgtk_wait_for_main_loop (bool *done, double timeout)
{
  bool timed_out = false;
  guint src = 0;

  if (timeout >= 0)
    src = g_timeout_add (timeout * 1000, xg_wait_timeout, &timed_out);

  while (!*done && !timed_out)
    g_main_context_iteration (NULL, TRUE);

  if (src && !timed_out)
    g_source_remove (src);
}

/* Return the toplevel window that dialogs for frame F should be
   transient for.  */

static GtkWindow *
xg_frame_dialog_parent (struct frame *f)
{
  while (FRAME_PARENT_FRAME (f))
    f = FRAME_PARENT_FRAME (f);

  return (FRAME_GTK_OUTER_WIDGET (f)
	  ? GTK_WINDOW (FRAME_GTK_OUTER_WIDGET (f)) : NULL);
}

/* Return the dialog title to use for a dialog of type KEY.
   This is the encoding used by lwlib.  We use the same for GTK.  */

static const char *
get_dialog_title (char key)
{
  const char *title = "";

  switch (key) {
  case 'E': case 'e':
    title = "Error";
    break;

  case 'I': case 'i':
    title = "Information";
    break;

  case 'L': case 'l':
    title = "Prompt";
    break;

  case 'P': case 'p':
    title = "Prompt";
    break;

  case 'Q': case 'q':
    title = "Question";
    break;
  }

  return title;
}

static gboolean
dialog_close_request (GtkWindow *window, gpointer user_data)
{
  GCallback deactivate_cb = user_data;

  if (deactivate_cb)
    ((void (*) (GtkWidget *, gpointer)) deactivate_cb) (GTK_WIDGET (window),
							NULL);
  /* The caller destroys the dialog.  */
  gtk_widget_set_visible (GTK_WIDGET (window), FALSE);
  return TRUE;
}

/* Make Escape and C-g close the window W.  */

static void
xg_add_close_shortcuts (GtkWidget *w, const char *action)
{
  GtkEventController *controller = gtk_shortcut_controller_new ();

  gtk_shortcut_controller_add_shortcut
    (GTK_SHORTCUT_CONTROLLER (controller),
     gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
		       gtk_named_action_new (action)));
  gtk_shortcut_controller_add_shortcut
    (GTK_SHORTCUT_CONTROLLER (controller),
     gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_g, GDK_CONTROL_MASK),
		       gtk_named_action_new (action)));
  gtk_widget_add_controller (w, controller);
}

/* Create a popup dialog window.  See also xg_create_widget below.
   WV is a widget_value describing the dialog.
   SELECT_CB is the callback to use when a button has been pressed.
   DEACTIVATE_CB is the callback to use when the dialog pops down.
   Returns the GTK dialog widget.  */

static GtkWidget *
create_dialog (widget_value *wv,
               GCallback select_cb,
               GCallback deactivate_cb)
{
  const char *title = get_dialog_title (wv->name[0]);
  int total_buttons = wv->name[1] - '0';
  int right_buttons = wv->name[4] - '0';
  int left_buttons;
  int button_nr = 0;
  GtkWidget *wdialog = gtk_window_new ();
  GtkWidget *wvbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  GtkWidget *wbuttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *wspacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  widget_value *item;

  if (right_buttons == 0) right_buttons = total_buttons/2;
  left_buttons = total_buttons - right_buttons;

  gtk_window_set_title (GTK_WINDOW (wdialog), title);
  gtk_widget_set_name (wdialog, "emacs-dialog");
  gtk_window_set_resizable (GTK_WINDOW (wdialog), FALSE);
  gtk_widget_add_css_class (wdialog, "dialog");
  gtk_widget_add_css_class (wdialog, "message");

  gtk_widget_set_margin_start (wvbox, 18);
  gtk_widget_set_margin_end (wvbox, 18);
  gtk_widget_set_margin_top (wvbox, 18);
  gtk_widget_set_margin_bottom (wvbox, 18);
  gtk_window_set_child (GTK_WINDOW (wdialog), wvbox);
  gtk_widget_set_hexpand (wspacer, TRUE);

  g_signal_connect (G_OBJECT (wdialog), "close-request",
		    G_CALLBACK (dialog_close_request), deactivate_cb);
  xg_add_close_shortcuts (wdialog, "window.close");

  for (item = wv->contents; item; item = item->next)
    {
      char *utf8_label = get_utf8_string (item->value);
      GtkWidget *w;

      if (item->name && strcmp (item->name, "message") == 0)
        {
          /* This is the text part of the dialog.  */
          w = gtk_label_new (utf8_label);
	  gtk_label_set_wrap (GTK_LABEL (w), TRUE);
	  gtk_label_set_max_width_chars (GTK_LABEL (w), 60);
	  gtk_label_set_xalign (GTK_LABEL (w), 0.0);
          gtk_box_prepend (GTK_BOX (wvbox), w);
        }
      else
        {
          /* This is one button to add to the dialog.  */
          w = gtk_button_new_with_label (utf8_label ? utf8_label : "");
          if (! item->enabled)
            gtk_widget_set_sensitive (w, FALSE);
          if (select_cb)
            g_signal_connect (G_OBJECT (w), "clicked",
                              select_cb, item->call_data);

	  if (button_nr == left_buttons)
	    gtk_box_append (GTK_BOX (wbuttons), wspacer);
	  gtk_box_append (GTK_BOX (wbuttons), w);
	  button_nr++;
        }

     if (utf8_label)
       g_free (utf8_label);
    }

  if (!gtk_widget_get_parent (wspacer))
    gtk_box_append (GTK_BOX (wbuttons), wspacer);
  gtk_box_append (GTK_BOX (wvbox), wbuttons);
  return wdialog;
}


/* Data shared between an asynchronous GTK dialog and the function
   waiting for it to finish.  It is allocated on the heap, because the
   dialog can outlive the waiting function if that is exited
   non-locally.  */

struct xg_async_dialog
{
  /* Set once the dialog has finished.  */
  bool done;

  /* Set if the function that waited for the dialog is gone.  */
  bool abandoned;

  /* What kind of dialog this is.  */
  int kind;

  /* Cancellable used to cancel an abandoned dialog.  */
  GCancellable *cancellable;

  /* The result of the dialog.  */
  gpointer result;
};

static struct xg_async_dialog *
xg_make_async_dialog (int kind)
{
  struct xg_async_dialog *ad = xzalloc (sizeof *ad);

  ad->kind = kind;
  ad->cancellable = g_cancellable_new ();
  return ad;
}

static void
xg_async_dialog_unwind (void *data)
{
  struct xg_async_dialog *ad = data;

  if (ad->done)
    {
      g_object_unref (ad->cancellable);
      xfree (ad);
    }
  else
    {
      /* The callback frees AD.  */
      ad->abandoned = true;
      g_cancellable_cancel (ad->cancellable);
    }
}

/* Mark AD as done.  Return true if nobody is waiting for it any
   longer, in which case AD has been freed and the result of the dialog
   must be discarded by the caller.  */

static bool
xg_async_dialog_finish (struct xg_async_dialog *ad)
{
  ad->done = true;

  if (ad->abandoned)
    {
      g_object_unref (ad->cancellable);
      xfree (ad);
      return true;
    }

  return false;
}

static void
xg_unref_object (void *object)
{
  g_object_unref (object);
}


/***********************************************************************
                      File dialog functions
 ***********************************************************************/

/* Return true if the old file selection dialog is being used.  */

bool
xg_uses_old_file_dialog (void)
{
  return false;
}

enum { XG_FILE_OPEN, XG_FILE_SAVE, XG_FILE_FOLDER };

static void
xg_file_dialog_cb (GObject *source, GAsyncResult *result, gpointer data)
{
  struct xg_async_dialog *ad = data;
  GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
  GError *error = NULL;
  GFile *file;

  switch (ad->kind)
    {
    case XG_FILE_OPEN:
      file = gtk_file_dialog_open_finish (dialog, result, &error);
      break;
    case XG_FILE_SAVE:
      file = gtk_file_dialog_save_finish (dialog, result, &error);
      break;
    default:
      file = gtk_file_dialog_select_folder_finish (dialog, result, &error);
      break;
    }

  g_clear_error (&error);

  if (xg_async_dialog_finish (ad))
    g_clear_object (&file);
  else
    ad->result = file;
}

/* Read a file name from the user, using the portal-aware GTK 4 file
   dialog.  F is the current frame.
   PROMPT is a prompt to show to the user.  May not be NULL.
   DEFAULT_FILENAME is a default selection to be displayed.  May be NULL.
   If MUSTMATCH_P, the returned file name must be an existing
   file.  (Actually, this only has cosmetic effects, the user can
   still enter a non-existing file.)  *FUNC is set to a function that
   can be used to retrieve the selected file name from the returned widget.
   If ONLY_DIR_P is non-zero, the user can only select directories.

   Returns a file name or NULL if no file was selected.
   The returned string must be freed by the caller.  */

char *
xg_get_file_name (struct frame *f,
                  char *prompt,
                  char *default_filename,
                  bool mustmatch_p,
                  bool only_dir_p)
{
  GtkFileDialog *dialog;
  struct xg_async_dialog *ad;
  specpdl_ref count = SPECPDL_INDEX ();
  char *fn = NULL;
  GFile *file;

  if (!FRAME_GTK_OUTER_WIDGET (f))
    error ("Can't open dialog from child frames");

  dialog = gtk_file_dialog_new ();
  record_unwind_protect_ptr (xg_unref_object, dialog);
  ad = xg_make_async_dialog (only_dir_p ? XG_FILE_FOLDER
			     : mustmatch_p ? XG_FILE_OPEN : XG_FILE_SAVE);
  record_unwind_protect_ptr (xg_async_dialog_unwind, ad);

  gtk_file_dialog_set_title (dialog, prompt);
  gtk_file_dialog_set_modal (dialog, TRUE);

  if (default_filename)
    {
      Lisp_Object file = build_string (default_filename);
      GFile *gfile;

      /* The file dialog does not understand ~/... in the file name.
         It must be an absolute name starting with /.  */
      if (default_filename[0] != '/')
        file = Fexpand_file_name (file, Qnil);

      gfile = g_file_new_for_path (SSDATA (ENCODE_FILE (file)));

      if (! NILP (Ffile_directory_p (file)))
	gtk_file_dialog_set_initial_folder (dialog, gfile);
      else if (ad->kind == XG_FILE_SAVE)
	{
	  GFile *parent = g_file_get_parent (gfile);
	  char *name = g_file_get_basename (gfile);

	  if (parent)
	    {
	      gtk_file_dialog_set_initial_folder (dialog, parent);
	      g_object_unref (parent);
	    }
	  gtk_file_dialog_set_initial_name (dialog, name);
	  g_free (name);
	}
      else
	gtk_file_dialog_set_initial_file (dialog, gfile);

      g_object_unref (gfile);
    }

  switch (ad->kind)
    {
    case XG_FILE_OPEN:
      gtk_file_dialog_open (dialog, xg_frame_dialog_parent (f),
			    ad->cancellable, xg_file_dialog_cb, ad);
      break;
    case XG_FILE_SAVE:
      gtk_file_dialog_save (dialog, xg_frame_dialog_parent (f),
			    ad->cancellable, xg_file_dialog_cb, ad);
      break;
    default:
      gtk_file_dialog_select_folder (dialog, xg_frame_dialog_parent (f),
				     ad->cancellable, xg_file_dialog_cb, ad);
      break;
    }

  xg_run_dialog_loop (&ad->done, NULL);

  file = ad->result;
  if (file)
    {
      char *path = g_file_get_path (file);

      if (path)
	{
	  fn = xstrdup (path);
	  g_free (path);
	}
      g_object_unref (file);
    }

  unbind_to (count, Qnil);
  return fn;
}


/***********************************************************************
                      GTK font chooser
 ***********************************************************************/

#ifdef HAVE_FREETYPE

static
Lisp_Object xg_weight_to_symbol (PangoWeight w)
{
  return
    (w <= PANGO_WEIGHT_THIN ? Qthin                  /* 100 */
     : w <= PANGO_WEIGHT_ULTRALIGHT ? Qultra_light   /* 200 */
     : w <= PANGO_WEIGHT_LIGHT ? Qlight              /* 300 */
     : w <= PANGO_WEIGHT_SEMILIGHT ? Qsemi_light     /* 350 */
     : w <= PANGO_WEIGHT_BOOK ? Qbook                /* 380 */
     : w <= PANGO_WEIGHT_NORMAL ? Qnormal            /* 400 */
     : w <= PANGO_WEIGHT_MEDIUM ? Qmedium            /* 500 */
     : w <= PANGO_WEIGHT_SEMIBOLD ? Qsemi_bold       /* 600 */
     : w <= PANGO_WEIGHT_BOLD ? Qbold                /* 700 */
     : w <= PANGO_WEIGHT_ULTRABOLD ? Qultra_bold     /* 800 */
     : w <= PANGO_WEIGHT_HEAVY ? Qblack              /* 900 */
     : Qultra_heavy);                                /* 1000 */
}

static
Lisp_Object xg_style_to_symbol (PangoStyle s)
{
  return
    (s == PANGO_STYLE_OBLIQUE ? Qoblique
     : s == PANGO_STYLE_ITALIC ? Qitalic
     : Qnormal);
}

static char *x_last_font_name;

static gboolean
xg_font_filter (gpointer item, gpointer data)
{
  const char *name;

  if (PANGO_IS_FONT_FAMILY (item))
    name = pango_font_family_get_name (PANGO_FONT_FAMILY (item));
  else if (PANGO_IS_FONT_FACE (item))
    name = pango_font_family_get_name
      (pango_font_face_get_family (PANGO_FONT_FACE (item)));
  else
    return TRUE;

  return !font_is_ignored (name, strlen (name));
}

static void
xg_font_dialog_cb (GObject *source, GAsyncResult *result, gpointer data)
{
  struct xg_async_dialog *ad = data;
  GError *error = NULL;
  PangoFontDescription *desc;

  desc = gtk_font_dialog_choose_font_finish (GTK_FONT_DIALOG (source),
					     result, &error);
  g_clear_error (&error);

  if (xg_async_dialog_finish (ad))
    {
      if (desc)
	pango_font_description_free (desc);
    }
  else
    ad->result = desc;
}

/* Pop up a GTK font selector and return the font the user selects,
   as a font spec.  DEFAULT_NAME, if non-null, is the default font
   name; it might be updated in place.  */

Lisp_Object
xg_get_font (struct frame *f, char *default_name)
{
  GtkFontDialog *dialog;
  GtkFilter *filter;
  struct xg_async_dialog *ad;
  PangoFontDescription *initial = NULL;
  Lisp_Object font = Qnil;
  specpdl_ref count = SPECPDL_INDEX ();

  if (!FRAME_GTK_OUTER_WIDGET (f))
    error ("Can't open dialog from child frames");

  dialog = gtk_font_dialog_new ();
  record_unwind_protect_ptr (xg_unref_object, dialog);
  ad = xg_make_async_dialog (0);
  record_unwind_protect_ptr (xg_async_dialog_unwind, ad);

  gtk_font_dialog_set_title (dialog, "Pick a font");
  gtk_font_dialog_set_modal (dialog, TRUE);
  filter = GTK_FILTER (gtk_custom_filter_new (xg_font_filter, NULL, NULL));
  gtk_font_dialog_set_filter (dialog, filter);
  g_object_unref (filter);

  if (default_name)
    {
      /* Convert fontconfig names to Gtk names, i.e. remove - before
	 number */
      char *p = strrchr (default_name, '-');
      if (p)
        {
          char *ep = p+1;
          while (c_isdigit (*ep))
            ++ep;
          if (*ep == '\0') *p = ' ';
        }
    }
  else if (x_last_font_name)
    default_name = x_last_font_name;

  if (default_name)
    initial = pango_font_description_from_string (default_name);

  gtk_font_dialog_choose_font (dialog, xg_frame_dialog_parent (f), initial,
			       ad->cancellable, xg_font_dialog_cb, ad);
  if (initial)
    pango_font_description_free (initial);

  xg_run_dialog_loop (&ad->done, NULL);

  if (ad->result)
    {
      PangoFontDescription *desc = ad->result;
      const char *family = pango_font_description_get_family (desc);
      gint        size   = pango_font_description_get_size (desc);
      PangoWeight weight = pango_font_description_get_weight (desc);
      PangoStyle  style  = pango_font_description_get_style (desc);
      char *font_desc_str;

      font = CALLN (Ffont_spec,
		    QCfamily, build_string (family),
		    QCsize, make_float (pango_units_to_double (size)),
		    QCweight, xg_weight_to_symbol (weight),
		    QCslant, xg_style_to_symbol (style));

      font_desc_str = pango_font_description_to_string (desc);
      dupstring (&x_last_font_name, font_desc_str);
      g_free (font_desc_str);
      pango_font_description_free (desc);
    }

  unbind_to (count, Qnil);
  return font;
}

#endif /* HAVE_FREETYPE */


/***********************************************************************
	                Menu functions.
 ***********************************************************************/

/* Menus are described to GTK with a GMenuModel.  Each selectable item
   is bound to a GSimpleAction in a group inserted into the widget
   hierarchy with the prefix "emacs"; activating the action runs the
   select callback with the item's xg_menu_item_cb_data.  The group is
   kept in the xg_menu_cb_data of the menu.  */

/* Linked list of all allocated struct xg_menu_cb_data.  Used for marking
   during GC.  The next member points to the items.  */
static xg_list_node xg_menu_cb_list;

/* Linked list of all allocated struct xg_menu_item_cb_data.  Used for marking
   during GC.  The next member points to the items.  */
static xg_list_node xg_menu_item_cb_list;

/* Allocate and initialize CL_DATA if NULL, otherwise increase ref_count.
   F is the frame CL_DATA will be initialized for.
   SELECT_CB is the callback to call when a menu item is selected.
   HIGHLIGHT_CB is the callback to call when entering/leaving menu items.

   Returns CL_DATA if CL_DATA is not NULL,  or a pointer to a newly
   allocated xg_menu_cb_data if CL_DATA is NULL.  */

static xg_menu_cb_data *
make_cl_data (xg_menu_cb_data *cl_data, struct frame *f,
	      GCallback select_cb, GCallback highlight_cb)
{
  if (! cl_data)
    {
      cl_data = xzalloc (sizeof *cl_data);
      cl_data->f = f;
      cl_data->menu_bar_vector = f->menu_bar_vector;
      cl_data->menu_bar_items_used = f->menu_bar_items_used;
      cl_data->highlight_cb = highlight_cb;
      cl_data->select_cb = select_cb;
      cl_data->actions = g_simple_action_group_new ();
      cl_data->ref_count = 0;

      xg_list_insert (&xg_menu_cb_list, &cl_data->ptrs);
    }

  cl_data->ref_count++;

  return cl_data;
}

/* Update CL_DATA with values from frame F and with HIGHLIGHT_CB.
   When the menu bar is updated, menu items may have been added and/or
   removed, so menu_bar_vector and menu_bar_items_used change.  We must
   then update CL_DATA since it is used to determine which menu
   item that is invoked in the menu.  */

static void
update_cl_data (xg_menu_cb_data *cl_data,
                struct frame *f,
                GCallback highlight_cb)
{
  if (cl_data)
    {
      cl_data->f = f;
      cl_data->menu_bar_vector = f->menu_bar_vector;
      cl_data->menu_bar_items_used = f->menu_bar_items_used;
      cl_data->highlight_cb = highlight_cb;
    }
}

/* Decrease reference count for CL_DATA.
   If reference count is zero, free CL_DATA.  */

static void
unref_cl_data (gpointer data)
{
  xg_menu_cb_data *cl_data = data;

  if (cl_data && cl_data->ref_count > 0)
    {
      cl_data->ref_count--;
      if (cl_data->ref_count == 0)
        {
          xg_list_remove (&xg_menu_cb_list, &cl_data->ptrs);
	  g_clear_object (&cl_data->actions);
          xfree (cl_data);
        }
    }
}

/* Function that marks all lisp data during GC.  */

void
xg_mark_data (void)
{
  xg_list_node *iter;
  Lisp_Object rest, frame;

  for (iter = xg_menu_cb_list.next; iter; iter = iter->next)
    mark_object (((xg_menu_cb_data *) iter)->menu_bar_vector);

  for (iter = xg_menu_item_cb_list.next; iter; iter = iter->next)
    {
      xg_menu_item_cb_data *cb_data = (xg_menu_item_cb_data *) iter;

      if (! NILP (cb_data->help))
        mark_object (cb_data->help);
    }

  FOR_EACH_FRAME (rest, frame)
    {
      struct frame *f = XFRAME (frame);

      if (FRAME_PGTK_P (f) && FRAME_GTK_OUTER_WIDGET (f))
        {
          struct xg_frame_tb_info *tbinfo
            = g_object_get_data (G_OBJECT (FRAME_GTK_OUTER_WIDGET (f)),
                                 TB_INFO_KEY);
          if (tbinfo)
            {
              mark_object (tbinfo->last_tool_bar);
              mark_object (tbinfo->style);
            }
        }
    }
}

/* Free the callback data of a menu item, when its action is
   destroyed.  */

static void
xg_free_menu_item_cb_data (gpointer data)
{
  xg_menu_item_cb_data *cb_data = data;

  xg_list_remove (&xg_menu_item_cb_list, &cb_data->ptrs);
  xfree (cb_data);
}

/* Called when the action bound to a menu item is activated.  */

static void
xg_menu_item_activated (GSimpleAction *action, GVariant *parameter,
			gpointer user_data)
{
  xg_menu_item_cb_data *cb_data = user_data;
  xg_menu_cb_data *cl_data = cb_data->cl_data;
  GCallback select_cb = cl_data->select_cb;
  GtkWidget *menu = cl_data->widget;

  if (menu)
    g_object_ref (menu);

  if (select_cb)
    ((void (*) (GtkWidget *, gpointer)) select_cb) (NULL, cb_data);

  /* GTK does not close menus when toggle or radio items are
     activated, but Emacs runs a command for them as for any other
     item.  */
  if (menu && GTK_IS_POPOVER (menu))
    gtk_popover_popdown (GTK_POPOVER (menu));
  else if (menu)
    {
      GtkWidget *item, *child;

      for (item = gtk_widget_get_first_child (menu); item;
	   item = gtk_widget_get_next_sibling (item))
	for (child = gtk_widget_get_first_child (item); child;
	     child = gtk_widget_get_next_sibling (child))
	  if (GTK_IS_POPOVER (child))
	    gtk_popover_popdown (GTK_POPOVER (child));
    }

  if (menu)
    g_object_unref (menu);
}

/* Return the label for a menu item with the UTF-8 strings LABEL and
   KEY.  Set *MARKUP to whether it contains Pango markup.  Free the
   result with g_free.  */

static char *
xg_menu_item_label (const char *label, const char *key, bool *markup)
{
  char *escaped, *escaped_key, *result;

  if (!label)
    label = " ";

  /* GTK 4 menus can only show single-key accelerators in their own
     column, so show Emacs key sequences after the label instead.  */
  if (!key || !*key)
    {
      *markup = false;
      return xg_escape_mnemonics (label);
    }

  escaped = g_markup_escape_text (label, -1);
  escaped_key = g_markup_escape_text (key, -1);
  result = g_strdup_printf ("%s\t<span alpha=\"55%%\">%s</span>",
			    escaped, escaped_key);
  g_free (escaped);
  g_free (escaped_key);

  escaped = xg_escape_mnemonics (result);
  g_free (result);
  *markup = true;
  return escaped;
}

static void xg_fill_menu (GMenu *, widget_value *, struct frame *,
			  xg_menu_cb_data *);

/* Create a menu item for ITEM.  F is the frame the menu belongs to,
   and CL_DATA the callback data of the menu.  */

static GMenuItem *
xg_make_menu_item (widget_value *item, struct frame *f,
		   xg_menu_cb_data *cl_data)
{
  char *utf8_label = get_utf8_string (item->name);
  char *utf8_key = get_utf8_string (item->key);
  char *label;
  bool markup;
  GMenuItem *mitem;

  label = xg_menu_item_label (utf8_label, utf8_key, &markup);
  mitem = g_menu_item_new (label, NULL);
  g_free (label);
  g_free (utf8_label);
  g_free (utf8_key);

  if (markup)
    g_menu_item_set_attribute (mitem, "use-markup", "s", "true");

  if (item->contents)
    {
      GMenu *submenu = g_menu_new ();

      xg_fill_menu (submenu, item->contents, f, cl_data);
      g_menu_item_set_submenu (mitem, G_MENU_MODEL (submenu));
      g_object_unref (submenu);
    }
  else
    {
      char name[sizeof "i" + INT_STRLEN_BOUND (int)];
      char detailed[sizeof "emacs." + sizeof name];
      GSimpleAction *action;

      sprintf (name, "i%d", cl_data->n_actions++);
      sprintf (detailed, "emacs.%s", name);

      if (item->button_type == BUTTON_TYPE_TOGGLE)
	action = g_simple_action_new_stateful
	  (name, NULL, g_variant_new_boolean (item->selected));
      else if (item->button_type == BUTTON_TYPE_RADIO)
	action = g_simple_action_new_stateful
	  (name, G_VARIANT_TYPE_BOOLEAN, g_variant_new_boolean (item->selected));
      else
	action = g_simple_action_new (name, NULL);

      /* Items without call data, such as the titles of popup menus,
	 cannot be selected.  */
      g_simple_action_set_enabled (action, item->enabled && item->call_data);

      if (item->call_data)
	{
	  xg_menu_item_cb_data *cb_data = xmalloc (sizeof *cb_data);

	  xg_list_insert (&xg_menu_item_cb_list, &cb_data->ptrs);
	  cb_data->select_id = 0;
	  cb_data->help = item->help;
	  cb_data->cl_data = cl_data;
	  cb_data->call_data = item->call_data;

	  g_signal_connect (action, "activate",
			    G_CALLBACK (xg_menu_item_activated), cb_data);
	  g_object_set_data_full (G_OBJECT (action), XG_ITEM_DATA, cb_data,
				  xg_free_menu_item_cb_data);
	}

      g_action_map_add_action (G_ACTION_MAP (cl_data->actions),
			       G_ACTION (action));
      g_object_unref (action);

      /* GTK displays an item as a radio button if its action has a
	 state and the item has a target.  */
      if (item->button_type == BUTTON_TYPE_RADIO)
	g_menu_item_set_action_and_target_value (mitem, detailed,
						 g_variant_new_boolean (TRUE));
      else
	g_menu_item_set_detailed_action (mitem, detailed);
    }

  return mitem;
}

/* Append the items described by DATA to MENU.  Separators split the
   menu into sections.  */

static void
xg_fill_menu (GMenu *menu, widget_value *data, struct frame *f,
	      xg_menu_cb_data *cl_data)
{
  GMenu *section = g_menu_new ();
  widget_value *item;

  for (item = data; item; item = item->next)
    {
      GMenuItem *mitem;

      if (menu_separator_name_p (item->name))
	{
	  if (g_menu_model_get_n_items (G_MENU_MODEL (section)) > 0)
	    {
	      g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
	      g_object_unref (section);
	      section = g_menu_new ();
	    }
	  continue;
	}

      mitem = xg_make_menu_item (item, f, cl_data);
      g_menu_append_item (section, mitem);
      g_object_unref (mitem);
    }

  if (g_menu_model_get_n_items (G_MENU_MODEL (section)) > 0)
    g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
  g_object_unref (section);
}

/* Append the top level items of a menu bar, described by DATA, to
   MENU.  Each of them gets a submenu, which is empty if the contents
   of the menu have not been computed yet.  */

static void
xg_fill_menubar (GMenu *menu, widget_value *data, struct frame *f,
		 xg_menu_cb_data *cl_data)
{
  widget_value *item;

  for (item = data; item; item = item->next)
    {
      char *utf8_label = get_utf8_string (item->name);
      char *label = xg_escape_mnemonics (utf8_label);
      GMenu *submenu = g_menu_new ();
      GMenuItem *mitem;

      if (item->contents)
	xg_fill_menu (submenu, item->contents, f, cl_data);

      mitem = g_menu_item_new_submenu (label, G_MENU_MODEL (submenu));
      g_menu_append_item (menu, mitem);

      g_object_unref (mitem);
      g_object_unref (submenu);
      g_free (label);
      g_free (utf8_label);
    }
}

/* Return true if the top level items of MENU have the same labels as
   the widget values in DATA.  */

static bool
xg_menubar_labels_same_p (GMenuModel *menu, widget_value *data)
{
  int i, n = g_menu_model_get_n_items (menu);
  widget_value *item;

  for (i = 0, item = data; item; item = item->next, i++)
    {
      char *old, *utf8_label, *label;
      bool same;

      if (i >= n
	  || !g_menu_model_get_item_attribute (menu, i, G_MENU_ATTRIBUTE_LABEL,
					       "s", &old))
	return false;

      utf8_label = get_utf8_string (item->name);
      label = xg_escape_mnemonics (utf8_label);
      same = !strcmp (old, label);
      g_free (old);
      g_free (label);
      g_free (utf8_label);

      if (!same)
	return false;
    }

  return i == n;
}

/* Return whether a menu of MENUBAR is shown.  */

static bool
xg_menubar_popped_up_p (GtkWidget *menubar)
{
  GtkWidget *item, *child;

  for (item = gtk_widget_get_first_child (menubar); item;
       item = gtk_widget_get_next_sibling (item))
    for (child = gtk_widget_get_first_child (item); child;
	 child = gtk_widget_get_next_sibling (child))
      if (GTK_IS_POPOVER (child) && gtk_widget_get_visible (child))
	return true;

  return false;
}

/* Give the keyboard focus back to the frame of MENUBAR, passed in
   DATA, once its menus are closed.  GTK leaves it in the menu bar,
   where Emacs doesn't see key presses.  */

static gboolean
xg_menubar_refocus (gpointer data)
{
  GtkWidget *menubar = data;
  xg_menu_cb_data *cl_data = g_object_get_data (G_OBJECT (menubar),
						XG_FRAME_DATA);
  struct frame *f = cl_data ? cl_data->f : NULL;

  if (f && FRAME_LIVE_P (f) && FRAME_PGTK_P (f) && FRAME_GTK_WIDGET (f)
      && f->output_data.pgtk->menubar_widget == menubar
      && !xg_menubar_popped_up_p (menubar))
    gtk_widget_grab_focus (FRAME_GTK_WIDGET (f));

  g_object_unref (menubar);
  return G_SOURCE_REMOVE;
}

/* Callback for the "closed" signal of the menus of a menu bar.  */

static void
menubar_popover_closed (GtkPopover *popover, gpointer user_data)
{
  GCallback deactivate_cb = user_data;
  GtkWidget *menubar
    = gtk_widget_get_ancestor (GTK_WIDGET (popover),
			       GTK_TYPE_POPOVER_MENU_BAR);

  if (deactivate_cb)
    ((void (*) (GtkWidget *, gpointer)) deactivate_cb) (GTK_WIDGET (popover),
							NULL);

  /* Another menu might be opened right after this one is closed, as
     the pointer moves along the menu bar.  */
  if (menubar)
    g_idle_add (xg_menubar_refocus, g_object_ref (menubar));
}

static void
menubar_popover_shown (GtkWidget *popover, gpointer user_data)
{
  pgtk_menu_set_in_use (true);
}

/* Connect to the signals of the menus of MENUBAR that haven't been
   connected to yet.  */

static void
xg_menubar_watch_popovers (GtkWidget *menubar)
{
  GtkWidget *item, *child;
  GCallback deactivate_cb
    = g_object_get_data (G_OBJECT (menubar), "emacs-deactivate-cb");

  for (item = gtk_widget_get_first_child (menubar); item;
       item = gtk_widget_get_next_sibling (item))
    for (child = gtk_widget_get_first_child (item); child;
	 child = gtk_widget_get_next_sibling (child))
      if (GTK_IS_POPOVER (child)
	  && !g_object_get_data (G_OBJECT (child), "emacs-watched"))
	{
	  g_object_set_data (G_OBJECT (child), "emacs-watched",
			     GINT_TO_POINTER (1));
	  g_signal_connect (child, "show", G_CALLBACK (menubar_popover_shown),
			    NULL);
	  g_signal_connect (child, "closed",
			    G_CALLBACK (menubar_popover_closed),
			    deactivate_cb);
	}
}

static void
menubar_items_changed (GMenuModel *model, int position, int removed,
		       int added, gpointer user_data)
{
  xg_menubar_watch_popovers (GTK_WIDGET (user_data));
}

/* Called before the menu bar of frame F (passed in USER_DATA) handles
   a mouse click.  Compute the contents of the menus, which are only
   computed lazily.  */

static gboolean
menu_bar_pressed_cb (GtkEventControllerLegacy *controller, GdkEvent *event,
		     gpointer user_data)
{
  struct frame *f = user_data;

  if (gdk_event_get_event_type (event) == GDK_BUTTON_PRESS
      && gdk_button_event_get_button (event) < 4
      && !popup_activated ())
    set_frame_menubar (f, true);

  return GDK_EVENT_PROPAGATE;
}

static gboolean
popover_popdown_cb (GtkWidget *widget, GVariant *args, gpointer user_data)
{
  gtk_popover_popdown (GTK_POPOVER (widget));
  return TRUE;
}

/* Create a menubar, popup menu or dialog, depending on the TYPE argument.
   TYPE can be "menubar", "popup" for popup menu, or "dialog" for a dialog
   with some text and buttons.
   F is the frame the created item belongs to.
   NAME is the name to use for the top widget.
   VAL is a widget_value structure describing items to be created.
   SELECT_CB is the callback to use when a menu item is selected or
   a dialog button is pressed.
   DEACTIVATE_CB is the callback to use when an item is deactivated.
   For a menu, when a sub menu is not shown anymore, for a dialog it is
   called when the dialog is popped down.
   HIGHLIGHT_CB is the callback to call when entering/leaving menu items.

   Returns the widget created.  */

GtkWidget *
xg_create_widget (const char *type, const char *name, struct frame *f,
		  widget_value *val, GCallback select_cb,
		  GCallback deactivate_cb, GCallback highlight_cb)
{
  GtkWidget *w = 0;
  bool menu_bar_p = strcmp (type, "menubar") == 0;
  bool pop_up_p = strcmp (type, "popup") == 0;

  if (strcmp (type, "dialog") == 0)
    {
      w = create_dialog (val, select_cb, deactivate_cb);
      gtk_window_set_transient_for (GTK_WINDOW (w),
				    xg_frame_dialog_parent (f));
      gtk_window_set_destroy_with_parent (GTK_WINDOW (w), TRUE);
      gtk_window_set_modal (GTK_WINDOW (w), TRUE);
    }
  else if (menu_bar_p || pop_up_p)
    {
      xg_menu_cb_data *cl_data = make_cl_data (NULL, f, select_cb,
					       highlight_cb);
      GMenu *model = g_menu_new ();

      if (menu_bar_p)
	{
	  GtkEventController *controller;

	  xg_fill_menubar (model, val->contents, f, cl_data);
	  w = gtk_popover_menu_bar_new_from_model (G_MENU_MODEL (model));

	  /* Fill in the contents of the menus before the menu bar
	     handles a click.  */
	  controller = gtk_event_controller_legacy_new ();
	  gtk_event_controller_set_propagation_phase (controller,
						      GTK_PHASE_CAPTURE);
	  g_signal_connect (controller, "event",
			    G_CALLBACK (menu_bar_pressed_cb), f);
	  gtk_widget_add_controller (w, controller);

	  g_object_set_data (G_OBJECT (w), "emacs-deactivate-cb",
			     deactivate_cb);
	  g_signal_connect_after (model, "items-changed",
				  G_CALLBACK (menubar_items_changed), w);
	  xg_menubar_watch_popovers (w);
	  g_object_set_data_full (G_OBJECT (w), XG_MENUBAR_MODEL, model,
				  g_object_unref);
	}
      else
	{
	  GtkEventController *controller;

	  xg_fill_menu (model, val->contents, f, cl_data);
	  w = gtk_popover_menu_new_from_model_full (G_MENU_MODEL (model),
						    GTK_POPOVER_MENU_NESTED);
	  g_object_unref (model);

	  if (deactivate_cb)
	    g_signal_connect (w, "closed", deactivate_cb, NULL);

	  /* Make C-g close the menu, like Escape.  */
	  controller = gtk_shortcut_controller_new ();
	  gtk_shortcut_controller_add_shortcut
	    (GTK_SHORTCUT_CONTROLLER (controller),
	     gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_g,
						       GDK_CONTROL_MASK),
			       gtk_callback_action_new (popover_popdown_cb,
							NULL, NULL)));
	  gtk_widget_add_controller (w, controller);
	}

      gtk_widget_insert_action_group (w, "emacs",
				      G_ACTION_GROUP (cl_data->actions));
      cl_data->widget = w;
      g_object_set_data_full (G_OBJECT (w), XG_FRAME_DATA, cl_data,
			      unref_cl_data);

      if (name)
        gtk_widget_set_name (w, name);
    }
  else
    {
      fprintf (stderr, "bad type in xg_create_widget: %s, doing nothing\n",
               type);
    }

  return w;
}

/* Update the MENUBAR.
   F is the frame the menu bar belongs to.
   VAL describes the contents of the menu bar.
   If DEEP_P, rebuild all the menus, otherwise just the names in the
   menubar.  SELECT_CB is the callback to use when a menu item is
   selected.  DEACTIVATE_CB is the callback to use when a sub menu is
   not shown anymore.  HIGHLIGHT_CB is the callback to call when
   entering/leaving menu items.  */

void
xg_modify_menubar_widgets (GtkWidget *menubar, struct frame *f,
			   widget_value *val, bool deep_p,
                           GCallback select_cb, GCallback deactivate_cb,
			   GCallback highlight_cb)
{
  xg_menu_cb_data *cl_data = g_object_get_data (G_OBJECT (menubar),
						XG_FRAME_DATA);
  GMenu *model = g_object_get_data (G_OBJECT (menubar), XG_MENUBAR_MODEL);
  int i;
  widget_value *item;

  if (!cl_data || !model)
    return;

  update_cl_data (cl_data, f, highlight_cb);
  cl_data->select_cb = select_cb;
  g_object_set_data (G_OBJECT (menubar), "emacs-deactivate-cb",
		     deactivate_cb);

  if (deep_p)
    {
      /* Replace all the actions.  Insert the new group before filling
	 the menus, so that their items find the right actions.  */
      g_clear_object (&cl_data->actions);
      cl_data->actions = g_simple_action_group_new ();
      cl_data->n_actions = 0;
      gtk_widget_insert_action_group (menubar, "emacs",
				      G_ACTION_GROUP (cl_data->actions));
    }

  if (!xg_menubar_labels_same_p (G_MENU_MODEL (model), val->contents))
    {
      /* The menu bar changed.  Rebuild it from scratch.  */
      g_menu_remove_all (model);
      xg_fill_menubar (model, val->contents, f, cl_data);
    }
  else if (deep_p)
    {
      /* Update the contents of the menus, keeping the menu bar items,
	 one of which might be handling the click that caused this
	 update.  */
      for (i = 0, item = val->contents; item; item = item->next, i++)
	{
	  GMenuModel *submenu
	    = g_menu_model_get_item_link (G_MENU_MODEL (model), i,
					  G_MENU_LINK_SUBMENU);

	  if (submenu && G_IS_MENU (submenu))
	    {
	      g_menu_remove_all (G_MENU (submenu));
	      xg_fill_menu (G_MENU (submenu), item->contents, f, cl_data);
	    }

	  if (submenu)
	    g_object_unref (submenu);
	}
    }
}

/* Recompute the height of the menu bar of frame F.  */

static void
xg_update_menubar_height (struct frame *f)
{
  GtkWidget *menubar = f->output_data.pgtk->menubar_widget;
  GtkWidget *container = gtk_widget_get_parent (menubar);
  int min, nat, height;

  /* The menu bar is inside a scrolled window, see below.  */
  while (container && !GTK_IS_SCROLLED_WINDOW (container))
    container = gtk_widget_get_parent (container);

  gtk_widget_measure (container ? container : menubar,
		      GTK_ORIENTATION_VERTICAL, -1, &min, &nat, NULL, NULL);
  height = lrint (nat * FRAME_GTK_SCALE (f));

  if (FRAME_MENUBAR_HEIGHT (f) != height)
    {
      FRAME_MENUBAR_HEIGHT (f) = height;
      adjust_frame_size (f, -1, -1, 2, 0, Qmenu_bar_lines);
    }
}

/* Called when the scale of frame F changed, which changes the sizes
   of its menu bar and tool bar in device pixels.  */

static bool xg_update_tool_bar_sizes (struct frame *f);

void
xg_frame_scale_changed (struct frame *f)
{
  struct pgtk_output *x = f->output_data.pgtk;

  if (x->menubar_widget && gtk_widget_get_parent (x->menubar_widget))
    xg_update_menubar_height (f);

  if (x->toolbar_widget && x->toolbar_is_packed
      && xg_update_tool_bar_sizes (f))
    adjust_frame_size (f, -1, -1, 2, false, Qtool_bar_lines);

  xg_wm_set_size_hint (f, 0, false);
}

/* Callback called when the menu bar W is mapped.  Used to find the
   height of the menu bar.  */

static void
menubar_map_cb (GtkWidget *w, gpointer user_data)
{
  xg_update_menubar_height (user_data);
}

/* Recompute all the widgets of frame F, when the menu bar has been
   changed.  */

void
xg_update_frame_menubar (struct frame *f)
{
  struct pgtk_output *x = f->output_data.pgtk;
  GtkWidget *scrolled;

  if (!x->menubar_widget || !x->vbox_widget
      || gtk_widget_get_parent (x->menubar_widget))
    return;

  block_input ();

  /* Put the menu bar in a scrolled window without scroll bars, so that
     a long menu bar does not prevent the frame from being made
     narrower.  */
  scrolled = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
				  GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_propagate_natural_height
    (GTK_SCROLLED_WINDOW (scrolled), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled),
				 x->menubar_widget);
  gtk_box_prepend (GTK_BOX (x->vbox_widget), scrolled);

  g_signal_connect (x->menubar_widget, "map", G_CALLBACK (menubar_map_cb), f);
  xg_update_menubar_height (f);

  unblock_input ();
}

/* Get rid of the menu bar of frame F, and free its storage.
   This is used when deleting a frame, and when turning off the menu bar.  */

void
free_frame_menubar (struct frame *f)
{
  struct pgtk_output *x = f->output_data.pgtk;

  if (x->menubar_widget)
    {
      GtkWidget *container = x->menubar_widget;

      block_input ();

      while (gtk_widget_get_parent (container)
	     && gtk_widget_get_parent (container) != x->vbox_widget)
	container = gtk_widget_get_parent (container);

      if (x->vbox_widget && gtk_widget_get_parent (container))
	gtk_box_remove (GTK_BOX (x->vbox_widget), container);
      else if (g_object_is_floating (x->menubar_widget))
	g_object_ref_sink (x->menubar_widget), g_object_unref (x->menubar_widget);

      x->menubar_widget = 0;
      FRAME_MENUBAR_HEIGHT (f) = 0;
      adjust_frame_size (f, -1, -1, 2, 0, Qmenu_bar_lines);
      unblock_input ();
    }
}

/* Open the first menu of the menu bar of frame F, for
   `menu-bar-open'.  */

void
xg_menubar_open (struct frame *f)
{
  GtkWidget *menubar = f->output_data.pgtk->menubar_widget;
  GtkWidget *item;

  if (menubar && (item = gtk_widget_get_first_child (menubar)))
    gtk_widget_activate (item);
}

/* Pop up MENU, a popup menu created by xg_create_widget, on frame F.
   X and Y are the position at which to show the menu, in pixels
   relative to the edit widget of F.  */

void
xg_show_popup_menu (struct frame *f, GtkWidget *menu, int x, int y)
{
  GdkRectangle rect;

  rect.x = floor (PGTK_TO_LOGICAL (f, x));
  rect.y = floor (PGTK_TO_LOGICAL (f, y));
  rect.width = 1;
  rect.height = 1;

  gtk_widget_set_parent (menu, FRAME_GTK_WIDGET (f));
  gtk_popover_set_pointing_to (GTK_POPOVER (menu), &rect);
  gtk_popover_set_has_arrow (GTK_POPOVER (menu), FALSE);
  gtk_popover_set_position (GTK_POPOVER (menu), GTK_POS_BOTTOM);
  gtk_widget_set_halign (menu, GTK_ALIGN_START);
  gtk_popover_popup (GTK_POPOVER (menu));
}

/* Destroy MENU, a popup menu shown by xg_show_popup_menu.  */

void
xg_destroy_popup_menu (GtkWidget *menu)
{
  gtk_popover_popdown (GTK_POPOVER (menu));

  if (gtk_widget_get_parent (menu))
    gtk_widget_unparent (menu);
  else
    g_object_ref_sink (menu), g_object_unref (menu);
}


/***********************************************************************
                      Scroll bar functions
 ***********************************************************************/

/* Xlib's `Window' fits in 32 bits.  But we want to store pointers, and they
   may be larger than 32 bits.  Keep a mapping from integer index to widget
   pointers to get around the 32 bit limitation.  */

static struct
{
  GtkWidget **widgets;
  ptrdiff_t max_size;
  ptrdiff_t used;
} id_to_widget;

/* Grow this much every time we need to allocate more  */

#define ID_TO_WIDGET_INCR  32

/* Store the widget pointer W in id_to_widget and return the integer index.  */

static ptrdiff_t
xg_store_widget_in_map (GtkWidget *w)
{
  ptrdiff_t i;

  if (id_to_widget.max_size == id_to_widget.used)
    {
      ptrdiff_t new_size;
      if (TYPE_MAXIMUM (Window) - ID_TO_WIDGET_INCR < id_to_widget.max_size)
	memory_full_up ();

      new_size = id_to_widget.max_size + ID_TO_WIDGET_INCR;
      id_to_widget.widgets = xnrealloc (id_to_widget.widgets,
					new_size, sizeof (GtkWidget *));

      for (i = id_to_widget.max_size; i < new_size; ++i)
        id_to_widget.widgets[i] = 0;
      id_to_widget.max_size = new_size;
    }

  /* Just loop over the array and find a free place.  After all,
     how many scroll bars are we creating?  Should be a small number.
     The check above guarantees we will find a free place.  */
  for (i = 0; i < id_to_widget.max_size; ++i)
    {
      if (! id_to_widget.widgets[i])
        {
          id_to_widget.widgets[i] = w;
          ++id_to_widget.used;

          return i;
        }
    }

  /* Should never end up here  */
  emacs_abort ();
}

/* Remove pointer at IDX from id_to_widget.
   Called when scroll bar is destroyed.  */

static void
xg_remove_widget_from_map (ptrdiff_t idx)
{
  if (idx < id_to_widget.max_size && id_to_widget.widgets[idx] != 0)
    {
      id_to_widget.widgets[idx] = 0;
      --id_to_widget.used;
    }
}

/* Get the widget pointer at IDX from id_to_widget. */

static GtkWidget *
xg_get_widget_from_map (ptrdiff_t idx)
{
  if (idx < id_to_widget.max_size && id_to_widget.widgets[idx] != 0)
    return id_to_widget.widgets[idx];

  return 0;
}

/* Return the thickness of a scroll bar with ORIENTATION for the
   current theme, in logical pixels.  Store its minimum length in
   *MIN_LENGTH.  */

static int
xg_measure_scrollbar (GtkOrientation orientation, int *min_length)
{
  GtkAdjustment *adj = gtk_adjustment_new (XG_SB_MIN, XG_SB_MIN, XG_SB_MAX,
					   0.1, 0.1, 0.1);
  GtkWidget *wscroll = gtk_scrollbar_new (orientation, adj);
  int min, nat;

  g_object_ref_sink (wscroll);
  gtk_widget_measure (wscroll, orientation, -1, min_length, NULL,
		      NULL, NULL);
  gtk_widget_measure (wscroll,
		      (orientation == GTK_ORIENTATION_VERTICAL
		       ? GTK_ORIENTATION_HORIZONTAL
		       : GTK_ORIENTATION_VERTICAL),
		      -1, &min, &nat, NULL, NULL);
  g_object_unref (wscroll);

  return max (min, nat);
}

static void
update_theme_scrollbar_width (void)
{
  scroll_bar_width_for_theme
    = max (8, xg_measure_scrollbar (GTK_ORIENTATION_VERTICAL,
				    &scroll_bar_min_length_for_theme));
}

static void
update_theme_scrollbar_height (void)
{
  scroll_bar_height_for_theme
    = max (8, xg_measure_scrollbar (GTK_ORIENTATION_HORIZONTAL,
				    &scroll_bar_min_width_for_theme));
}

int
xg_get_default_scrollbar_width (struct frame *f)
{
  return ceil (scroll_bar_width_for_theme * FRAME_GTK_SCALE (f));
}

int
xg_get_default_scrollbar_height (struct frame *f)
{
  return ceil (scroll_bar_height_for_theme * FRAME_GTK_SCALE (f));
}

/* Callback invoked when scroll bar WIDGET is destroyed.
   DATA is the index into id_to_widget for WIDGET.  */

static void
xg_gtk_scroll_destroy (GtkWidget *widget, gpointer data)
{
  intptr_t id = (intptr_t) data;
  xg_remove_widget_from_map (id);
}

/* Called for every event delivered to a scroll bar.  Run the end
   callback of the scroll bar, which is passed in USER_DATA, when the
   mouse button is released.  */

static gboolean
xg_scroll_bar_event_cb (GtkEventControllerLegacy *controller,
			GdkEvent *event, gpointer user_data)
{
  GtkWidget *w
    = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));

  /* Remember whether a button is held down in the scroll bar, so
     that the scroll callback can tell drags from other changes.  */
  if (gdk_event_get_event_type (event) == GDK_BUTTON_PRESS)
    g_object_set_data (G_OBJECT (w), "emacs-pressed", GINT_TO_POINTER (1));
  else if (gdk_event_get_event_type (event) == GDK_BUTTON_RELEASE)
    {
      GCallback end_cb = g_object_get_data (G_OBJECT (w), "emacs-end-scroll");

      g_object_set_data (G_OBJECT (w), "emacs-pressed", NULL);
      if (end_cb)
	((void (*) (GtkWidget *, gpointer)) end_cb) (w, user_data);
    }

  return GDK_EVENT_PROPAGATE;
}

static void
xg_finish_scroll_bar_creation (struct frame *f,
                               GtkWidget *wscroll,
                               struct scroll_bar *bar,
                               GCallback scroll_callback,
                               GCallback end_callback,
                               const char *scroll_bar_name)
{
  GtkEventController *controller;
  GtkStyleContext *ctxt;
  ptrdiff_t scroll_id;

  gtk_widget_set_name (wscroll, scroll_bar_name);
  g_object_set_data (G_OBJECT (wscroll), XG_FRAME_DATA, (gpointer) f);

  scroll_id = xg_store_widget_in_map (wscroll);
  g_signal_connect (G_OBJECT (wscroll),
                    "destroy",
                    G_CALLBACK (xg_gtk_scroll_destroy),
                    (gpointer) scroll_id);

  /* GtkScrollbar is not a GtkRange under GTK 4, so scrolling has to be
     detected through its adjustment.  */
  g_signal_connect (gtk_scrollbar_get_adjustment (GTK_SCROLLBAR (wscroll)),
		    "value-changed", scroll_callback, bar);
  g_object_set_data (G_OBJECT (gtk_scrollbar_get_adjustment
			       (GTK_SCROLLBAR (wscroll))),
		     "emacs-scrollbar", wscroll);

  g_object_set_data (G_OBJECT (wscroll), "emacs-end-scroll",
		     (gpointer) end_callback);
  controller = gtk_event_controller_legacy_new ();
  gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
  g_signal_connect (controller, "event",
		    G_CALLBACK (xg_scroll_bar_event_cb), bar);
  gtk_widget_add_controller (wscroll, controller);

  /* Set the cursor to an arrow.  */
  gtk_widget_set_cursor (wscroll, FRAME_DISPLAY_INFO (f)->xg_cursor);

  ctxt = gtk_widget_get_style_context (wscroll);
  gtk_style_context_add_provider (ctxt,
				  GTK_STYLE_PROVIDER
				  (FRAME_OUTPUT_DATA (f)
				   ->scrollbar_foreground_css_provider),
				  GTK_STYLE_PROVIDER_PRIORITY_USER);
  gtk_style_context_add_provider (ctxt,
				  GTK_STYLE_PROVIDER
				  (FRAME_OUTPUT_DATA (f)
				   ->scrollbar_background_css_provider),
				  GTK_STYLE_PROVIDER_PRIORITY_USER);

  /* The scroll bar is shown once it has been positioned.  */
  gtk_widget_set_visible (wscroll, FALSE);
  emacs_fixed_put (FRAME_GTK_WIDGET (f), wscroll, 0, 0);

  bar->x_window = scroll_id;
}

/* Create a scroll bar widget for frame F.  Store the scroll bar
   in BAR.
   SCROLL_CALLBACK is the callback to invoke when the value of the
   bar changes.
   END_CALLBACK is the callback to invoke when scrolling ends.
   SCROLL_BAR_NAME is the name we use for the scroll bar.  Can be used
   to set resources for the widget.  */

void
xg_create_scroll_bar (struct frame *f,
                      struct scroll_bar *bar,
                      GCallback scroll_callback,
                      GCallback end_callback,
                      const char *scroll_bar_name)
{
  GtkAdjustment *vadj;
  GtkWidget *wscroll;

  /* Page, step increment values are not so important here, they
     will be corrected in x_set_toolkit_scroll_bar_thumb. */
  vadj = gtk_adjustment_new (XG_SB_MIN, XG_SB_MIN, XG_SB_MAX,
                             0.1, 0.1, 0.1);

  wscroll = gtk_scrollbar_new (GTK_ORIENTATION_VERTICAL, vadj);
  xg_finish_scroll_bar_creation (f, wscroll, bar, scroll_callback,
                                 end_callback, scroll_bar_name);
  bar->horizontal = 0;
}

/* Create a horizontal scroll bar widget for frame F.  Store the scroll
   bar in BAR.  SCROLL_CALLBACK is the callback to invoke when the value
   of the bar changes.  END_CALLBACK is the callback to invoke when
   scrolling ends.  SCROLL_BAR_NAME is the name we use for the scroll
   bar.  Can be used to set resources for the widget.  */

void
xg_create_horizontal_scroll_bar (struct frame *f,
				 struct scroll_bar *bar,
				 GCallback scroll_callback,
				 GCallback end_callback,
				 const char *scroll_bar_name)
{
  GtkAdjustment *hadj;
  GtkWidget *wscroll;

  /* Page, step increment values are not so important here, they
     will be corrected in x_set_toolkit_scroll_bar_thumb. */
  hadj = gtk_adjustment_new (YG_SB_MIN, YG_SB_MIN, YG_SB_MAX,
                             0.1, 0.1, 0.1);

  wscroll = gtk_scrollbar_new (GTK_ORIENTATION_HORIZONTAL, hadj);
  xg_finish_scroll_bar_creation (f, wscroll, bar, scroll_callback,
                                 end_callback, scroll_bar_name);
  bar->horizontal = 1;
}

/* Remove the scroll bar represented by SCROLLBAR_ID from the frame F.  */

void
xg_remove_scroll_bar (struct frame *f, ptrdiff_t scrollbar_id)
{
  GtkWidget *w = xg_get_widget_from_map (scrollbar_id);

  if (w)
    {
      GtkWidget *wparent = gtk_widget_get_parent (w);

      if (wparent)
	emacs_fixed_remove (wparent, w);
      SET_FRAME_GARBAGED (f);
    }
}

/* Move scroll bar WSCROLL on frame F to LEFT, TOP and give it the size
   WIDTH by HEIGHT, all in device pixels.  If HORIZONTAL, it is a
   horizontal scroll bar.  */

static void
xg_update_scrollbar_pos_1 (struct frame *f, GtkWidget *wscroll,
			   int top, int left, int width, int height,
			   bool horizontal)
{
  double scale = FRAME_GTK_SCALE (f);
  int lwidth = lrint (width / scale);
  int lheight = lrint (height / scale);
  int min_length = (horizontal ? scroll_bar_min_width_for_theme
		    : scroll_bar_min_length_for_theme);

  emacs_fixed_move (FRAME_GTK_WIDGET (f), wscroll,
		    left / scale, top / scale);

  if ((horizontal ? lwidth : lheight) < min_length
      || lwidth <= 0 || lheight <= 0)
    /* No room.  Hide scroll bar as some themes output a warning if
       the size is less than the min size.  */
    gtk_widget_set_visible (wscroll, FALSE);
  else
    {
      gtk_widget_set_size_request (wscroll, lwidth, lheight);
      gtk_widget_set_visible (wscroll, TRUE);
    }

  SET_FRAME_GARBAGED (f);
  cancel_mouse_face (f);
}

/* Update the position of the vertical scroll bar represented by SCROLLBAR_ID
   in frame F.
   TOP/LEFT are the new pixel positions where the bar shall appear.
   WIDTH, HEIGHT is the size in pixels the bar shall have.  */

void
xg_update_scrollbar_pos (struct frame *f,
                         ptrdiff_t scrollbar_id,
                         int top,
                         int left,
                         int width,
                         int height)
{
  GtkWidget *wscroll = xg_get_widget_from_map (scrollbar_id);

  if (wscroll)
    xg_update_scrollbar_pos_1 (f, wscroll, top, left, width, height, false);
}

/* Update the position of the horizontal scroll bar represented by SCROLLBAR_ID
   in frame F.
   TOP/LEFT are the new pixel positions where the bar shall appear.
   WIDTH, HEIGHT is the size in pixels the bar shall have.  */

void
xg_update_horizontal_scrollbar_pos (struct frame *f,
				    ptrdiff_t scrollbar_id,
				    int top,
				    int left,
				    int width,
				    int height)
{
  GtkWidget *wscroll = xg_get_widget_from_map (scrollbar_id);

  if (wscroll)
    xg_update_scrollbar_pos_1 (f, wscroll, top, left, width, height, true);
}

/* Set the thumb size and position of scroll bar BAR.  We are currently
   displaying PORTION out of a whole WHOLE, and our position POSITION.  */

void
xg_set_toolkit_scroll_bar_thumb (struct scroll_bar *bar,
                                 int portion,
                                 int position,
                                 int whole)
{
  struct frame *f = XFRAME (WINDOW_FRAME (XWINDOW (bar->window)));
  GtkWidget *wscroll = xg_get_widget_from_map (bar->x_window);

  if (wscroll && bar->dragging == -1)
    {
      GtkAdjustment *adj;
      gdouble shown;
      gdouble top;
      int size, value;
      int old_size;
      int new_step;
      bool changed = 0;

      adj = gtk_scrollbar_get_adjustment (GTK_SCROLLBAR (wscroll));

      if (scroll_bar_adjust_thumb_portion_p)
        {
          /* We do the same as for MOTIF in xterm.c, use 30 chars per
             line rather than the real portion value.  This makes the
             thumb less likely to resize and that looks better.  */
          portion = WINDOW_TOTAL_LINES (XWINDOW (bar->window)) * 30;

          /* When the thumb is at the bottom, position == whole.
             So we need to increase `whole' to make space for the thumb.  */
          whole += portion;
        }

      if (whole <= 0)
        top = 0, shown = 1;
      else
        {
          top = (gdouble) position / whole;
          shown = (gdouble) portion / whole;
        }

      size = clip_to_bounds (1, shown * XG_SB_RANGE, XG_SB_RANGE);
      value = clip_to_bounds (XG_SB_MIN, top * XG_SB_RANGE, XG_SB_MAX - size);

      /* Assume all lines are of equal size.  */
      new_step = size / max (1, FRAME_LINES (f));

      old_size = gtk_adjustment_get_page_size (adj);
      if (old_size != size)
	{
	  int old_step = gtk_adjustment_get_step_increment (adj);
	  if (old_step != new_step)
	    {
	      gtk_adjustment_set_page_size (adj, size);
	      gtk_adjustment_set_step_increment (adj, new_step);
	      /* Assume a page increment is about 95% of the page size  */
	      gtk_adjustment_set_page_increment (adj, size - size / 20);
	      changed = 1;
	    }
	}

      if (changed || (int) gtk_adjustment_get_value (adj) != value)
      {
        block_input ();

        /* gtk_adjustment_set_value invokes the callback.  Set
           ignore_gtk_scrollbar to make the callback do nothing  */
        xg_ignore_gtk_scrollbar = 1;

        if ((int) gtk_adjustment_get_value (adj) != value)
          gtk_adjustment_set_value (adj, (gdouble) value);

        xg_ignore_gtk_scrollbar = 0;

        unblock_input ();
      }
    }
}

/* Set the thumb size and position of horizontal scroll bar BAR.  We are
   currently displaying PORTION out of a whole WHOLE, and our position
   POSITION.  */

void
xg_set_toolkit_horizontal_scroll_bar_thumb (struct scroll_bar *bar,
					    int portion,
					    int position,
					    int whole)
{
  GtkWidget *wscroll = xg_get_widget_from_map (bar->x_window);

  if (wscroll && bar->dragging == -1)
    {
      GtkAdjustment *adj;
      int lower = 0;
      int upper = max (whole - 1, 0);
      int pagesize = min (upper, max (portion, 0));
      int value = max (0, min (position, upper - pagesize));
      /* These should be set to something more <portion, whole>
	 related.  */
      int page_increment = 4;
      int step_increment = 1;

      block_input ();
      adj = gtk_scrollbar_get_adjustment (GTK_SCROLLBAR (wscroll));
      xg_ignore_gtk_scrollbar = 1;
      gtk_adjustment_configure (adj, (gdouble) value, (gdouble) lower,
				(gdouble) upper, (gdouble) step_increment,
				(gdouble) page_increment, (gdouble) pagesize);
      xg_ignore_gtk_scrollbar = 0;
      unblock_input ();
    }
}


/***********************************************************************
			       Printing
 ***********************************************************************/
#ifdef USE_CAIRO
static GtkPrintSettings *print_settings = NULL;
static GtkPageSetup *page_setup = NULL;

void
xg_page_setup_dialog (void)
{
  GtkPageSetup *new_page_setup = NULL;

  if (print_settings == NULL)
    print_settings = gtk_print_settings_new ();
  new_page_setup = gtk_print_run_page_setup_dialog (NULL, page_setup,
						    print_settings);
  if (page_setup)
    g_object_unref (page_setup);
  page_setup = new_page_setup;
}

Lisp_Object
xg_get_page_setup (void)
{
  Lisp_Object orientation_symbol;

  if (page_setup == NULL)
    page_setup = gtk_page_setup_new ();

  switch (gtk_page_setup_get_orientation (page_setup))
    {
    case GTK_PAGE_ORIENTATION_PORTRAIT:
      orientation_symbol = Qportrait;
      break;
    case GTK_PAGE_ORIENTATION_LANDSCAPE:
      orientation_symbol = Qlandscape;
      break;
    case GTK_PAGE_ORIENTATION_REVERSE_PORTRAIT:
      orientation_symbol = Qreverse_portrait;
      break;
    case GTK_PAGE_ORIENTATION_REVERSE_LANDSCAPE:
      orientation_symbol = Qreverse_landscape;
      break;
    default:
      eassume (false);
    }

#define GETSETUP(f) make_float (f (page_setup, GTK_UNIT_POINTS))
  return
    list (Fcons (Qorientation, orientation_symbol),
	  Fcons (Qwidth, GETSETUP (gtk_page_setup_get_page_width)),
	  Fcons (Qheight, GETSETUP (gtk_page_setup_get_page_height)),
	  Fcons (Qleft_margin, GETSETUP (gtk_page_setup_get_left_margin)),
	  Fcons (Qright_margin, GETSETUP (gtk_page_setup_get_right_margin)),
	  Fcons (Qtop_margin, GETSETUP (gtk_page_setup_get_top_margin)),
	  Fcons (Qbottom_margin, GETSETUP (gtk_page_setup_get_bottom_margin)));
#undef GETSETUP
}

static void
draw_page (GtkPrintOperation *operation, GtkPrintContext *context,
	   gint page_nr, gpointer user_data)
{
  Lisp_Object frames = *((Lisp_Object *) user_data);
  struct frame *f = XFRAME (Fnth (make_fixnum (page_nr), frames));
  cairo_t *cr = gtk_print_context_get_cairo_context (context);

  pgtk_cr_draw_frame (cr, f);
}

void
xg_print_frames_dialog (Lisp_Object frames)
{
  GtkPrintOperation *print;
  GtkPrintOperationResult res;

  print = gtk_print_operation_new ();
  if (print_settings != NULL)
    gtk_print_operation_set_print_settings (print, print_settings);
  if (page_setup != NULL)
    gtk_print_operation_set_default_page_setup (print, page_setup);
  gtk_print_operation_set_n_pages (print, list_length (frames));
  g_signal_connect (print, "draw-page", G_CALLBACK (draw_page), &frames);
  res = gtk_print_operation_run (print, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
                                 NULL, NULL);
  if (res == GTK_PRINT_OPERATION_RESULT_APPLY)
    {
      if (print_settings != NULL)
        g_object_unref (print_settings);
      print_settings =
	g_object_ref (gtk_print_operation_get_print_settings (print));
    }
  g_object_unref (print);
}

#endif	/* USE_CAIRO */


/***********************************************************************
                      Tool bar functions
 ***********************************************************************/

/* GTK 4 has no tool bar widget; the tool bar is a box of flat
   buttons.  */

/* Keys for data stored in tool bar buttons: the image (struct image
   *), or icon name, displayed in the button, its label, and the index
   of the tool bar item it represents.  */
#define XG_TOOL_BAR_IMAGE_DATA "emacs-tool-bar-image"
#define XG_TOOL_BAR_ICON_NAME "emacs-tool-bar-icon-name"
#define XG_TOOL_BAR_LABEL "emacs-tool-bar-label"
#define XG_TOOL_BAR_HORIZ "emacs-tool-bar-horiz"
#define XG_TOOL_BAR_INDEX "emacs-tool-bar-index"

/* The size of icons from the icon theme in the tool bar, in logical
   pixels.  */
#define XG_TOOL_BAR_ICON_SIZE 24

/* Callback function invoked when a tool bar item is pressed.
   W is the button widget in the tool bar that got pressed.  */

static void
xg_tool_bar_callback (GtkButton *w, gpointer client_data)
{
  intptr_t idx = (intptr_t) g_object_get_data (G_OBJECT (w),
					       XG_TOOL_BAR_INDEX);
  struct frame *f = g_object_get_data (G_OBJECT (w), XG_FRAME_DATA);
  GdkModifierType mod = 0;
  GdkDevice *keyboard;
  Lisp_Object key, frame;
  struct input_event event;
  EVENT_INIT (event);

  if (! f || ! f->n_tool_bar_items || NILP (f->tool_bar_items)
      || idx >= f->n_tool_bar_items)
    return;

  /* GTK makes no distinction based on modifiers in the clicked
     callback, so ask for the state of the keyboard.  */
  keyboard = gdk_seat_get_keyboard (gdk_display_get_default_seat
				    (gtk_widget_get_display (GTK_WIDGET (w))));
  if (keyboard)
    mod = gdk_device_get_modifier_state (keyboard);

  idx *= TOOL_BAR_ITEM_NSLOTS;

  key = AREF (f->tool_bar_items, idx + TOOL_BAR_ITEM_KEY);
  XSETFRAME (frame, f);

  event.kind = TOOL_BAR_EVENT;
  event.frame_or_window = frame;
  event.arg = key;
  event.modifiers = pgtk_gtk_to_emacs_modifiers (FRAME_DISPLAY_INFO (f), mod);
  kbd_buffer_store_event (&event);
}

/* Return a widget showing the Emacs image IMG on frame F, or NULL.  */

static GtkWidget *
xg_get_image_for_pixmap (struct frame *f, struct image *img)
{
  cairo_surface_t *surface = NULL;
  GdkTexture *texture;
  GtkWidget *w;

  if (img->cr_data
      && cairo_pattern_get_type (img->cr_data) == CAIRO_PATTERN_TYPE_SURFACE)
    cairo_pattern_get_surface (img->cr_data, &surface);

  if (!surface)
    return NULL;

  /* The image is in device pixels; display it at its natural size.  */
  texture = xg_texture_from_surface (surface);
  w = gtk_image_new_from_paintable (GDK_PAINTABLE (texture));
  gtk_image_set_pixel_size (GTK_IMAGE (w),
			    max (1, lrint (max (img->width, img->height)
					   / FRAME_GTK_SCALE (f))));
  g_object_unref (texture);
  return w;
}

/* Attach a tool bar to frame F.  */

static void
xg_pack_tool_bar (struct frame *f, Lisp_Object pos)
{
  struct pgtk_output *x = f->output_data.pgtk;
  bool into_hbox = EQ (pos, Qleft) || EQ (pos, Qright);
  GtkWidget *top_widget = x->toolbar_widget;
  GtkWidget *child;

  gtk_orientable_set_orientation (GTK_ORIENTABLE (top_widget),
                                  into_hbox
                                  ? GTK_ORIENTATION_VERTICAL
                                  : GTK_ORIENTATION_HORIZONTAL);

  /* Separators are perpendicular to the tool bar.  */
  for (child = gtk_widget_get_first_child (top_widget); child;
       child = gtk_widget_get_next_sibling (child))
    if (GTK_IS_SEPARATOR (child))
      gtk_orientable_set_orientation (GTK_ORIENTABLE (child),
				      into_hbox
				      ? GTK_ORIENTATION_HORIZONTAL
				      : GTK_ORIENTATION_VERTICAL);

  if (into_hbox)
    {
      if (EQ (pos, Qleft))
	gtk_box_prepend (GTK_BOX (x->hbox_widget), top_widget);
      else
	gtk_box_append (GTK_BOX (x->hbox_widget), top_widget);
      x->toolbar_in_hbox = true;
    }
  else
    {
      if (EQ (pos, Qtop))
	gtk_box_insert_child_after (GTK_BOX (x->vbox_widget), top_widget,
				    gtk_widget_get_prev_sibling
				    (x->hbox_widget));
      else
	gtk_box_append (GTK_BOX (x->vbox_widget), top_widget);
      x->toolbar_in_hbox = false;
    }
  x->toolbar_is_packed = true;
}

/* Create a tool bar for frame F.  */

static void
xg_create_tool_bar (struct frame *f)
{
  struct pgtk_output *x = f->output_data.pgtk;
  struct xg_frame_tb_info *tbinfo
    = g_object_get_data (G_OBJECT (FRAME_GTK_OUTER_WIDGET (f)),
                         TB_INFO_KEY);
  if (! tbinfo)
    {
      tbinfo = xmalloc (sizeof (*tbinfo));
      tbinfo->last_tool_bar = Qnil;
      tbinfo->style = Qnil;
      tbinfo->hmargin = tbinfo->vmargin = 0;
      tbinfo->dir = GTK_TEXT_DIR_NONE;
      tbinfo->n_last_items = 0;
      g_object_set_data (G_OBJECT (FRAME_GTK_OUTER_WIDGET (f)),
                         TB_INFO_KEY,
                         tbinfo);
    }

  /* We keep a reference to the tool bar, since it is unpacked and
     packed again when its position changes.  */
  x->toolbar_widget = g_object_ref_sink (gtk_box_new
					 (GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_widget_set_name (x->toolbar_widget, "emacs-toolbar");
  gtk_widget_add_css_class (x->toolbar_widget, "toolbar");
  gtk_widget_add_css_class (x->toolbar_widget, "primary-toolbar");
}

#define PROP(IDX) AREF (f->tool_bar_items, i * TOOL_BAR_ITEM_NSLOTS + (IDX))

/* Find the right-to-left image named by RTL in the tool bar images for F.
   Returns IMAGE if RTL is not found.  */

static Lisp_Object
find_rtl_image (struct frame *f, Lisp_Object image, Lisp_Object rtl)
{
  int i;
  Lisp_Object file, rtl_name;

  rtl_name = Ffile_name_nondirectory (rtl);

  for (i = 0; i < f->n_tool_bar_items; ++i)
    {
      Lisp_Object rtl_image = PROP (TOOL_BAR_ITEM_IMAGES);
      if (!NILP (file = file_for_image (rtl_image)))
        {
          file = calln (Qfile_name_sans_extension,
			Ffile_name_nondirectory (file));
          if (! NILP (Fequal (file, rtl_name)))
            {
              image = rtl_image;
              break;
            }
        }
    }

  return image;
}

/* Make a tool bar button showing the image WIMAGE (if not NULL) and
   LABEL (if not NULL), for the item with index I.  */

static GtkWidget *
xg_make_tool_item (struct frame *f,
                   GtkWidget *wimage,
                   const char *label,
                   int i, bool horiz, bool text_image)
{
  GtkWidget *wb = gtk_button_new ();
  GtkWidget *vb = gtk_box_new (horiz
                               ? GTK_ORIENTATION_HORIZONTAL
                               : GTK_ORIENTATION_VERTICAL,
                               2);

  if (wimage && !text_image)
    gtk_box_append (GTK_BOX (vb), wimage);
  if (label)
    gtk_box_append (GTK_BOX (vb), gtk_label_new (label));
  if (wimage && text_image)
    gtk_box_append (GTK_BOX (vb), wimage);

  gtk_button_set_child (GTK_BUTTON (wb), vb);
  gtk_button_set_has_frame (GTK_BUTTON (wb), FALSE);
  /* Keep the keyboard focus in the edit widget.  */
  gtk_widget_set_focus_on_click (wb, FALSE);
  gtk_widget_set_focusable (wb, FALSE);

  g_object_set_data (G_OBJECT (wb), XG_FRAME_DATA, (gpointer) f);
  g_object_set_data_full (G_OBJECT (wb), XG_TOOL_BAR_LABEL,
			  g_strdup (label), g_free);
  g_object_set_data (G_OBJECT (wb), XG_TOOL_BAR_HORIZ,
		     GINT_TO_POINTER (horiz));
  g_signal_connect (G_OBJECT (wb), "clicked",
		    G_CALLBACK (xg_tool_bar_callback), NULL);
  return wb;
}

/* Return true if the tool bar button WBUTTON doesn't show the icon
   ICON_NAME or the image IMG with LABEL anymore, or HORIZ changed.  */

static bool
xg_tool_item_stale_p (GtkWidget *wbutton, const char *icon_name,
		      const struct image *img, const char *label,
		      bool horiz)
{
  const char *old_label, *old_icon;

  if (!GTK_IS_BUTTON (wbutton))
    return true;

  old_icon = g_object_get_data (G_OBJECT (wbutton), XG_TOOL_BAR_ICON_NAME);
  if (icon_name
      ? !old_icon || strcmp (old_icon, icon_name)
      : old_icon != NULL)
    return true;

  if (!icon_name
      && (g_object_get_data (G_OBJECT (wbutton), XG_TOOL_BAR_IMAGE_DATA)
	  != (gpointer) img))
    return true;

  old_label = g_object_get_data (G_OBJECT (wbutton), XG_TOOL_BAR_LABEL);
  if (label
      ? !old_label || strcmp (old_label, label)
      : old_label != NULL)
    return true;

  return (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (wbutton),
					      XG_TOOL_BAR_HORIZ))
	  != horiz);
}

/* Recompute the size of the tool bar of frame F.  Return true if it
   changed.  */

static bool
xg_update_tool_bar_sizes (struct frame *f)
{
  struct pgtk_output *x = f->output_data.pgtk;
  GtkRequisition req;
  int nl = 0, nr = 0, nt = 0, nb = 0;
  GtkWidget *top_widget = x->toolbar_widget;
  double scale = FRAME_GTK_SCALE (f);

  gtk_widget_get_preferred_size (GTK_WIDGET (top_widget), NULL, &req);
  if (x->toolbar_in_hbox)
    {
      if (!gtk_widget_get_prev_sibling (top_widget))
	nl = lrint (req.width * scale);
      else
	nr = lrint (req.width * scale);
    }
  else
    {
      GtkWidget *w;
      bool top = false;

      /* The tool bar is at the top if it comes before the box holding
	 the edit widget.  */
      for (w = gtk_widget_get_next_sibling (top_widget); w;
	   w = gtk_widget_get_next_sibling (w))
	if (w == x->hbox_widget)
	  top = true;

      if (top)
	nt = lrint (req.height * scale);
      else
	nb = lrint (req.height * scale);
    }

  if (nl != FRAME_TOOLBAR_LEFT_WIDTH (f)
      || nr != FRAME_TOOLBAR_RIGHT_WIDTH (f)
      || nt != FRAME_TOOLBAR_TOP_HEIGHT (f)
      || nb != FRAME_TOOLBAR_BOTTOM_HEIGHT (f))
    {
      FRAME_TOOLBAR_LEFT_WIDTH (f) = nl;
      FRAME_TOOLBAR_RIGHT_WIDTH (f) = nr;
      FRAME_TOOLBAR_TOP_HEIGHT (f) = nt;
      FRAME_TOOLBAR_BOTTOM_HEIGHT (f) = nb;

      return true;
    }
  else
    return false;
}

/* Return the name of an icon in ICON_THEME for NAME, a name found in
   `x-gtk-stock-map', or NULL if there is none.  Names starting with
   "n:" are icon names; GTK 4 has no stock items.  */

static char *
find_icon_from_name (char *name, GtkIconTheme *icon_theme)
{
  if (name[0] == 'n' && name[1] == ':')
    name += 2;

  return gtk_icon_theme_has_icon (icon_theme, name) ? name : NULL;
}

/* Update the tool bar for frame F.  Add new buttons and remove old.  */

void
update_frame_tool_bar (struct frame *f)
{
  int i;
  struct pgtk_output *x = f->output_data.pgtk;
  int hmargin = 0, vmargin = 0;
  GtkWidget *wtoolbar, *cur, *prev;
  GtkTextDirection dir;
  Lisp_Object style;
  bool text_image, horiz;
  struct xg_frame_tb_info *tbinfo;
  GtkIconTheme *icon_theme;

  if (! FRAME_GTK_WIDGET (f))
    return;

  if (! FRAME_GTK_OUTER_WIDGET (f))
    return;

  block_input ();

  if (RANGED_FIXNUMP (1, Vtool_bar_button_margin, INT_MAX))
    {
      hmargin = XFIXNAT (Vtool_bar_button_margin);
      vmargin = XFIXNAT (Vtool_bar_button_margin);
    }
  else if (CONSP (Vtool_bar_button_margin))
    {
      if (RANGED_FIXNUMP (1, XCAR (Vtool_bar_button_margin), INT_MAX))
        hmargin = XFIXNAT (XCAR (Vtool_bar_button_margin));

      if (RANGED_FIXNUMP (1, XCDR (Vtool_bar_button_margin), INT_MAX))
        vmargin = XFIXNAT (XCDR (Vtool_bar_button_margin));
    }

  /* The natural size (i.e. when GTK uses 0 as margin) looks best,
     so take DEFAULT_TOOL_BAR_BUTTON_MARGIN to mean "default for GTK",
     i.e. zero.  This means that margins less than
     DEFAULT_TOOL_BAR_BUTTON_MARGIN has no effect.  */
  hmargin = max (0, hmargin - DEFAULT_TOOL_BAR_BUTTON_MARGIN);
  vmargin = max (0, vmargin - DEFAULT_TOOL_BAR_BUTTON_MARGIN);

  if (! x->toolbar_widget)
    xg_create_tool_bar (f);

  wtoolbar = x->toolbar_widget;
  dir = gtk_widget_get_direction (wtoolbar);

  style = Ftool_bar_get_system_style ();
  icon_theme = gtk_icon_theme_get_for_display (gtk_widget_get_display
					       (FRAME_GTK_OUTER_WIDGET (f)));

  /* Are we up to date? */
  tbinfo = g_object_get_data (G_OBJECT (FRAME_GTK_OUTER_WIDGET (f)),
                              TB_INFO_KEY);

  if (! NILP (tbinfo->last_tool_bar) && ! NILP (f->tool_bar_items)
      && tbinfo->n_last_items == f->n_tool_bar_items
      && tbinfo->hmargin == hmargin && tbinfo->vmargin == vmargin
      && tbinfo->dir == dir
      && ! NILP (Fequal (tbinfo->style, style))
      && ! NILP (Fequal (tbinfo->last_tool_bar, f->tool_bar_items)))
    {
      unblock_input ();
      return;
    }

  tbinfo->last_tool_bar = f->tool_bar_items;
  tbinfo->n_last_items = f->n_tool_bar_items;
  tbinfo->style = style;
  tbinfo->hmargin = hmargin;
  tbinfo->vmargin = vmargin;
  tbinfo->dir = dir;

  text_image = EQ (style, Qtext_image_horiz);
  horiz = EQ (style, Qboth_horiz) || text_image;

  /* CUR is the existing child of the tool bar that is considered for
     reuse, and PREV the last child that has been updated.  */
  cur = gtk_widget_get_first_child (wtoolbar);
  prev = NULL;

  for (i = 0; i < f->n_tool_bar_items; ++i)
    {
      bool enabled_p = !NILP (PROP (TOOL_BAR_ITEM_ENABLED_P));
      bool selected_p = !NILP (PROP (TOOL_BAR_ITEM_SELECTED_P));
      int idx;
      ptrdiff_t img_id;
      struct image *img = NULL;
      Lisp_Object image;
      Lisp_Object stock = Qnil;
      char *icon_name = NULL;
      Lisp_Object rtl;
      GtkWidget *wbutton = NULL;
      Lisp_Object specified_file;
      bool vert_only = ! NILP (PROP (TOOL_BAR_ITEM_VERT_ONLY));
      Lisp_Object label
	= (EQ (style, Qimage) || (vert_only && horiz))
	? Qnil
	: PROP (TOOL_BAR_ITEM_LABEL);
      const char *label_string;
      Lisp_Object help;

      /* If this is a separator, use a gtk separator item.  */
      if (EQ (PROP (TOOL_BAR_ITEM_TYPE), Qt))
	{
	  if (cur && GTK_IS_SEPARATOR (cur))
	    {
	      prev = cur;
	      cur = gtk_widget_get_next_sibling (cur);
	    }
	  else
	    {
	      GtkWidget *sep
		= gtk_separator_new (x->toolbar_in_hbox
				     ? GTK_ORIENTATION_HORIZONTAL
				     : GTK_ORIENTATION_VERTICAL);
	      gtk_box_insert_child_after (GTK_BOX (wtoolbar), sep, prev);
	      prev = sep;
	    }
	  continue;
	}

      /* Ignore invalid image specifications.  */
      image = PROP (TOOL_BAR_ITEM_IMAGES);
      if (!valid_image_p (image))
	continue;

      specified_file = file_for_image (image);
      if (!NILP (specified_file) && !NILP (Ffboundp (Qx_gtk_map_stock)))
        stock = calln (Qx_gtk_map_stock, specified_file);

      if (CONSP (stock))
        {
          Lisp_Object tem;
          for (tem = stock; CONSP (tem); tem = XCDR (tem))
            if (! NILP (tem) && STRINGP (XCAR (tem)))
              {
                icon_name = find_icon_from_name (SSDATA (XCAR (tem)),
						 icon_theme);
                if (icon_name) break;
              }
        }
      else if (STRINGP (stock))
	icon_name = find_icon_from_name (SSDATA (stock), icon_theme);

      if (icon_name == NULL)
        {
          /* No icon, or icon not known.  Try regular image.  If image
             is a vector, choose it according to the button state.  */
          if (dir == GTK_TEXT_DIR_RTL
              && !NILP (rtl = PROP (TOOL_BAR_ITEM_RTL_IMAGE))
              && STRINGP (rtl))
	    image = find_rtl_image (f, image, rtl);

          if (VECTORP (image))
            {
              if (enabled_p)
                idx = (selected_p
                       ? TOOL_BAR_IMAGE_ENABLED_SELECTED
                       : TOOL_BAR_IMAGE_ENABLED_DESELECTED);
              else
                idx = (selected_p
                       ? TOOL_BAR_IMAGE_DISABLED_SELECTED
                       : TOOL_BAR_IMAGE_DISABLED_DESELECTED);

              eassert (ASIZE (image) >= idx);
              image = AREF (image, idx);
            }
          else
            idx = -1;

          img_id = lookup_image (f, image, -1);
          img = IMAGE_FROM_ID (f, img_id);
          prepare_image_for_display (f, img);

          if (img->load_failed_p || img->cr_data == NULL)
	    continue;
        }

      label_string = (NILP (label)
		      ? NULL
		      : STRINGP (label) ? SSDATA (label) : "");

      /* Reuse the existing button if it is up to date, otherwise
	 replace it.  */
      if (cur && !xg_tool_item_stale_p (cur, icon_name, img, label_string,
					horiz))
	{
	  wbutton = cur;
	  cur = gtk_widget_get_next_sibling (cur);
	}
      else
        {
          GtkWidget *w;

	  if (cur)
	    {
	      GtkWidget *next = gtk_widget_get_next_sibling (cur);
	      gtk_box_remove (GTK_BOX (wtoolbar), cur);
	      cur = next;
	    }

	  if (EQ (style, Qtext))
	    w = NULL;
          else if (icon_name)
            {
              w = gtk_image_new_from_icon_name (icon_name);
	      gtk_image_set_pixel_size (GTK_IMAGE (w), XG_TOOL_BAR_ICON_SIZE);
            }
          else
	    w = xg_get_image_for_pixmap (f, img);

          if (w)
            {
              gtk_widget_set_margin_start (w, hmargin);
              gtk_widget_set_margin_end (w, hmargin);
              gtk_widget_set_margin_top (w, vmargin);
              gtk_widget_set_margin_bottom (w, vmargin);
            }

          wbutton = xg_make_tool_item (f, w, label_string, i, horiz,
				       text_image);

	  /* Save the image so we can see if an update is needed the
	     next time we call xg_tool_item_stale_p.  */
	  if (icon_name)
	    g_object_set_data_full (G_OBJECT (wbutton),
				    XG_TOOL_BAR_ICON_NAME,
				    g_strdup (icon_name), g_free);
	  else
	    g_object_set_data (G_OBJECT (wbutton), XG_TOOL_BAR_IMAGE_DATA,
			       img);

          gtk_box_insert_child_after (GTK_BOX (wtoolbar), wbutton, prev);
        }

      prev = wbutton;
      g_object_set_data (G_OBJECT (wbutton), XG_TOOL_BAR_INDEX,
			 (gpointer) (intptr_t) i);
      gtk_widget_set_sensitive (wbutton, enabled_p);

      help = PROP (TOOL_BAR_ITEM_HELP);
      if (NILP (help))
	help = PROP (TOOL_BAR_ITEM_CAPTION);
      gtk_widget_set_tooltip_text (wbutton,
				   (STRINGP (help)
				    ? SSDATA (ENCODE_UTF_8 (help)) : NULL));
    }

  /* Remove buttons not longer needed.  */
  while (cur)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (cur);
      gtk_box_remove (GTK_BOX (wtoolbar), cur);
      cur = next;
    }

  if (f->n_tool_bar_items != 0)
    {
      if (! x->toolbar_is_packed)
        xg_pack_tool_bar (f, FRAME_TOOL_BAR_POSITION (f));
      gtk_widget_set_visible (x->toolbar_widget, TRUE);
      if (xg_update_tool_bar_sizes (f))
	adjust_frame_size (f, -1, -1, 2, false, Qtool_bar_lines);
    }

  /* Set this regardless of whether a tool bar was made or not.  It's
     needed for 'frame-inhibit-implied-resize' to work on GTK.  */
  f->tool_bar_resized = true;

  unblock_input ();
}

#undef PROP

/* Remove the tool bar of frame F from the box it is in.  */

static void
xg_unpack_tool_bar (struct frame *f)
{
  struct pgtk_output *x = f->output_data.pgtk;

  if (x->toolbar_is_packed)
    {
      gtk_box_remove (GTK_BOX (x->toolbar_in_hbox
			       ? x->hbox_widget : x->vbox_widget),
		      x->toolbar_widget);
      x->toolbar_is_packed = false;
    }
}

/* Deallocate all resources for the tool bar on frame F.
   Remove the tool bar.  */

void
free_frame_tool_bar (struct frame *f)
{
  struct pgtk_output *x = f->output_data.pgtk;

  if (x->toolbar_widget)
    {
      struct xg_frame_tb_info *tbinfo;

      block_input ();

      xg_unpack_tool_bar (f);
      g_clear_object (&x->toolbar_widget);

      FRAME_TOOLBAR_TOP_HEIGHT (f) = FRAME_TOOLBAR_BOTTOM_HEIGHT (f) = 0;
      FRAME_TOOLBAR_LEFT_WIDTH (f) = FRAME_TOOLBAR_RIGHT_WIDTH (f) = 0;

      tbinfo = g_object_get_data (G_OBJECT (FRAME_GTK_OUTER_WIDGET (f)),
                                  TB_INFO_KEY);
      if (tbinfo)
        {
          xfree (tbinfo);
          g_object_set_data (G_OBJECT (FRAME_GTK_OUTER_WIDGET (f)),
                             TB_INFO_KEY,
                             NULL);
        }

      adjust_frame_size (f, -1, -1, 2, 0, Qtool_bar_lines);

      unblock_input ();
    }
}

void
xg_change_toolbar_position (struct frame *f, Lisp_Object pos)
{
  struct pgtk_output *x = f->output_data.pgtk;

  if (! x->toolbar_widget)
    return;

  block_input ();
  xg_unpack_tool_bar (f);
  xg_pack_tool_bar (f, pos);

  if (xg_update_tool_bar_sizes (f))
    adjust_frame_size (f, -1, -1, 2, 0, Qtool_bar_lines);

  unblock_input ();
}


/***********************************************************************
                      Initializing
***********************************************************************/

void
xg_initialize (void)
{
  gdpy_def = NULL;
  xg_ignore_gtk_scrollbar = 0;
  xg_menu_cb_list.prev = xg_menu_cb_list.next =
    xg_menu_item_cb_list.prev = xg_menu_item_cb_list.next = 0;

  id_to_widget.max_size = id_to_widget.used = 0;
  id_to_widget.widgets = 0;

  update_theme_scrollbar_width ();
  update_theme_scrollbar_height ();

#ifdef HAVE_FREETYPE
  x_last_font_name = NULL;
#endif

#ifdef HAVE_XWIDGETS
  xg_gtk_initialized = true;
#endif
}
