{
  description = "eshetcpp";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.11";
  inputs.flake-utils.url = "github:numtide/flake-utils";

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let pkgs = nixpkgs.legacyPackages.${system}; in
      rec {
        packages.eshetcpp = pkgs.stdenv.mkDerivation {
          name = "eshetcpp";
          src = ./.;
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
          checkPhase = ''
            ./test/test_msgpack
            ./src/eshet --help
          '';
          doCheck = true;

          meta.mainProgram = "eshet";
        };
        packages.default = packages.eshetcpp;
      }
    );
}
