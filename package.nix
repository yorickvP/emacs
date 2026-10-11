# Build this tree as Emacs with the pure GTK 4 front end:
#
#   nix-build package.nix
#   nix profile install -f package.nix
#
# This reuses nixpkgs' emacs31-pgtk, swapping GTK 3 for GTK 4.  xwidgets
# are GTK 3 only for now.
{
  pkgs ? import <nixpkgs> { },
}:
let
  inherit (pkgs) lib;
in
(pkgs.emacs31-pgtk.override {
  srcRepo = true;
  withXwidgets = false;
}).overrideAttrs (old: {
  pname = "emacs-pgtk-gtk4";
  version = "32.0.50";

  # Leave out out-of-tree build directories.
  src = lib.cleanSourceWith {
    src = lib.cleanSource ./.;
    filter =
      path: type: !(type == "directory" && builtins.pathExists (path + "/config.status"));
  };

  # The 31.1 security patch is already part of this tree.
  patches = builtins.filter (
    p: !(lib.isDerivation p && lib.hasPrefix "CVE-" p.name)
  ) old.patches;

  configureFlags = old.configureFlags ++ [ "--with-gtk4" ];
  buildInputs = lib.subtractLists [ pkgs.gtk3 ] old.buildInputs ++ [ pkgs.gtk4 ];
  nativeBuildInputs =
    lib.subtractLists [ pkgs.wrapGAppsHook3 ] old.nativeBuildInputs
    ++ [ pkgs.wrapGAppsHook4 ];
})
