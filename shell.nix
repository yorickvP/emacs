# Development shell for the GTK 4 port of the pgtk build.
#   nix-shell --run './autogen.sh && ./configure --with-pgtk --with-gtk4 ...'
{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  inputsFrom = [ pkgs.emacs-pgtk ];
  packages = with pkgs; [
    gtk4
    autoconf
    automake
    texinfo
    pkg-config
    gdb
    # Headless Wayland testing.
    sway
    grim
    wtype
    wlrctl
    jq
    imagemagick
    wl-clipboard
  ];
}
