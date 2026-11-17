#!/bin/sh
# fix-senselapp.sh: make Sensel's SenselApp 0.19.32 open on current macOS.
#
# Sensel's last installer (SenselApp0.19.32.pkg) installs an app whose code signature
# lists six .nib files that are not in the package, so macOS says "SenselApp is damaged
# and can't be opened". This script takes the app out of Sensel's .pkg (it does NOT run
# the installer), copies it to ~/Applications, and re-signs it locally (ad hoc).
#
# Usage:
#   ./fix-senselapp.sh                       use SenselApp*.pkg from ~/Downloads
#   ./fix-senselapp.sh path/to/SenselApp0.19.32.pkg
#   ./fix-senselapp.sh --download            fetch the installer from the Internet Archive
#
# Options:
#   --dest DIR     install into DIR instead of ~/Applications (e.g. /Applications)
#   --force        replace an existing SenselApp.app (the old one is moved aside)
#   --skip-hash    accept an installer whose SHA-256 differs from the known 0.19.32 build
#                  (it must still be signed by Sensel, Inc.)
#   -h, --help     show this help
#
# Needs no administrator password for ~/Applications. On Apple Silicon, SenselApp runs
# under Rosetta 2. No Sensel files are included with this script.
set -eu

ARCHIVE_URL='https://web.archive.org/web/20240327074839if_/http://senselapp.com/download/mac.php'
KNOWN_SHA256='3f3654bd6eb96d8c5114f72f6596aa015b3e2217cdba179889d5e67b927f3797'   # SenselApp0.19.32.pkg
SENSEL_TEAM='Q3CDE494RG'                                                          # Sensel, Inc.

pkg=""
dest="$HOME/Applications"
force=0
download=0
skip_hash=0
while [ $# -gt 0 ]; do
  case "$1" in
    --dest) dest=${2:?--dest needs a directory}; shift 2 ;;
    --force) force=1; shift ;;
    --download) download=1; shift ;;
    --skip-hash) skip_hash=1; shift ;;
    -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    *) pkg=$1; shift ;;
  esac
done

say()  { printf '%s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = Darwin ] || fail "this script is for macOS"

# SenselApp is an Intel-only app; on Apple Silicon it needs Rosetta 2.
if [ "$(uname -m)" = arm64 ] && ! arch -x86_64 /usr/bin/true 2>/dev/null; then
  fail "Rosetta 2 is not installed. Install it with:
  softwareupdate --install-rosetta --agree-to-license
then run this script again."
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/fix-senselapp.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

# ---- locate or download the installer
if [ "$download" = 1 ]; then
  [ -z "$pkg" ] || fail "give either a .pkg path or --download, not both"
  pkg="$work/SenselApp0.19.32.pkg"
  say "Downloading SenselApp0.19.32.pkg (41 MB) from the Internet Archive…"
  curl -fL --progress-bar --retry 3 -o "$pkg" "$ARCHIVE_URL" || fail "download failed; try again later, or download it in a browser:
  $ARCHIVE_URL"
elif [ -z "$pkg" ]; then
  pkg=$(ls -t "$HOME"/Downloads/SenselApp*.pkg 2>/dev/null | head -1 || true)
  [ -n "$pkg" ] || fail "no SenselApp*.pkg found in ~/Downloads.
Pass the installer's path, or run:  $0 --download"
fi
[ -f "$pkg" ] || fail "not a file: $pkg"
say "Installer: $pkg"

# ---- verify it is Sensel's 0.19.32 installer
sum=$(shasum -a 256 "$pkg" | awk '{print $1}')
if [ "$sum" = "$KNOWN_SHA256" ]; then
  say "Checksum OK: this is Sensel's SenselApp 0.19.32 installer."
elif [ "$skip_hash" = 1 ]; then
  say "warning: checksum differs from the known 0.19.32 installer (continuing because of --skip-hash)."
else
  fail "this installer's SHA-256 ($sum) does not match SenselApp0.19.32.pkg.
If you are sure it is a genuine Sensel installer, rerun with --skip-hash."
fi

# ---- unpack (the installer is not run)
say "Unpacking (the installer itself is not run)…"
pkgutil --expand-full "$pkg" "$work/pkg" >/dev/null || fail "could not unpack $pkg"
app=$(find "$work/pkg" -type d -name 'SenselApp.app' -prune | head -1)
[ -n "$app" ] || fail "no SenselApp.app inside $pkg (is this Sensel's SenselApp installer?)"

id=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$app/Contents/Info.plist" 2>/dev/null || true)
[ "$id" = "unity.Sensel.SenselApp" ] || fail "unexpected bundle identifier '$id'; not Sensel's SenselApp"
codesign -dv "$app" 2>&1 | grep -q "TeamIdentifier=$SENSEL_TEAM" ||
  fail "the app in this package is not signed by Sensel, Inc.; refusing to re-sign it"
ver=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$app/Contents/Info.plist" 2>/dev/null || true)
say "Found ${ver:-SenselApp}, signed by Sensel, Inc."

# ---- install
target="$dest/SenselApp.app"
if pgrep -f "$target/Contents/MacOS/SenselApp" >/dev/null 2>&1; then
  fail "SenselApp is running from $target; quit it first"
fi
mkdir -p "$dest" 2>/dev/null || fail "cannot create $dest"
[ -w "$dest" ] || fail "no permission to write to $dest (use the default ~/Applications, or run as an administrator)"
if [ -e "$target" ]; then
  [ "$force" = 1 ] || fail "$target already exists; rerun with --force to replace it"
  old="$target.old-$(date +%Y%m%d-%H%M%S)"
  mv "$target" "$old" || fail "could not move the existing $target aside (if it was installed by Sensel's installer, drag it to the Trash first)"
  say "Moved the existing copy aside: $old"
fi

ditto "$app" "$target"
xattr -cr "$target"
codesign --force --deep --sign - "$target" >/dev/null 2>&1 || fail "re-signing failed"
codesign --verify --deep --strict "$target" || fail "signature check failed after re-signing"

say ""
say "Done. SenselApp is at: $target"
say "Open it from there (Spotlight finds it)."
say "Do not run Sensel's installer again: it would put the broken copy back in /Applications."
