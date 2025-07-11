{
  description = "A shared-memory multilevel graph and hypergraph partitioner";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { nixpkgs, flake-utils, ... }: flake-utils.lib.eachDefaultSystem (system:
    let
      pkgs = import nixpkgs { inherit system; };
    in
    {
      devShells.default = pkgs.mkShell {
        packages = builtins.attrValues {
          # Mt-KaHyPar inputs
          inherit (pkgs) python3 cmake tbb_2022_0 boost hwloc;

          # Development inputs
          inherit (pkgs) ccache ninja mold-wrapped gdb;
        };
      };
    }
  );
}
