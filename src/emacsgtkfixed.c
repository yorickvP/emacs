/* A Gtk Widget that inherits GtkFixed, but can be shrunk.
This file is only use when compiling with Gtk+ 3.

Copyright (C) 2011-2026 Free Software Foundation, Inc.

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

#include <config.h>

#include "lisp.h"
#include "frame.h"
#ifdef HAVE_PGTK
#include "pgtkterm.h"
#else
#include "xterm.h"
#endif
#include "emacsgtkfixed.h"

#ifdef HAVE_GTK4

struct _EmacsFixed
{
  GtkWidget parent_instance;

  /* The frame whose edit widget this is, or NULL once the frame has
     been deleted.  */
  struct frame *f;
};

struct _EmacsFixedClass
{
  GtkWidgetClass parent_class;
};

G_DEFINE_TYPE (EmacsFixed, emacs_fixed, GTK_TYPE_WIDGET)

/* The position of a child, in logical pixels relative to the
   EmacsFixed it is placed in.  */
struct emacs_fixed_position
{
  double x, y;
};

static GQuark emacs_fixed_position_quark;

static void
emacs_fixed_measure (GtkWidget *widget, GtkOrientation orientation,
		     int for_size, int *minimum, int *natural,
		     int *minimum_baseline, int *natural_baseline)
{
  struct frame *f = EMACS_FIXED (widget)->f;
  int min = 1, nat = 1;

  /* Children never affect the size of the widget: scroll bars and
     child frames are clipped to the frame, as under X.  */
  if (f && FRAME_LIVE_P (f) && f->output_data.pgtk)
    {
      if (orientation == GTK_ORIENTATION_HORIZONTAL)
	{
	  min = f->output_data.pgtk->size_hints.min_width;
	  nat = f->output_data.pgtk->preferred_width;
	}
      else
	{
	  min = f->output_data.pgtk->size_hints.min_height;
	  nat = f->output_data.pgtk->preferred_height;
	}
    }

  *minimum = max (min, 1);
  *natural = max (nat, *minimum);
}

static void
emacs_fixed_size_allocate (GtkWidget *widget, int width, int height,
			   int baseline)
{
  GtkWidget *child;
  struct frame *f = EMACS_FIXED (widget)->f;

  for (child = gtk_widget_get_first_child (widget); child;
       child = gtk_widget_get_next_sibling (child))
    {
      struct emacs_fixed_position *pos;
      GtkRequisition min, nat;
      int w, h;
      GskTransform *transform;

      /* Popovers, used for popup menus and tooltip frames, must be
	 presented by their parent when it is allocated.  */
      if (GTK_IS_POPOVER (child))
	{
	  if (gtk_widget_get_visible (child))
	    gtk_popover_present (GTK_POPOVER (child));
	  continue;
	}

      if (!gtk_widget_should_layout (child))
	continue;

      pos = g_object_get_qdata (G_OBJECT (child),
				emacs_fixed_position_quark);
      gtk_widget_get_preferred_size (child, &min, &nat);

      /* Children are given the size they requested, which is how
	 Emacs sizes scroll bars and child frames, or else their
	 natural size.  */
      gtk_widget_get_size_request (child, &w, &h);
      w = max (w < 0 ? nat.width : w, min.width);
      h = max (h < 0 ? nat.height : h, min.height);

      transform = gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT
					   (pos ? pos->x : 0,
					    pos ? pos->y : 0));
      gtk_widget_allocate (child, w, h, -1, transform);
    }

  if (f && FRAME_LIVE_P (f) && f->output_data.pgtk)
    pgtk_edit_widget_allocated (f, width, height);
}

static void
emacs_fixed_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  GtkWidget *child;
  struct frame *f = EMACS_FIXED (widget)->f;

  if (f && FRAME_LIVE_P (f) && f->output_data.pgtk)
    pgtk_snapshot_frame (f, widget, snapshot);

  for (child = gtk_widget_get_first_child (widget); child;
       child = gtk_widget_get_next_sibling (child))
    if (!GTK_IS_NATIVE (child))
      gtk_widget_snapshot_child (widget, child, snapshot);
}

static void
emacs_fixed_dispose (GObject *object)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (object))))
    gtk_widget_unparent (child);

  EMACS_FIXED (object)->f = NULL;
  G_OBJECT_CLASS (emacs_fixed_parent_class)->dispose (object);
}

static void
emacs_fixed_class_init (EmacsFixedClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = emacs_fixed_dispose;
  widget_class->measure = emacs_fixed_measure;
  widget_class->size_allocate = emacs_fixed_size_allocate;
  widget_class->snapshot = emacs_fixed_snapshot;
  gtk_widget_class_set_css_name (widget_class, "emacs-frame");
  emacs_fixed_position_quark
    = g_quark_from_static_string ("emacs-fixed-position");
}

static void
emacs_fixed_init (EmacsFixed *fixed)
{
  GtkWidget *widget = GTK_WIDGET (fixed);

  fixed->f = NULL;
  gtk_widget_set_focusable (widget, TRUE);
  gtk_widget_set_hexpand (widget, TRUE);
  gtk_widget_set_vexpand (widget, TRUE);
  gtk_widget_set_overflow (widget, GTK_OVERFLOW_HIDDEN);
}

GtkWidget *
emacs_fixed_new (struct frame *f)
{
  EmacsFixed *fixed = g_object_new (EMACS_TYPE_FIXED, NULL);

  fixed->f = f;
  return GTK_WIDGET (fixed);
}

/* Return the frame whose edit widget is FIXED, or NULL if it has been
   deleted.  */

struct frame *
emacs_fixed_get_frame (GtkWidget *fixed)
{
  return EMACS_FIXED (fixed)->f;
}

/* Make FIXED the edit widget of F, which can be NULL.  */

void
emacs_fixed_set_frame (GtkWidget *fixed, struct frame *f)
{
  EMACS_FIXED (fixed)->f = f;
}

/* Place CHILD in FIXED at the logical coordinates X and Y.  */

void
emacs_fixed_put (GtkWidget *fixed, GtkWidget *child, double x, double y)
{
  struct emacs_fixed_position *pos = g_new (struct emacs_fixed_position, 1);

  pos->x = x;
  pos->y = y;
  g_object_set_qdata_full (G_OBJECT (child), emacs_fixed_position_quark,
			   pos, g_free);
  gtk_widget_set_parent (child, fixed);
}

/* Move CHILD, a child of FIXED, to the logical coordinates X and
   Y.  */

void
emacs_fixed_move (GtkWidget *fixed, GtkWidget *child, double x, double y)
{
  struct emacs_fixed_position *pos
    = g_object_get_qdata (G_OBJECT (child), emacs_fixed_position_quark);

  if (pos && pos->x == x && pos->y == y)
    return;

  if (!pos)
    {
      pos = g_new (struct emacs_fixed_position, 1);
      g_object_set_qdata_full (G_OBJECT (child), emacs_fixed_position_quark,
			       pos, g_free);
    }

  pos->x = x;
  pos->y = y;
  gtk_widget_queue_allocate (fixed);
}

/* Remove CHILD from FIXED.  */

void
emacs_fixed_remove (GtkWidget *fixed, GtkWidget *child)
{
  eassert (gtk_widget_get_parent (child) == fixed);
  gtk_widget_unparent (child);
}

#else /* !HAVE_GTK4 */

/* Silence a bogus diagnostic; see GNOME bug 683906.  */
#if GNUC_PREREQ (4, 7, 0) && ! GLIB_CHECK_VERSION (2, 35, 7)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wunused-local-typedefs"
#endif

typedef struct _EmacsFixed EmacsFixed;
typedef struct _EmacsFixedClass EmacsFixedClass;

struct _EmacsFixedPrivate
{
  struct frame *f;
};


static void emacs_fixed_get_preferred_width  (GtkWidget *widget,
                                              gint      *minimum,
                                              gint      *natural);
static void emacs_fixed_get_preferred_height (GtkWidget *widget,
                                              gint      *minimum,
                                              gint      *natural);
#ifndef HAVE_PGTK
static GType emacs_fixed_get_type (void);
#endif
G_DEFINE_TYPE (EmacsFixed, emacs_fixed, GTK_TYPE_FIXED)

static EmacsFixed *
EMACS_FIXED (GtkWidget *widget)
{
  return G_TYPE_CHECK_INSTANCE_CAST (widget, emacs_fixed_get_type (),
				     EmacsFixed);
}

static void
emacs_fixed_class_init (EmacsFixedClass *klass)
{
  GtkWidgetClass *widget_class;

  widget_class = (GtkWidgetClass *) klass;

  widget_class->get_preferred_width = emacs_fixed_get_preferred_width;
  widget_class->get_preferred_height = emacs_fixed_get_preferred_height;
  g_type_class_add_private (klass, sizeof (EmacsFixedPrivate));
}

static void
emacs_fixed_init (EmacsFixed *fixed)
{
  fixed->priv = G_TYPE_INSTANCE_GET_PRIVATE (fixed, emacs_fixed_get_type (),
                                             EmacsFixedPrivate);
  fixed->priv->f = 0;
}

GtkWidget *
emacs_fixed_new (struct frame *f)
{
  EmacsFixed *fixed = g_object_new (emacs_fixed_get_type (), NULL);
  EmacsFixedPrivate *priv = fixed->priv;
  priv->f = f;
  return GTK_WIDGET (fixed);
}

static void
emacs_fixed_get_preferred_width (GtkWidget *widget,
                                 gint      *minimum,
                                 gint      *natural)
{
  EmacsFixed *fixed = EMACS_FIXED (widget);
  EmacsFixedPrivate *priv = fixed->priv;
#ifdef HAVE_PGTK
  int w = priv->f->output_data.pgtk->size_hints.min_width;
  if (minimum) *minimum = w;
  if (natural) *natural = priv->f->output_data.pgtk->preferred_width;
#else
  int w = priv->f->output_data.x->size_hints.min_width;
  if (minimum) *minimum = w;
  if (natural) *natural = w;
#endif
}

static void
emacs_fixed_get_preferred_height (GtkWidget *widget,
                                  gint      *minimum,
                                  gint      *natural)
{
  EmacsFixed *fixed = EMACS_FIXED (widget);
  EmacsFixedPrivate *priv = fixed->priv;
#ifdef HAVE_PGTK
  int h = priv->f->output_data.pgtk->size_hints.min_height;
  if (minimum) *minimum = h;
  if (natural) *natural = priv->f->output_data.pgtk->preferred_height;
#else
  int h = priv->f->output_data.x->size_hints.min_height;
  if (minimum) *minimum = h;
  if (natural) *natural = h;
#endif
}


#ifndef HAVE_PGTK

/* Override the X function so we can intercept Gtk+ 3 calls.
   Use our values for min_width/height so that KDE don't freak out
   (Bug#8919), and so users can resize our frames as they wish.  */

void
XSetWMSizeHints (Display *d,
                 Window w,
                 XSizeHints *hints,
                 Atom prop)
{
  struct x_display_info *dpyinfo = x_display_info_for_display (d);
  struct frame *f = x_top_window_to_frame (dpyinfo, w);
  long data[18];
  data[0] = hints->flags;
  data[1] = hints->x;
  data[2] = hints->y;
  data[3] = hints->width;
  data[4] = hints->height;
  data[5] = hints->min_width;
  data[6] = hints->min_height;
  data[7] = hints->max_width;
  data[8] = hints->max_height;
  data[9] = hints->width_inc;
  data[10] = hints->height_inc;
  data[11] = hints->min_aspect.x;
  data[12] = hints->min_aspect.y;
  data[13] = hints->max_aspect.x;
  data[14] = hints->max_aspect.y;
  data[15] = hints->base_width;
  data[16] = hints->base_height;
  data[17] = hints->win_gravity;

  if ((hints->flags & PMinSize) && f)
    {
      /* Overriding the size hints with our own values of min_width
	 and min_height used to work, but these days just results in
	 frames resizing unpredictably and emitting GTK warnings while
	 Emacs fights with GTK over the size of the frame.  So instead
	 of doing that, just respect the hints set by GTK, but make
	 sure they are an integer multiple of the resize increments so
	 that bug#8919 stays fixed.  */

      /* int w = f->output_data.x->size_hints.min_width;
         int h = f->output_data.x->size_hints.min_height;

	 data[5] = w;
	 data[6] = h; */

      /* Make sure min_width and min_height are multiples of width_inc
	 and height_inc.  */

      if (hints->flags & PResizeInc)
	{
	  /* Some versions of GTK set PResizeInc even if the
	     increments are at their initial values.  */

	  if (hints->width_inc && data[5] % hints->width_inc)
	    data[5] += (hints->width_inc - (data[5] % hints->width_inc));
	  if (hints->height_inc && data[6] % hints->height_inc)
	    data[6] += (hints->height_inc - (data[6] % hints->height_inc));
	}
    }

  XChangeProperty (d, w, prop, XA_WM_SIZE_HINTS, 32, PropModeReplace,
		   (unsigned char *) data, 18);
}

/* Override this X11 function.
   This function is in the same X11 file as the one above.  So we must
   provide it also.  */

void
XSetWMNormalHints (Display *d, Window w, XSizeHints *hints)
{
  XSetWMSizeHints (d, w, hints, XA_WM_NORMAL_HINTS);
}

#endif

#endif /* !HAVE_GTK4 */
