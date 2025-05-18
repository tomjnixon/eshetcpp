{
  description = "eshetcpp";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.11";
  inputs.flake-utils.url = "github:numtide/flake-utils";
  inputs.eshetsrv.url = "github:tomjnixon/eshetsrv";

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
      eshetsrv,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        eshetsrv_bin = "${eshetsrv.packages.${system}.eshetsrv}/bin/eshetsrv_release";
      in
      rec {
        packages.eshetcpp = pkgs.stdenv.mkDerivation {
          name = "eshetcpp";
          src = ./.;
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
          ];

          # TODO: make this a passthrough test?
          preCheck = ''
            RUNNER_LOG_DIR=$(pwd)/eshetsrv_logs ${eshetsrv_bin} daemon
          '';
          doCheck = true;

          meta.mainProgram = "eshet";
        };
        packages.default = packages.eshetcpp;

        devShells.eshetcpp = packages.eshetcpp.overrideAttrs (attrs: {
          nativeBuildInputs = attrs.nativeBuildInputs ++ [
            pkgs.clang-tools
            pkgs.nixfmt-rfc-style
          ];
        });
        devShells.default = devShells.eshetcpp;
      }
    );
}
