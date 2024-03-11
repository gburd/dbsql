{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {

  nativeBuildInputs = with pkgs.buildPackages; [
    act
    autoconf
    ed
    gcc
    gdb
    gettext
    libtool
    m4
    pkg-config
    perl
    ripgrep
    python3
  ];

  buildInputs = with pkgs; [
#    (enableDebuging db4)
    db4
    glibc.out
    glibc.static
    tcl-8_5
  ];

  DOCKER_BUILDKIT = 1;
}
