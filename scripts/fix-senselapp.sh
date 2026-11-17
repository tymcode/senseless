#!/bin/sh
# fix-senselapp.sh: make Sensel's SenselApp 0.19.x open on current macOS.
#
# Sensel's installer package ships an app whose code signature lists six .nib files
# that are not in the package, so macOS reports "SenselApp is damaged and can't be
# opened". This script takes the app out of YOUR copy of Sensel's .pkg (it does not
# run the installer), copies it to ~/Applications, and re-signs it locally (ad hoc).
# No Sensel files are distributed with this script.
#
#   scripts/fix-senselapp.sh [path/to/SenselApp0.19.32.pkg] [--dest DIR] [--force]
#
# Defaults: the newest ~/Downloads/SenselApp*.pkg, installed into ~/Applications.
# Needs no administrator password. On Apple Silicon, SenselApp runs under Rosetta 2.
set -eu

pkg=""
dest="$HOME/Applications"
force=0
while [ $# -gt 0 ]; do
  case "$1" in
    --dest) dest=${2:?--dest needs a directory}; shift 2 ;;
    --force) force=1; shift ;;
    -h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) pkg=$1; shift ;;
  esac
done

say()  { printf '%s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

if [ -z "$pkg" ]; then
  pkg=$(ls -t "$HOME"/Downloads/SenselApp*.pkg 2>/dev/null | head -1 || true)
  [ -n "$pkg" ] || fail "no SenselApp*.pkg in ~/Downloads; pass the path to Sensel's installer"
fi
[ -f "$pkg" ] || fail "not a file: $pkg"

# Rosetta 2 (Apple Silicon only): SenselApp is an Intel-only app.
if [ "$(uname -m)" = arm64 ] && ! arch -x86_64 /usr/bin/true 2>/dev/null; then
  fail "Rosetta 2 is not installed. Run:  softwareupdate --install-rosetta --agree-to-license"
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/fix-senselapp.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

say "Unpacking $(basename "$pkg") (the installer is not run)…"
pkgutil --expand-full "$pkg" "$work/pkg" >/dev/null || fail "could not unpack $pkg"
app=$(find "$work/pkg" -type d -name 'SenselApp.app' -prune | head -1)
[ -n "$app" ] || fail "no SenselApp.app inside $pkg (is this Sensel's SenselApp installer?)"

id=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$app/Contents/Info.plist" 2>/dev/null || true)
ver=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$app/Contents/Info.plist" 2>/dev/null || true)
[ "$id" = "unity.Sensel.SenselApp" ] || fail "unexpected bundle identifier '$id'; not Sensel's SenselApp"
say "Found: $ver."

# Check that this really is Sensel's original build before re-signing it.
if codesign -dv "$app" 2>&1 | grep -q 'TeamIdentifier=Q3CDE494RG'; then
  say "Original signature: Sensel, Inc. (Q3CDE494RG)."
else
  fail "SenselApp in this package is not signed by Sensel, Inc.; refusing to re-sign it"
fi

target="$dest/SenselApp.app"
if pgrep -f "$target/Contents/MacOS/SenselApp" >/dev/null 2>&1; then
  fail "SenselApp is running from $target; quit it first"
fi
if [ -e "$target" ]; then
  [ "$force" = 1 ] || fail "$target already exists; rerun with --force to replace it"
  old="$target.old-$(date +%Y%m%d-%H%M%S)"
  mv "$target" "$old" || fail "could not move the existing $target aside"
  say "Moved the existing copy to $(basename "$old")."
fi

mkdir -p "$dest"
ditto "$app" "$target"
xattr -cr "$target"
codesign --force --deep --sign - "$target" >/dev/null 2>&1 || fail "re-signing failed"
codesign --verify --deep --strict "$target" || fail "signature check failed after re-signing"

say ""
say "Done: $target"
say "Open it from there (Spotlight finds it). Do not re-run Sensel's installer: it would"
say "install the broken copy into /Applications again."
say "Notes: senselapp.com is gone, so the app's update and firmware checks fail (ignore them)."
say "Quit other programs that use the Morph (e.g. senseless-mpe) while SenselApp is open."
