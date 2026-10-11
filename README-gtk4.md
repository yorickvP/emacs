# Emacs with GTK 4

This is an unofficial fork of GNU Emacs whose pure GTK ("pgtk") build
can use GTK 4 instead of GTK 3.  It exists mainly for Wayland desktops
with fractional scaling (say 125% or 150%): GTK 3 makes every character
cell a whole number of *logical* pixels, so monospace text is either
too wide or too narrow and gets resampled.  With GTK 4, Emacs draws in
the screen's real pixels, so text is sharp and the character grid fits
the font.

**Do not report bugs in this build to the Emacs developers** (with
`M-x report-emacs-bug`, bug-gnu-emacs@gnu.org, or emacs-devel).  The
GTK 4 code was written with an AI model (Claude), and GNU Emacs does
not accept LLM-generated contributions or bug reports.  Report
problems to the maintainer of this fork instead.  If a problem also
happens in a regular Emacs build, report it from that build.

## Building

GTK 4.14 or later and GLib 2.76 or later are needed.

    ./autogen.sh
    ./configure --with-gtk4    # implies --with-pgtk
    make

The file dialog needs GTK's GSettings schemas, which desktop
distributions install anyway.

With Nix, `package.nix` builds this tree on top of nixpkgs'
`emacs31-pgtk`, with native compilation:

    nix-build package.nix              # or: nix profile install -f package.nix

xwidgets are not available, since they need GTK 3.

## What is different from GTK 3

* **Pixels.**  Frame pixels are device pixels, so at 150% Lisp code
  sees 1.5 times as many pixels as under GTK 3, for instance in
  `frame-pixel-width` or `window-pixel-height`.  Fonts are opened at
  the resolution of the monitor the frame is on.
* **Moving between monitors** with different scales reopens the fonts
  at the same point size and keeps the window size, so the number of
  columns and lines changes a little, as in other programs.  Some font
  sizes give a cell whose width is much closer to the font's design
  than others: at 150%, DejaVu Sans Mono 12.5pt has 15-pixel cells
  where 12pt rounds 14.45 down to 14.
* **Dead keys** work without an input method daemon.  GTK 4 leaves
  composing them to the input method, so without one Emacs composes
  them itself (with `iso-transl`).
* **Printing** with the GTK print dialog: `M-x pgtk-print-frames-dialog`.
  (File → Print in the menu bar uses `lpr` instead.)

## Scrolling

High resolution mouse wheels and touchpads scroll by pixels with
`pixel-scroll-precision-mode`.  Wheel events report the distance GTK 4's
own scrolling widgets use per detent, which `pixel-scroll` then doubles
for mice (`pixel-scroll-precision-interpolation-factor`), and touchpads
follow the fingers exactly.

    (pixel-scroll-precision-mode 1)

Settings specific to this build:

* `pgtk-scroll-event-delta-factor` scales the distance scrolled by
  pixels (default 1.0).
* `pgtk-wheel-clicks-per-detent` sets how many clicks a detent of the
  wheel counts as (default 1).  With 2 or 3, a high resolution wheel
  scrolls by lines more often, and programs that act once per click
  react to part of a detent.

Wheel events in pixel scrolling mode also carry the number of whole
clicks they complete as their line count (`(nth 3 event)`), which is 0
for a part of a click.  Ghostel, for instance, sends a program that
tracks the mouse one press for every wheel event from a mouse; this
makes it send one press per click instead:

```elisp
(defvar ghostel--term)
(declare-function ghostel--terminal-input-mode-p "ghostel")
(declare-function ghostel--mouse-tracking-p "ghostel-module")

(defun my-ghostel-forward-wheel-clicks (orig event button)
  "Call ORIG to forward EVENT as BUTTON once per click it completed."
  (let ((clicks (nth 3 event)))
    (if (and (integerp clicks)
             (eq (device-class last-event-frame last-event-device) 'mouse)
             (ghostel--terminal-input-mode-p)
             (ghostel--mouse-tracking-p ghostel--term))
        (progn (dotimes (_ clicks) (funcall orig event button))
               t)
      (funcall orig event button))))

(with-eval-after-load 'ghostel
  (advice-add 'ghostel--forward-scroll-event :around
              #'my-ghostel-forward-wheel-clicks))
```

## Known limitations

* Input method daemons (IBus, Fcitx) have not been tried.
* The GTK 4 X11 backend has not been tried; as with GTK 3, pgtk is
  meant for Wayland.
* Emacs can't know or set where its frames are on the screen, as with
  GTK 3 under Wayland.
* Dragging text or files out of Emacs isn't supported (dropping into
  it is).
* Keyboard shortcuts in menus are shown in the item's label, since GTK
  4 menus only show one shortcut per item in their own column.
* Every redisplay uploads the whole frame to the GPU, which might be
  slow for large frames on slow GPUs.

## When Emacs crashes

A crash prints a backtrace of addresses, like

    ./result/bin/emacs(+0x181016) [0x5c4cc71e6016]

Include it in your report, together with what you did.  `package.nix`
keeps the debug information in a separate `debug` output, which turns
those addresses into source lines:

    nix-build package.nix -A debug -o result-debug
    export NIX_DEBUG_INFO_DIRS=$PWD/result-debug/lib/debug
    gdb -batch -ex 'info line *0x181016' ./result/bin/.emacs-*-wrapped

To get a full backtrace, run Emacs in GDB, and type `bt` at the
`(gdb)` prompt after the crash:

    gdb -ex run --args ./result/bin/emacs

## Where the code is

The GTK 4 code is mostly in `src/gtk4util.c`, the GTK 4 counterpart of
`src/gtkutil.c`, and in the `HAVE_GTK4` parts of `src/pgtkterm.c`,
`src/pgtkfns.c`, `src/pgtkselect.c` and `src/emacsgtkfixed.c`.

## License and copyright

Like GNU Emacs, this fork is free software under the GNU General
Public License, version 3 or later; see `COPYING`.

The GNU Emacs code it is based on is copyrighted by the Free Software
Foundation, and its files keep their copyright notices.  The changes
for GTK 4 were generated with an AI model (Claude).  They are not part
of GNU Emacs, and their copyright, if any, has not been assigned to the
Free Software Foundation.  Files changed for the fork say so in their
headers; `src/gtk4util.c` is new, with parts adapted from
`src/gtkutil.c`.  The Git history shows exactly what changed.
