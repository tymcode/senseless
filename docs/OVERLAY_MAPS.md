# Overlay maps (firmware-side MIDI / MPE / HID)

The Morph's own firmware turns touches into USB-MIDI (including MPE), keyboard,
trackpad and gamepad output. Which output each region of the surface produces is set
by **maps** stored in flash, one per overlay type; the firmware picks the map for
whichever overlay is attached (detected magnetically). SenselApp was the only tool that
could write those maps. `senseless map-flash` replaces that function.

> ⚠️ **Status: not yet verified on hardware.** The format and procedure below are a
> faithful reimplementation of what SenselApp 0.19.32 did, and `senseless` refuses to
> write anything without `--yes`, always backs up first, and can restore or clear.
> Until someone confirms it on a real Morph, treat flashing as experimental.

## Flash area (register 0xC5, 17 408 bytes = 68 × 256-byte pages)

### Header (bytes 0–95)

```
00  CA FE FE ED            magic
04  00 44 00 00            0x44 = 68 pages
08  01 00 00 00
0C  slot entries, 4 bytes each: 00 <page> 07 08   (FF FF FF FF = empty)
```

| slot (header offset) | overlay | body page | body size | regions |
|---|---|---|---|---|
| 12 | QWERTY Keyboard | 4 | 1024 | 79 |
| 16 | Piano | 8 | 1024 | 38 |
| 20 | Music Production | 12 | 1024 | 47 |
| 24 | Drum Pad | 16 | 1024 | 21 |
| 28 | Art | 20 | 1024 | 20 |
| 32 | Gaming | 24 | 1024 | 23 |
| 36 | *Morph (device settings)* | 28 | — | 9 setting bytes |
| 40 | Video Editing | 32 | 1024 | 66 |
| 44 | Innovator's | 44 | 2048 | 96 |
| 48 | No Overlay | 40 | 1024 | 1 |
| 52 | Buchla Thunder | 52 | 4096 | 306 |

The body page is `slot − 8`, except Innovator's and Thunder, which use `slot`.

### Body

```
13 00 00 00                       map format version (19)
per region:
  v0..v6                          7 value bytes (widget type + parameters)
  l0..l2                          3 "LED" bytes (l2 = touch threshold)
  Innovator's only:
    4 × (int byte, frac byte)     region rectangle in mm, 8.8 fixed point
    s0, s4                        setting bytes 0 and 4
Generic overlays only:
  0D 0D <map label ASCII> 0D 0D   label = .senselmap file name without extension
zero padding to body size
```

### Writing sequence (as SenselApp did it)

1. Read the current header (and in senseless: back up all 17 408 bytes).
2. VS-write the body at offset `page << 8`.
3. VS-write the 96-byte header at offset 0, keeping other slots, with the first 12 bytes
   reset to the default prefix and this slot set to `00 <page> 07 08`.
4. Write `1` to register 0xC8 (FLASH_APP_RESET). The Morph reboots.

**Clear all maps:** VS-write 512 × `0xFF` at offset 0, then step 4.

## The `.senselmap` text format

```
SenselMap
<version>
<regions>
<value columns = 7>
<regions lines of 7 comma-separated bytes>        values
<regions lines>                                     values_original (ignored)
<overlay name>                                      e.g. "Buchla Thunder"
<map name>
<led rows>
<led cols = 3>
<led rows lines> × 2                                LED + LED_original
<position rows>
<position cols = 4>
<position rows lines of floats> × 2
<settings rows>
<settings cols = 5>
<settings rows lines>
<position rows lines of strings>                    version ≥ 17, display only
```

Maps from versions 16–19 compile directly. Older versions go through SenselApp's
migrations (`senselmap_upgrade`). Pre-0.10 binary (.NET `BinaryFormatter`) maps are not
supported.

## Region value bytes

`v0` is the widget type:

| v0 | widget |
|----|--------|
| 0 | keyboard |
| 1 | MIDI (notes, CCs, sliders, …) |
| 2 | gamepad |
| 3 | **MPE** |
| 4 | touchpad |
| 5 | setting |
| 6 | string |

For **MPE** regions (from SenselApp's MPE editor panel):

| byte | meaning |
|------|---------|
| v1 | sub-type (MPE note / XYZ) |
| v2 | MIDI note |
| v3 | X destination |
| v4 | Y destination |
| v5 | Z (pressure) destination |
| v6 | flags: bit0 14-bit, bit1 absolute X, bit2 absolute Y, bit3 **no** snap-back; bit7 + bits 4–6 = pitch-bend span |
| l2 | touch threshold (one of 0, 5, 12, 20, 40, 60, 80, 120, 160, 200, 250) |

Destination values: `0` off, `1–127` CC number, `128` pitch bend, `129` poly
aftertouch, `130` channel aftertouch.

Pitch-bend span index (bits 4–6 of v6): 0 = 460 mm, 1 = 230 mm, 2 = 130 mm,
3 = 920 mm, 4 = 1840 mm, 5 = no pitch bend. This is the finger travel for the
full bend range.

Other widget types' parameter bytes are not documented yet. The SenselApp editor panels
(`SenselApp*Event.cs` in a decompile) are the reference.

## Ready-made maps

Sensel's [sensel-controller-scripts](https://github.com/sensel/sensel-controller-scripts)
repository has MPE maps for Thunder, Piano, Music Production, Drum Pad and Innovator's
overlays (Bitwig and Ableton Live 11 folders). All of them parse and compile with
`senseless map-check`.
