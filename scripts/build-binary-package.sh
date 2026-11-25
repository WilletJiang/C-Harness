#!/usr/bin/env bash
set -euo pipefail

source_root=$(cd "$(dirname "$0")/.." && pwd)
work=$(cd "$1" && pwd)
jobs=${AH_PACKAGE_JOBS:-4}
deps="$work/deps"
mkdir -p "$deps" "$work/dist"

fetch() {
    local url=$1 sha=$2 file=$3
    if [ ! -f "$file" ] || ! printf '%s  %s\n' "$sha" "$file" | shasum -a 256 -c - >/dev/null; then
        curl -fsSL --retry 4 --retry-all-errors --max-time 180 "$url" -o "$file"
    fi
    printf '%s  %s\n' "$sha" "$file" | shasum -a 256 -c -
}

common=()
case "$(uname -s)/$(uname -m)" in
    Darwin/arm64)
        system=macos-arm64
        openssl_target=darwin64-arm64-cc
        export MACOSX_DEPLOYMENT_TARGET=13.0
        common+=(-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_OSX_ARCHITECTURES=arm64)
        ;;
    Linux/x86_64)
        system=linux-x86_64
        openssl_target=linux-x86_64
        triplet=$(cc -print-multiarch)
        for name in sqlite3 tinfo z; do
            test -f "/usr/lib/$triplet/lib$name.a"
        done
        common+=("-DSQLite3_LIBRARY=/usr/lib/$triplet/libsqlite3.a"
                 "-DAH_TERMINFO_LIBRARY=/usr/lib/$triplet/libtinfo.a"
                 "-DZLIB_LIBRARY=/usr/lib/$triplet/libz.a"
                 -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none -DCURL_CA_FALLBACK=ON)
        ;;
    *) printf 'Unsupported build host\n' >&2; exit 1 ;;
esac

ossl="$deps/openssl"
if [ ! -f "$ossl/lib/libcrypto.a" ]; then
    fetch https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz \
        603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a "$deps/openssl.tar.gz"
    mkdir -p "$deps/openssl-src"
    tar -xzf "$deps/openssl.tar.gz" --strip-components=1 -C "$deps/openssl-src"
    (
        cd "$deps/openssl-src"
        ./Configure "$openssl_target" no-shared no-module no-tests no-docs \
            --prefix="$ossl" --libdir=lib --openssldir=/etc/ssl
        make -j "$jobs" build_libs
        make install_dev
    )
fi

ng="$deps/nghttp2"
if [ ! -f "$ng/lib/libnghttp2.a" ]; then
    fetch https://github.com/nghttp2/nghttp2/releases/download/v1.71.0/nghttp2-1.71.0.tar.gz \
        0cdd35ecec77919b4b7b245f87fe8d2216e3d62da854eb0838c594ff058042fc "$deps/nghttp2.tar.gz"
    mkdir -p "$deps/nghttp2-src"
    tar -xzf "$deps/nghttp2.tar.gz" --strip-components=1 -C "$deps/nghttp2-src"
    cmake -S "$deps/nghttp2-src" -B "$deps/nghttp2-build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$ng" -DCMAKE_INSTALL_LIBDIR=lib -DENABLE_LIB_ONLY=ON \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON "${common[@]}"
    cmake --build "$deps/nghttp2-build" --parallel "$jobs"
    cmake --install "$deps/nghttp2-build"
fi

utf="$deps/utf8proc"
if [ ! -f "$utf/lib/libutf8proc.a" ]; then
    fetch https://codeload.github.com/JuliaStrings/utf8proc/tar.gz/refs/tags/v2.11.3 \
        abfed50b6d4da51345713661370290f4f4747263ee73dc90356299dfc7990c78 "$deps/utf8proc.tar.gz"
    mkdir -p "$deps/utf8proc-src"
    tar -xzf "$deps/utf8proc.tar.gz" --strip-components=1 -C "$deps/utf8proc-src"
    cmake -S "$deps/utf8proc-src" -B "$deps/utf8proc-build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$utf" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF "${common[@]}"
    cmake --build "$deps/utf8proc-build" --parallel "$jobs"
    cmake --install "$deps/utf8proc-build"
fi
export PKG_CONFIG_PATH="$utf/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
for name in AH_CURL_SOURCE AH_MD4C_SOURCE; do
    override="FETCHCONTENT_SOURCE_DIR_$name"
    if [ -n "${!override:-}" ]; then
        common+=("-D$override=${!override}")
    fi
done

build="$work/build"
cmake -S "$source_root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DAH_BUILD_TUI=ON -DAH_BUNDLE_CURL=ON -DAH_REQUIRE_WEBSOCKETS=ON \
    -DAH_BUILD_EXAMPLES=OFF -DAH_TUI_PYTHON:STRING=python3 -DAH_TUI_HOST_ROOT= \
    "-DOPENSSL_ROOT_DIR=$ossl" -DOPENSSL_USE_STATIC_LIBS=ON \
    "-DOPENSSL_SSL_LIBRARY=$ossl/lib/libssl.a" "-DOPENSSL_CRYPTO_LIBRARY=$ossl/lib/libcrypto.a" \
    "-DNGHTTP2_LIBRARY=$ng/lib/libnghttp2.a" "-DNGHTTP2_INCLUDE_DIR=$ng/include" \
    -DNGHTTP2_USE_STATIC_LIBS=ON -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_LIBIDN2=OFF \
    "-Dpkgcfg_lib_AH_UTF8PROC_utf8proc=$utf/lib/libutf8proc.a" "${common[@]}"
cmake --build "$build" --parallel "$jobs" --target ah_cli ah_flow_vm

prefix="C-Harness-v0.11.0-$system"
staging=$(mktemp -d "$work/stage.XXXXXX")
package="$staging/$prefix"
mkdir -p "$package/bin" "$package/share/ah/host/packages/host" "$package/share/licenses"
install -m 755 "$build/ah" "$build/ah-flow-vm" "$package/bin/"
install -m 644 "$source_root"/packages/host/*.py "$source_root/packages/host/requirements.txt" \
    "$package/share/ah/host/packages/host/"
install -m 644 "$source_root/LICENSE" "$package/LICENSE"
install -m 644 "$source_root/scripts/binary-package-readme.md" "$package/README.md"
install -m 644 "$deps/openssl-src/LICENSE.txt" "$package/share/licenses/OpenSSL.txt"
install -m 644 "$deps/nghttp2-src/COPYING" "$package/share/licenses/nghttp2.txt"
install -m 644 "$deps/utf8proc-src/LICENSE.md" "$package/share/licenses/utf8proc.md"
if [ "$system" = linux-x86_64 ]; then
    install -m 644 /usr/share/doc/libncurses-dev/copyright "$package/share/licenses/ncurses.txt"
    install -m 644 /usr/share/doc/zlib1g-dev/copyright "$package/share/licenses/zlib.txt"
fi
install -m 644 "${FETCHCONTENT_SOURCE_DIR_AH_CURL_SOURCE:-$build/_deps/ah_curl_source-src}/COPYING" \
    "$package/share/licenses/libcurl.txt"
install -m 644 "${FETCHCONTENT_SOURCE_DIR_AH_MD4C_SOURCE:-$build/_deps/ah_md4c_source-src}/LICENSE.md" \
    "$package/share/licenses/md4c.md"
for dependency in quickjs yyjson tomlc17; do
    install -m 644 "$source_root/vendor/$dependency/LICENSE" "$package/share/licenses/$dependency.txt"
done
python3 -B "$source_root/scripts/binary-package-licenses.py" \
    "$source_root/packages/tui/render/math/Cargo.toml" "$package/share/licenses"

if [ "$system" = macos-arm64 ]; then
    otool -L "$package/bin/ah" "$package/bin/ah-flow-vm"
    if otool -L "$package/bin/ah" | grep -E '^[[:space:]]+(/opt/|/usr/local/|@rpath|@loader_path)'; then
        printf 'Executable links to dependencies outside macOS\n' >&2
        exit 1
    fi
    codesign --verify "$package/bin/ah" "$package/bin/ah-flow-vm"
else
    ldd "$package/bin/ah" "$package/bin/ah-flow-vm"
    readelf --version-info "$package/bin/ah" "$package/bin/ah-flow-vm" | grep -Eo 'GLIBC_[0-9.]+' | sort -Vu | tail -1
fi

COPYFILE_DISABLE=1 tar -czf "$work/dist/$prefix.tar.gz" -C "$staging" "$prefix"
unpacked=$(mktemp -d "$work/relocated.XXXXXX")
tar -xzf "$work/dist/$prefix.tar.gz" -C "$unpacked"
binary="$unpacked/$prefix/bin/ah"
"$binary" --help | grep -F 'ah 0.11.0'
"$binary" --help | grep -F 'Interactive terminal and account setup'
python3 -B "$source_root/tests/test_websocket.py" "$binary" --tui --bundled-curl
python3 -B "$source_root/tests/test_flow.py" "$binary"
python3 -B "$source_root/scripts/binary-package-host.py" "$binary"
if [ "$system" = linux-x86_64 ]; then
    python3 -B "$source_root/scripts/binary-package-wss.py" "$binary"
fi
shasum -a 256 "$work/dist/$prefix.tar.gz"
