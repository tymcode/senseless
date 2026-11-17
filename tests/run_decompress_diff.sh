#!/bin/sh
# Differential test: senseless's decompressor vs Sensel's original libSenselDecompress.
#
#   tests/run_decompress_diff.sh /path/to/SenselLibMac0.8.2.pkg   [iterations]
#   tests/run_decompress_diff.sh /path/to/libSenselDecompress.dylib [iterations]
#
# Sensel's library is x86_64-only, so on Apple Silicon this runs under Rosetta 2.
set -eu

src=${1:?usage: $0 <SenselLibMac.pkg | libSenselDecompress.dylib> [iterations]}
iters=${2:-2000}
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/senseless-diff.XXXXXX")
trap 'rm -rf "$work"' EXIT

case "$src" in
  *.pkg)
    pkgutil --expand-full "$src" "$work/pkg" >/dev/null
    ref=$(find "$work/pkg" -name libSenselDecompress.dylib | head -1)
    ;;
  *) ref=$src ;;
esac
[ -f "$ref" ] || { echo "libSenselDecompress.dylib not found in $src" >&2; exit 2; }

clang -arch x86_64 -O2 -ffp-contract=off -shared -fvisibility=hidden -DSENSEL_EXPORTS \
  -I"$root/lib/sensel/src" "$root/lib/sensel/src/sensel_decompress.c" -o "$work/candidate.dylib"
clang -arch x86_64 -O1 -I"$root/lib/sensel/src" "$root/tests/decompress_diff.c" -o "$work/diff"

"$work/diff" "$ref" "$work/candidate.dylib" "$iters"
