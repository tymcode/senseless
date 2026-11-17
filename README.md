# senseless

Modern macOS support for the **Sensel Morph**, an abandoned pressure-sensitive multitouch
controller. Native on Apple Silicon, with no kernel extensions.

- **`libSensel.dylib`**: a universal (arm64 + x86_64) build of Sensel's API, **with force
  images**. Sensel only ever shipped force decompression as a closed Intel binary; it's
  reimplemented here and verified bit-exact against the original. It's a drop-in for
  Sensel's `/usr/local/lib/libSensel.dylib`, so existing programs, the Python wrapper and
  the C examples work unchanged.
- **`senseless-mpe`**: turns the Morph into an MPE controller on a CoreMIDI virtual port,
  with any overlay or none.
- **`senseless`**: device info, contact/force streaming, and **overlay-map flashing**, the
  part of SenselApp you need to put MPE maps on the Morph's own firmware.

## Do you even need a driver?

No. The Morph is a USB composite device whose interfaces all use drivers built into macOS:

| Interface | What it's for | On a modern Mac |
|---|---|---|
| USB-MIDI | firmware-generated MIDI/MPE from overlay maps | works out of the box |
| HID | keyboard/trackpad overlays | works out of the box |
| CDC serial | the Sensel API (raw contacts and forces) | `/dev/cu.usbmodem*`, needs this library |

What *was* missing:
1. an Apple Silicon build of the API, including the closed force-decompression library;
2. a way to edit and flash overlay maps without SenselApp, an Intel-only Unity app whose
   firmware server is gone.

senseless fills both gaps.

## Two ways to get MPE

**A. Host-side MPE (`senseless-mpe`).** This is the recommended starting point. The library
reads every contact (position in mm, force in grams) and `senseless-mpe` produces
MPE on a virtual MIDI source called **"Senseless Morph"**:

- each finger gets its own member channel (lower zone, channels 2–16);
- X becomes per-note pitch bend, Y becomes CC74, and force becomes channel pressure;
- the surface is an isomorphic grid (semitones across, fourths up; configurable);
- it sends an MPE Configuration Message and pitch-bend ranges on connect.

It's independent of what's flashed on the device and can be changed with a command-line flag.

**B. Firmware MPE (overlay maps).** The Morph firmware can emit MPE directly over its
USB-MIDI port, with no host software running. Use this with physical overlays like the
Buchla Thunder. Sensel published MPE maps for Thunder, Piano, Music Production and
Drum Pad; flash one with `senseless map-flash`. Maps already on your Morph keep working
today without any software.

## Build

Requires the Xcode command-line tools and CMake.

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build                        # unit tests
sudo cmake --install build                    # → /usr/local/{lib,include,bin}
```

The install puts `libSensel.dylib` and a `libSenselDecompress.dylib` compatibility shim
where Sensel's installer put them, so software built against the old SDK finds them.
Use `-DCMAKE_INSTALL_PREFIX=…` to install elsewhere.

## Use

```sh
senseless list                    # find devices
senseless info                    # firmware, sensor size, capabilities
senseless contacts                # stream contacts
senseless forces --map            # live ASCII force image (uses the open decompressor)

senseless-mpe                     # start MPE; pick "Senseless Morph" as input in your DAW
senseless-mpe --demo              # no Morph needed: plays a test gesture to check DAW setup
senseless-mpe --row-interval 7 --base-note 48 --bend-range 24 -v
senseless-mpe --help              # all options
```

In the DAW, enable MPE for the "Senseless Morph" input:
- Bitwig: add it as a generic MIDI keyboard with MPE on.
- Ableton Live 11+: set MPE in the input's settings.
- Logic: enable MPE on the instrument.

The bend range must match the synth (default ±48 semitones, per the MPE spec).

### Overlay maps

```sh
senseless overlays                          # what's stored on the Morph now
senseless backup my-morph.bin               # save all 17 KB of map flash
senseless map-check path/to/*.senselmap     # parse + compile offline, no device
senseless map-flash "ThunderBitwigMPE.senselmap" --yes
senseless restore my-morph.bin --yes        # undo
senseless map-clear --yes                   # back to factory behaviour
```

`map-flash` always writes a timestamped backup first. Format details are in
[docs/OVERLAY_MAPS.md](docs/OVERLAY_MAPS.md).

### SenselApp says "damaged and can't be opened"

Sensel's last installer ships an app with a broken code signature, so current macOS
refuses to open it. [`senselapp-fix/`](senselapp-fix/) has a standalone script that
repairs it without running the installer and without an admin password. It can also
download the installer from the Internet Archive:

```sh
sh senselapp-fix/fix-senselapp.sh --download
```

See [senselapp-fix/README.md](senselapp-fix/README.md) for details and for what does and
doesn't work in SenselApp. In short: Visualizer and device info work; updates don't;
Thunder taps don't light up in the app, though the Morph still sends their MIDI.

### Python

`wrappers/python/sensel.py` is Sensel's wrapper, adjusted to also find the library via
`$SENSEL_LIB` or `/opt/homebrew/lib`. Sensel's examples are alongside it.

## Status

| Component | State |
|---|---|
| Force/label decompression | ✅ bit-exact vs Sensel's binary (8 000 randomised frames) |
| `libSensel` universal build + ABI drop-in | ✅ old-SDK binary runs on it |
| CoreMIDI / MPE output | ✅ verified with `--demo` and a MIDI listener |
| Serial/IOKit backend on a real Morph | ✅ tested on firmware 0.19.216 |
| `senseless-mpe` with a real Morph | ✅ works; velocity/pressure curves tuned from recorded playing |
| Overlay map flashing | ✅ flashed a No Overlay map; read back byte-exact, other slots untouched, map active after reboot |
| Firmware updates | ❌ firmware was only hosted on senselapp.com (now 404) |
| Map *editor* GUI | ❌ not started; maps are edited as text or taken from Sensel's repo |

### First test with a Morph

1. `senseless list` should show one device with a serial number.
2. `senseless info` should report 230 × 130 mm and 185 × 105 elements.
3. `senseless contacts`: touch it and you should see start/move/end events with forces in grams.
4. `senseless forces --map`: check that the force image looks right; this exercises the
   new decompressor end to end.
5. `senseless-mpe -v`, then play. Adjust `--velocity-min/--velocity-max` (log scale, defaults 5 g / 1 kg)
   and `--pressure-max` (default 1 kg) to taste.
6. Run `senseless backup` **before** trying `map-flash`.

**Firmware output while the API scans** (tested on hardware, firmware 0.19.216):
- **MIDI:** the firmware's own USB-MIDI output stops within about 0.1–0.4 s of the API
  starting a scan, and resumes when scanning stops. Firmware MPE and `senseless-mpe`
  therefore never double up.
- **HID (trackpad/keyboard maps):** not settled. With the trackpad map, the pointer
  seemed to keep moving during a scan, with brief pauses. If it does, flash a MIDI-only
  No Overlay map so the bare surface stops driving the pointer. Sensel's
  `No Overlay - Multitouch v1.2.senselmap` turns the whole surface into one firmware MPE
  region.

## Layout

```
lib/sensel/src/        LibSensel (Sensel, MIT) + senseless additions:
  sensel_decompress.c    open force/label decompression
  sensel_serial_mac.c    IOKit-based macOS serial backend
  senseless_ext.c        offset VS writes / streaming VS reads (flash access)
tools/                 senseless CLI, senseless-mpe, .senselmap compiler
examples/              Sensel's C examples (unchanged)
wrappers/python/       Sensel's Python wrapper + examples
senselapp-fix/         standalone fix for SenselApp 0.19.32 on current macOS
tests/                 decompression self-check + differential test vs Sensel's binary
docs/                  protocol, overlay maps, reverse-engineering notes
```

## Credits and licence

LibSensel and its examples are © Sensel, Inc. under the MIT licence
(`lib/sensel/LICENSE.sensel`). Everything else is MIT (`LICENSE`). Sensel's binaries,
firmware and map files are not redistributed. See
[docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md) for how the closed parts were
reimplemented.
