#!/usr/bin/env bash
# Builds the Rust libraries upstream Telegram Desktop links since v7.3.3,
# tlottie and wallet-engine, the way the "Rust libraries" step of upstream's
# .github/workflows/mac_packaged.yml does, into a prefix build_app.sh puts on
# CMAKE_PREFIX_PATH. That workflow is the packaged route this fork builds by;
# Telegram/build/prepare/prepare.py's tdesktop_rust stage is the same recipe
# for the static release build.
#
# Why one archive: a Rust staticlib carries its own copy of the standard
# library, so two of them define the runtime symbols twice and the link fails.
# Upstream builds both crates through one generated umbrella crate,
# tdesktop_rust, and cmake_helpers' packaged route looks for exactly that
# library first. With it found, tlottie and wallet_engine need only their
# headers: tlottie.h under include/tlottie, and wallet_engine.hpp with the
# generated wallet_engine.cpp beside it under include/wallet_engine.
#
# The workflow builds for the host only; this pins that to arm64, like the rest
# of the packaged build. The cdylib is built only because the C++ binding
# generator reads it; it is not installed, so find_library cannot pick it over
# the static archive.
#
#     purple/build_rust_deps.sh
#     RustWorkRoot=/Volumes/External/purple-deps/rust \
#         RustPrefix=/Volumes/External/purple-deps/tdesktop_rust \
#         purple/build_rust_deps.sh
#
# The toolchain, sources and cargo target live under RustWorkRoot, a few GB;
# the installed prefix is small. build_app.sh reads the same RustPrefix. The
# toolchain is the one the workflow pins, installed with its own RUSTUP_HOME and
# CARGO_HOME under RustWorkRoot, so ~/.rustup and ~/.cargo stay as they are.
# Re-running rebuilds the umbrella crate and reinstalls; clones are reused.
set -euo pipefail

RepoPath="$(cd "$(dirname "$0")/.." && pwd)"
LibrariesPath="${LibrariesPath:-$(dirname "$RepoPath")/tdesktop-libs}"

RustWorkRoot="${RustWorkRoot:-$LibrariesPath/rust-work}"
RustPrefix="${RustPrefix:-$LibrariesPath/local/tdesktop_rust}"

# Pinned to what the workflow pins in this tree (RUST_TOOLCHAIN, TLOTTIE,
# WALLET_ENGINE, RUST_PATCHES). The patches commit carries
# wallet-engine.patch.
RustToolchain="${RustToolchain:-1.96.1}"
TlottieCommit="${TlottieCommit:-92df98dc209bc39b1e567ec74a8c86a0af5239de}"
WalletEngineCommit="${WalletEngineCommit:-e59e0d89d7ee90c388bf36e3334c5b276f396697}"
PatchesCommit="${PatchesCommit:-aec474953ff7ee9b6e4cd9b8658288ea86d124f3}"
Target=aarch64-apple-darwin

Workflow="$RepoPath/.github/workflows/mac_packaged.yml"
for pin in "$RustToolchain" "$TlottieCommit" "$WalletEngineCommit" "$PatchesCommit"; do
    if ! command grep -q "\"$pin\"" "$Workflow"; then
        echo "mac_packaged.yml no longer pins $pin; update this script first." >&2
        exit 1
    fi
done

# The same target build_app.sh and build_deps.sh build for.
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-13}"
export RUSTUP_HOME="$RustWorkRoot/rustup"
export CARGO_HOME="$RustWorkRoot/cargo"
export RUSTUP_TOOLCHAIN="$RustToolchain"
export CARGO_TARGET_DIR="$RustWorkRoot/target"
mkdir -p "$RustWorkRoot" "$RUSTUP_HOME" "$CARGO_HOME"

# ---------------------------------------------------------------- toolchain

# The rustup already on PATH manages a second home here. CARGO_HOME/bin is
# not populated by toolchain installs, so the proxies stay the ones on PATH,
# which honour RUSTUP_HOME and RUSTUP_TOOLCHAIN.
if ! command -v rustup >/dev/null; then
    echo "rustup is needed on PATH (https://rustup.rs)." >&2
    exit 1
fi
rustup toolchain install "$RustToolchain" --profile minimal --target "$Target"
echo "rustc: $(rustc --version)"

# ---------------------------------------------------------------- sources

# One commit each, fetched by id as the workflow does; LFS objects are skipped
# there too.
checkout() {
    name="$1"; url="$2"; commit="$3"
    if [ ! -d "$RustWorkRoot/$name/.git" ]; then
        git init --quiet "$RustWorkRoot/$name"
        git -C "$RustWorkRoot/$name" remote add origin "$url"
    fi
    if [ "$(git -C "$RustWorkRoot/$name" rev-parse --verify --quiet HEAD)" != "$commit" ]; then
        echo "=== fetching $name ==="
        GIT_LFS_SKIP_SMUDGE=1 git -C "$RustWorkRoot/$name" fetch --depth=1 origin "$commit"
    fi
    GIT_LFS_SKIP_SMUDGE=1 git -C "$RustWorkRoot/$name" reset --quiet --hard "$commit"
    echo "$name at $(git -C "$RustWorkRoot/$name" rev-parse HEAD)"
}

checkout rust-patches https://github.com/desktop-app/patches.git "$PatchesCommit"
checkout tlottie https://github.com/dkaraush/tlottie.git "$TlottieCommit"
checkout wallet-engine https://github.com/i582/wallet-engine.git "$WalletEngineCommit"

# The workflow applies this patch on a fresh clone; the hard reset above
# undoes an earlier run's copy first, so it always applies to clean sources.
git -C "$RustWorkRoot/wallet-engine" clean -fdq
git -C "$RustWorkRoot/wallet-engine" apply "$RustWorkRoot/rust-patches/wallet-engine.patch"

# ---------------------------------------------------------------- build

Umbrella="$RustWorkRoot/tdesktop_rust"
rm -rf "$Umbrella"
(cd "$RustWorkRoot" && cargo new --lib --vcs none tdesktop_rust)
printf 'pub use ::tlottie;\npub use ::wallet_engine;\n' > "$Umbrella/src/lib.rs"
(cd "$Umbrella" \
    && cargo add --path ../tlottie --features c-api \
    && cargo add --path ../wallet-engine)

(cd "$Umbrella" && nice -n 15 cargo rustc --lib --release \
    --crate-type staticlib --crate-type cdylib \
    --target "$Target" \
    --config "profile.release.opt-level='z'" \
    --config "profile.release.lto='thin'" \
    --config "profile.release.codegen-units=1" \
    --config "profile.release.panic='unwind'" \
    --config "profile.release.package.tlottie.opt-level=3" \
    -- --print native-static-libs)

Release="$CARGO_TARGET_DIR/$Target/release"

# ---------------------------------------------------------------- install

rm -rf "$RustPrefix"
mkdir -p "$RustPrefix/lib" "$RustPrefix/include/tlottie" \
    "$RustPrefix/include/wallet_engine"
cp "$Release/libtdesktop_rust.a" "$RustPrefix/lib/"
cp "$RustWorkRoot/tlottie/include/tlottie.h" "$RustPrefix/include/tlottie/"
(cd "$Umbrella" && cargo run \
    --manifest-path ../wallet-engine/bindgen/cpp/bindgen/Cargo.toml --locked -- \
    --library --out-dir "$RustPrefix/include/wallet_engine" \
    "$Release/libtdesktop_rust.dylib")

# Apple clang 15 (Xcode 15.1) lacks class template argument deduction for
# aggregates (P1816, clang 17), which the generated binding relies on for
# RustFutureHandleGuard. One explicit deduction guide names the same type.
python3 - "$RustPrefix/include/wallet_engine/wallet_engine.cpp" <<'PY'
import sys
path = sys.argv[1]
text = open(path).read()
guide = ('template <typename F>\n'
    'RustFutureHandleGuard(uint64_t, F) -> RustFutureHandleGuard<F>;\n')
anchor = '    ~RustFutureHandleGuard() {\n        free(handle);\n    }\n};\n'
if guide not in text:
    if text.count(anchor) != 1:
        sys.exit('wallet_engine.cpp: RustFutureHandleGuard changed; recheck the clang 15 guide')
    text = text.replace(anchor, anchor + guide)
    open(path, 'w').write(text)
PY

# The packaged route compiles wallet_engine.cpp from the header's directory,
# where the generator writes it.
for file in lib/libtdesktop_rust.a include/tlottie/tlottie.h \
        include/wallet_engine/wallet_engine.hpp \
        include/wallet_engine/wallet_engine.cpp; do
    if [ ! -f "$RustPrefix/$file" ]; then
        echo "missing $RustPrefix/$file" >&2
        exit 1
    fi
done
printf 'toolchain %s\ntlottie %s\nwallet-engine %s\npatches %s\n' \
    "$RustToolchain" "$(git -C "$RustWorkRoot/tlottie" rev-parse HEAD)" \
    "$WalletEngineCommit" "$PatchesCommit" > "$RustPrefix/purple-pins.txt"
lipo -info "$RustPrefix/lib/libtdesktop_rust.a"
du -sh "$RustPrefix"
echo "=== installed into $RustPrefix ==="
