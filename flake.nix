{
  description = "whisper - Encrypted DM pipe for Nostr (NIP-17 + NIP-44)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    libnostr-c = {
      url = "github:privkeyio/libnostr-c";
      flake = false;
    };
    noscrypt = {
      url = "github:privkeyio/noscrypt";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, libnostr-c, noscrypt }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        noscryptLib = pkgs.stdenv.mkDerivation {
          pname = "noscrypt";
          version = "0.1.0";
          src = noscrypt;

          nativeBuildInputs = [ pkgs.cmake ];
          buildInputs = [ pkgs.openssl pkgs.secp256k1 ];

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
            "-DBUILD_SHARED_LIBS=ON"
          ];
        };

        libnostrC = pkgs.stdenv.mkDerivation {
          pname = "libnostr-c";
          version = "0.1.4";
          src = libnostr-c;

          nativeBuildInputs = [ pkgs.cmake pkgs.pkg-config ];
          buildInputs = [
            pkgs.cjson
            pkgs.secp256k1
            pkgs.libwebsockets
            pkgs.openssl
            noscryptLib
          ];

          # Patch cmake for Nix build
          preConfigure = ''
            # Add noscrypt include path (headers are in include/noscrypt/)
            sed -i '2i include_directories(${noscryptLib}/include/noscrypt)' CMakeLists.txt
            # Skip pkg-config check for noscrypt
            sed -i 's/pkg_check_modules(NOSCRYPT noscrypt)/# pkg_check_modules disabled for Nix/' CMakeLists.txt
            # Install generated nostr_features.h header
            printf '\ninstall(FILES ''${CMAKE_CURRENT_BINARY_DIR}/include/nostr_features.h DESTINATION include)\n' >> CMakeLists.txt
          '';

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
            "-DNOSTR_FEATURE_NIP17=ON"
            "-DNOSTR_FEATURE_NIP44=ON"
            "-DNOSTR_FEATURE_NIP59=ON"
            "-DNOSTR_FEATURE_RELAY=ON"
            "-DBUILD_EXAMPLES=OFF"
            "-DBUILD_TESTS=OFF"
            # Manually specify noscrypt since there's no pkg-config file
            "-DNOSCRYPT_FOUND=ON"
            "-DNOSCRYPT_LIBRARIES=${noscryptLib}/lib/libnoscrypt.so"
          ];
        };

        whisper = pkgs.stdenv.mkDerivation {
          pname = "whisper";
          version = "0.1.0";
          src = ./.;

          buildInputs = [
            libnostrC
            noscryptLib
            pkgs.cjson
            pkgs.secp256k1
            pkgs.libwebsockets
            pkgs.openssl
          ];

          buildPhase = ''
            $CC -Wall -Wextra -O2 -std=c99 -D_DEFAULT_SOURCE \
              -I${libnostrC}/include \
              -c -o main.o main.c
            $CC -Wall -Wextra -O2 -std=c99 -D_DEFAULT_SOURCE \
              -I${libnostrC}/include \
              -c -o send.o send.c
            $CC -Wall -Wextra -O2 -std=c99 -D_DEFAULT_SOURCE \
              -I${libnostrC}/include \
              -c -o recv.o recv.c
            $CC -Wall -Wextra -O2 -std=c99 -D_DEFAULT_SOURCE \
              -I${libnostrC}/include \
              -c -o util.o util.c
            $CC -o whisper main.o send.o recv.o util.o \
              -L${libnostrC}/lib -lnostr \
              -L${noscryptLib}/lib -lnoscrypt \
              -lcjson -lsecp256k1 -lwebsockets -lssl -lcrypto -lpthread -lm
          '';

          installPhase = ''
            mkdir -p $out/bin
            cp whisper $out/bin/
          '';

          meta = with pkgs.lib; {
            description = "Encrypted DM pipe for Nostr (NIP-17 + NIP-44)";
            homepage = "https://github.com/privkeyio/whisper";
            license = licenses.mit;
            platforms = platforms.unix;
          };
        };

      in {
        packages = {
          default = whisper;
          whisper = whisper;
          libnostr-c = libnostrC;
          noscrypt = noscryptLib;
        };

        devShells.default = pkgs.mkShell {
          buildInputs = [
            libnostrC
            noscryptLib
            pkgs.cjson
            pkgs.secp256k1
            pkgs.libwebsockets
            pkgs.openssl
            pkgs.pkg-config
          ];

          shellHook = ''
            echo "whisper dev shell"
            echo "Run: make"
          '';
        };

        apps.default = {
          type = "app";
          program = "${whisper}/bin/whisper";
        };
      }
    );
}
