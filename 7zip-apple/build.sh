#!/usr/bin/env bash
# Builds lib7zip_apple.a for arm64 and x86_64, joins them into a universal
# archive and runs the round-trip test against each slice.
# Output: 7zip-apple/build/universal/lib7zip_apple.a
# Usage: 7zip-apple/build.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD="${SCRIPT_DIR}/build"
BUNDLE="${REPO_ROOT}/CPP/7zip/Bundles/Alone2"
JOBS="$(sysctl -n hw.ncpu)"

export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-13.0}"

if [[ ! -s "${REPO_ROOT}/VERSION" ]]; then
    echo "ERROR: ${REPO_ROOT}/VERSION is missing" >&2
    exit 1
fi

# 7-Zip's makefile ARCH and the matching clang -arch for each slice.
SLICES=("arm64:arm64" "x64:x86_64")

for slice in "${SLICES[@]}"; do
    arch="${slice%%:*}"
    echo "▸ Building ${arch}"
    make -s -j"${JOBS}" -C "${BUNDLE}" -f "${SCRIPT_DIR}/makefile.gcc" ARCH="${arch}" O="${BUILD}/${arch}"
done

mkdir -p "${BUILD}/universal"
lipo -create "${BUILD}/arm64/lib7zip_apple.a" "${BUILD}/x64/lib7zip_apple.a" \
    -output "${BUILD}/universal/lib7zip_apple.a"
lipo -info "${BUILD}/universal/lib7zip_apple.a"

# The library must export its C API and nothing of 7-Zip itself.
LEAKED="$(nm -gU "${BUILD}/universal/lib7zip_apple.a" 2>/dev/null | awk 'NF == 3 {print $3}' | grep -v '^_sevenzip_' || true)"
if [[ -n "$LEAKED" ]]; then
    echo "ERROR: symbols outside the C API are exported:" >&2
    echo "$LEAKED" >&2
    exit 1
fi

for slice in "${SLICES[@]}"; do
    clang_arch="${slice##*:}"
    test_bin="${BUILD}/roundtrip-${clang_arch}"
    clang -arch "${clang_arch}" -Wall -Wextra -Werror \
        -I"${SCRIPT_DIR}/include" "${SCRIPT_DIR}/test/roundtrip.c" \
        "${BUILD}/universal/lib7zip_apple.a" -lc++ -framework CoreFoundation -o "${test_bin}"
    if arch "-${clang_arch}" /usr/bin/true 2>/dev/null; then
        echo "▸ Testing ${clang_arch}"
        TMPDIR="${BUILD}" arch "-${clang_arch}" "${test_bin}"
    else
        echo "▸ Skipping the ${clang_arch} test: this machine cannot run ${clang_arch}"
    fi
done
