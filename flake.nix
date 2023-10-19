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
                dbsql = with final; stdenv.mkDerivation rec {
                  inherit version;
                  inherit system;
                  pname = "dbsql";
                  src = self;
                  configureFlags = [
                    "--host ${system}"
                  ];
                  nativeBuildInputs = [ autoreconfHook ];
                  meta = {
                    changelog = "https://git.burd.me/greg/dbsql/raw/branch/main/ChangeLog";
                    downloadPage = "https://git.burd.me/greg/dbsql/releases";
                    homepage = "https://github.com/gburd/dbsql";
                    license = "https://github.com/gburd/dbsql/LICENSE";
                    mainProgram = "dbsql";
                    maintainers = [ "Greg Burd <greg@burd.me>" ];
                    platforms = supportedSystems;
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
