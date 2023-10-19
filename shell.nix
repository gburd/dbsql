{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  # nativeBuildInputs is usually what you want -- tools you need to run
  nativeBuildInputs = with pkgs.buildPackages; [
    act
    autoconf
    db4
    ed
    gcc
    gdb
    gettext
    libtool
    m4
    perl
    ripgrep
    tcl-8_5
  ];

  buildInputs = with pkgs; [
    glibc.out
    glibc.static
  ];

  DOCKER_BUILDKIT = 1;
}
