{
  description = "The DBSQL library, a mixup of SQLite and Berkeley DB";

  nixConfig = {
    bash-prompt = "\\[\\e[34;1m\\]nix ~ \\[\\e[0m\\]";
  };

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs";
    flake-utils.url = "github:numtide/flake-utils";
  };
  inputs.pre-commit-hooks.url = "github:cachix/pre-commit-hooks.nix";
  inputs.nix-github-actions.url = "github:nix-community/nix-github-actions";
  inputs.nix-github-actions.inputs.nixpkgs.follows = "nixpkgs";

  outputs = { self, nixpkgs, flake-utils, pre-commit-hooks, nix-github-actions }:
    let
      inherit (nixpkgs) lib;
      officialRelease = false;

      #version = "DBSQL ${DBSQL_VERSION}: (${versionSuffix})";
      version = "DBSQL 0.3.1: (${versionSuffix})";
      versionSuffix =
        if officialRelease
        then ""
        else "pre${builtins.substring 0 8 (self.lastModifiedDate or self.lastModified or "19700101")}_${self.shortRev or "dirty"}";

      supportedSystems = [ "x86_64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
      nixpkgsFor = forAllSystems (system: import nixpkgs {
        inherit system;
        overlays = [ self.overlay ];
      });

    in
      flake-utils.lib.eachDefaultSystem (system:
        let
          pkgs = import nixpkgs {
            inherit system;

            overlays = [

              (final: prev: {
                dbsql = with final; let
                  # Combine db4 outputs for configure script
                  db4-combined = symlinkJoin {
                    name = "db4-combined";
                    paths = [ db4.out db4.dev ];
                  };
                in stdenv.mkDerivation rec {
                  inherit version;
                  pname = "dbsql";
                  src = self;

                  buildInputs = [
                    db4
                    readline
                    tcl-8_5
                  ];

                  nativeBuildInputs = [
                    autoreconfHook
                    perl
                  ];

                  # Point autoreconfHook to the dist directory where configure.ac lives
                  autoreconfFlags = [ "-fi" "-Iaclocal" ];

                  # Disable autoheader since we'll generate config.hin manually
                  AUTOHEADER = "true";

                  configureFlags = [
                    "--with-berkeleydb=${db4-combined}"
                    "--enable-tcl"
                    "--with-tcl=${tcl-8_5}/lib"
                    "ac_cv_func_srand48_r=yes"
                    "ac_cv_func_lrand48_r=yes"
                  ];

                  dontStrip = true;
                  separateDebugInfo = false;

                  # autoreconfHook runs in the sourceRoot, which is the top-level directory
                  # We need to tell it to run in the dist directory where configure.ac lives
                  preAutoreconf = ''
                    cd dist
                    # Keep the existing config.hin instead of generating a minimal one
                    # The full config.hin has all the necessary #undef templates
                  '';

                  postAutoreconf = ''
                    # We're still in dist/ from preAutoreconf
                    # Substitute version placeholders if needed
                    sed -i "s/__EDIT_DBSQL_VERSION__/${version}/g" configure
                    sed -i "s/__EDIT_DBSQL_VERSION_MAJOR__/0/g" configure
                    sed -i "s/__EDIT_DBSQL_VERSION_MINOR__/4/g" configure
                    sed -i "s/__EDIT_DBSQL_VERSION_PATCH__/0/g" configure
                    sed -i "s/__EDIT_DBSQL_VERSION_STRING__/${version}/g" configure
                    # Return to source root for subsequent phases
                    cd ..
                  '';

                  # After autoreconf generates configure in dist/, follow Berkeley DB pattern
                  preConfigure = ''
                    mkdir -p build_unix
                    cd build_unix
                    # Symlink config.hin so configure can find it
                    ln -sf ../dist/config.hin .
                    configureScript=../dist/configure
                  '';

                  # Generate opcodes.h and opcodes.c before building
                  preBuild = ''
                    # Generate opcodes.h by extracting OP_* case statements from vdbe.c
                    echo '/* Automatically generated file.  Do not edit */' > opcodes.h
                    grep '^case OP_' ../src/vdbe.c | \
                      sed -e 's/:.*//' | \
                      awk '{printf "#define %-30s %3d\n", $2, ++cnt} END {print ""; print "/* Legacy DBSQL compatibility opcodes */"; printf "#define %-30s %3d  /* Copy register value */\n", "OP_Copy", cnt+1; printf "#define %-30s %3d  /* Legacy: use OP_ResultRow */\n", "OP_SortCallback", cnt+2; printf "#define %-30s %3d  /* Legacy: use OP_ResultRow */\n", "OP_NullCallback", cnt+3}' >> opcodes.h

                    echo "Generated opcodes.h with $(grep -c '^#define OP_' opcodes.h) opcodes"
                  '';

                  meta = with lib; {
                    description = "The DBSQL library, a mixup of SQLite and Berkeley DB";
                    changelog = "https://git.burd.me/greg/dbsql/raw/branch/main/ChangeLog";
                    downloadPage = "https://git.burd.me/greg/dbsql/releases";
                    homepage = "https://github.com/gburd/dbsql";
                    license = licenses.gpl3Plus;
                    mainProgram = "dbsql";
                    maintainers = [ "Greg Burd <greg@burd.me>" ];
                    platforms = platforms.linux;
                  };
                };
              })
            ];

            checks = {
              pre-commit-check = pre-commit-hooks.lib.${system}.run {
                src = ./.;
                hooks = {
                  nixpkgs-fmt.enable = true;
                };
              };
            };

            devShell = nixpkgs.legacyPackages.${system}.mkShell {
              inherit (self.checks.${system}.pre-commit-check) shellHook;
            };

          };

        in rec {
          packages = {
            inherit (pkgs) dbsql;

            githubActions = nix-github-actions.lib.mkGithubMatrix {
              checks = nixpkgs.lib.getAttrs [ "x86_64-linux" "x86_64-darwin" ] self.packages;
            };
          };
          packages.default = self.packages.${system}.dbsql;
          packages.container = pkgs.callPackage ./container.nix { package = packages.default; };

          apps.dbsql = flake-utils.lib.mkApp { drv = packages.default; };
          apps.${system}.default = apps.dbsql;

          devShells.default = import ./shell.nix { inherit pkgs; };
        }
      );
}
