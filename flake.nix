{
  description = "A shared-memory multilevel graph and hypergraph partitioner";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils, ... }: flake-utils.lib.eachDefaultSystem (system:
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

      packages.default =
        let
          kahypar-shared-resources-src = pkgs.fetchFromGitHub {
            owner = "kahypar";
            repo = "kahypar-shared-resources";
            rev = "6d5c8e2444e4310667ec1925e995f26179d7ee88";
            hash = "sha256-K3tQ9nSJrANdJPf7v/ko2etQLDq2f7Z0V/kvDuWKExM=";
          };

          whfc-src = pkgs.fetchFromGitHub {
            owner = "larsgottesbueren";
            repo = "WHFC";
            rev = "5ae2e3664391ca0db7fab2c82973e98c48937a08";
            hash = "sha256-Oyz6u1uAgQUTOjSWBC9hLbupmQwbzcZJcyxNnj7+qxo=";
          };

          growt-src = pkgs.fetchFromGitHub {
            owner = "TooBiased";
            repo = "growt";
            rev = "0c1148ebcdfd4c04803be79706533ad09cc81d37";
            hash = "sha256-4Vm4EiwmwCs3nyBdRg/MAk8SUWtX6kTukj8gJ7HfJNY=";
          };
        in
        pkgs.stdenv.mkDerivation {
          pname = "Mt-KaHyPar";
          version = "1.5";

          src = self;
          strictDeps = true;

          nativeBuildInputs = builtins.attrValues {
            inherit (pkgs) python3 cmake;
          };

          propagatedBuildInputs = builtins.attrValues {
            inherit (pkgs) tbb_2022_0;
          };

          buildInputs = builtins.attrValues {
            inherit (pkgs) boost hwloc;
          };

          preConfigure = ''
            # Remove the FetchContent_Populate calls in CMakeLists.txt
            sed -i '266,284d' CMakeLists.txt

            # Replace the target_include_directories with custom paths
            substituteInPlace CMakeLists.txt \
              --replace ''\'''${CMAKE_CURRENT_BINARY_DIR}/external_tools/kahypar-shared-resources' '${kahypar-shared-resources-src}'
            substituteInPlace CMakeLists.txt \
              --replace ''\'''${CMAKE_CURRENT_BINARY_DIR}/external_tools/growt' '${growt-src}'
            substituteInPlace CMakeLists.txt \
              --replace ''\'''${CMAKE_CURRENT_BINARY_DIR}/external_tools/WHFC' '${whfc-src}'
          '';

          cmakeFlags = [
            # The cmake package does not handle absolute CMAKE_INSTALL_INCLUDEDIR
            # correctly (setting it to an absolute path causes include files to go to
            # $out/$out/include, because the absolute path is interpreted with root
            # at $out).
            # See: https://github.com/NixOS/nixpkgs/issues/144170
            "-DCMAKE_INSTALL_INCLUDEDIR=include"
            "-DCMAKE_INSTALL_LIBDIR=lib"
          ];

          meta = {
            description = "Shared-memory multilevel graph and hypergraph partitioner";
            homepage = "https://github.com/kahypar/mt-kahypar";
            license = pkgs.lib.licenses.mit;
            platforms = pkgs.lib.platforms.linux ++ [ "aarch64-darwin" ];
            mainProgram = "mt-kahypar";
          };
        };
    }
  );
}
