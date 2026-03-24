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

                  # Generate opcodes.h before the build starts
                  # This must happen after configure but before make
                  # Use the pre-generated opcodes.h from build_unix.old
                  postConfigure = ''
                    cp ../build_unix.old/opcodes.h .
                    echo "Copied pre-generated opcodes.h with $(grep -c '^#define OP_' opcodes.h) opcodes"
                  '';

                  # Generate opcodes.h and opcodes.c before building
                  preBuild = ''
                    # Build the lemon parser generator
                    echo "Building lemon parser generator..."
                    gcc -o lemon ../src/lemon/lemon.c
                    cp ../src/lemon/lempar.c .

                    # Generate sql_parser.c from sql_parser.y
                    echo "Generating sql_parser.c..."
                    cp ../src/sql_parser.y .
                    ./lemon sql_parser.y

                    # Apply sed transformations to rename functions
                    cp ./sql_parser.c ./_sql_parser.c
                    sed -e 's/dbsql_parserAlloc/__sql_parser_alloc/g' \
                        -e 's/dbsql_parserFree/__sql_parser_free/g' \
                        -e 's/dbsql_parserTrace/__sql_parser_trace/g' \
                        -e 's/dbsql_parserFallback/__sql_parser_fallback/g' \
                        -e 's/ParseFallback/__sql_parser_fallback/g' \
                        -e 's/dbsql_parser(/__sql_parser(/g' \
                        -e 's/__set_stringn/__str_nappend/g' \
                        -e 's/__set_string/__str_append/g' \
                        ./_sql_parser.c > ./sql_parser.c
                    rm -f ./_sql_parser.c ./sql_parser.y

                    # Verify the sed replacement worked
                    if grep -q "__sql_parser_fallback" ./sql_parser.c; then
                      echo "Successfully renamed fallback function"
                      # Show the function definition for debugging
                      echo "Fallback function in sql_parser.c:"
                      grep -A3 "^int __sql_parser_fallback" ./sql_parser.c || echo "NOT FOUND AS FUNCTION DEF"
                      # Check if YYFALLBACK is defined
                      if grep -q "^#define YYFALLBACK" ./sql_parser.c; then
                        echo "YYFALLBACK is defined in sql_parser.c"
                      else
                        echo "WARNING: YYFALLBACK is NOT defined in sql_parser.c"
                      fi
                    else
                      echo "WARNING: Fallback function not found after sed"
                    fi

                    # Add fallback function declaration to sql_parser.h
                    echo "int __sql_parser_fallback(int);" >> sql_parser.h
                    echo "Added fallback declaration to sql_parser.h"

                    echo "Generated sql_parser.c and sql_parser.h"

                    # Note: opcodes.h will be generated by the Makefile based on vdbe.c
                    # The Makefile has proper dependency tracking for this
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
