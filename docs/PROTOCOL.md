# Sensel Morph wire protocol

Everything here was derived from Sensel's MIT-licensed LibSensel sources, the
decompiled SenselApp 0.19.32 (`Assembly-CSharp.dll`, a Unity/Mono app), and
disassembly of the closed `libSenselDecompress.dylib` from SenselLibMac 0.8.2.
See [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md) for method and confidence.

## Transport

The Morph is a USB composite device (vendor ID `0x2C2F`). Its interfaces are:

| Interface | macOS driver | Used for |
|-----------|--------------|----------|
| CDC-ACM serial | built-in `AppleUSBCDCACM` → `/dev/cu.usbmodem*` | the Sensel API (this document) |
| USB-MIDI | built-in class driver → "Sensel Morph" MIDI port | firmware-generated MIDI/MPE from overlay maps |
| HID | built-in | keyboard / trackpad / gamepad overlays |

**No kernel extension or DriverKit driver is needed or useful.** The "driver" is user
space code that speaks the register protocol below over the serial port. Baud rate is
irrelevant (CDC-ACM); LibSensel sets 115200 8N1 raw.

## Register protocol

All multi-byte values are little-endian. `0x01` is the board address; bit 7 = read.

### Fixed-size read

```
host → 81 <reg> <size>
dev  → <ack=01> <reg> <size:u16> <data[size]> <checksum>
```
`checksum = sum(data) & 0xFF`.

### Fixed-size write

```
host → 01 <reg> <size> <data[size]> <checksum>
dev  → <ack=05> <reg>
```

While scanning in async mode the device may send `PT_ASYNC_DATA` (`09`) frames before
the ack; LibSensel drains them.

### Ack codes

| code | meaning |
|------|---------|
| 1 / 2 | read ack / nack |
| 3 / 4 | VS read ack / nack |
| 5 / 6 | write ack / nack |
| 7 / 8 | VS write ack / nack |
| 9 | async frame data |
| 10 | buffered frame |

### Variable-size (VS) transfers

LibSensel only implements single-packet VS reads and offset-less VS writes. SenselApp
used the extended forms below to access flash; senseless implements them in
`senseless_ext.c`.

**Streaming VS read**:
```
host → 81 <reg> 00
dev  → <3 bytes ack>
loop:
  dev  → <len:u16, bit15 = more packets follow> <data[len & 0x7FFF]> <checksum>
  host → 01            (sent for every packet except a lone single-packet reply)
until bit15 == 0
```

**Paged VS read** (SenselApp used this for 33–600 byte reads of register 0xC5):
```
host → 81 <reg> 08 <chunk:u32 = 32> <offset:u32>
dev  → <3 bytes ack> <len:u16> <data> <checksum>
host → 01
```

**VS write with offset** (8-byte VS header):
```
host → 01 <reg> 00 08 <size:u32> <offset:u32> <sum(bytes 4..11)>
dev  → <ack> <reg>
for each chunk of ≤512 bytes:
  host → <len:u16> <data[len]> <checksum>
  dev  → <ack=07>
```
LibSensel's own `senselWriteRegVS` uses a 4-byte header (size only, no offset).

## Register map (selected)

Full list: `lib/sensel/src/sensel_register_map.h` (from LibSensel) plus the extra
registers SenselApp knew about:

| reg | name | notes |
|-----|------|-------|
| 0x00 | MAGIC | `"S3NS31"`, used to identify a Sensel device |
| 0x06–0x0E | FW_VERSION_*, DEVICE_ID, DEVICE_REVISION | |
| 0x0F | DEVICE_SERIAL_NUMBER | VS; 16 bytes, 13 valid, padding `0xFF` |
| 0x10 / 0x12 | SENSOR_NUM_COLS / ROWS | upsampled resolution (185 × 105 on Morph) |
| 0x14 / 0x18 | SENSOR_ACTIVE_AREA_WIDTH/HEIGHT_UM | 230 × 130 mm |
| 0x1C | COMPRESSION_METADATA | VS; see below |
| 0x20 | SCAN_FRAME_RATE | |
| 0x23 | SCAN_DETAIL_CONTROL | high/medium/low |
| 0x24 | FRAME_CONTENT_CONTROL | bitmask: 1 pressure, 2 labels, 4 contacts, 8 accel |
| 0x25 | SCAN_ENABLED | |
| 0x26 | SCAN_READ_FRAME | |
| 0x4B | CONTACTS_MASK | extra per-contact fields: 1 ellipse, 2 deltas, 4 bbox, 8 peak |
| 0x80–0x84 | LED_* | |
| 0xA0–0xA4 | UNIT_SHIFT_* | fixed-point shifts for dims/force/area/angle/time |
| 0xC0 | FLASH_HEADER | firmware update |
| 0xC4 | FLASH_APP | firmware image (encrypted `.sensel` files) |
| **0xC5** | **FLASH_OVERLAYS** | overlay-map storage, 17 408 bytes — see [OVERLAY_MAPS.md](OVERLAY_MAPS.md) |
| **0xC8** | **FLASH_APP_RESET** | write `1` to apply flashed data and reboot |
| 0xD0 | DEVICE_OPEN | |
| 0xE0 | SOFT_RESET | |
| 0xED | DFU_RESET | |

## Frames

`SCAN_READ_FRAME` returns `<payload_size:u16><payload>` where the payload is:

```
content_mask:u8  rolling_counter:u8  timestamp:u32
[contacts]   if mask & 0x04
[accel]      if mask & 0x08   (3 × int16)
[pressure]   if mask & 0x01
[labels]     if mask & 0x02
```

Contact parsing is in LibSensel (`_senselParseContactFrame`). Pressure and labels are
compressed; LibSensel left them to the closed `libSenselDecompress`, which senseless
now reimplements in `sensel_decompress.c`.

### COMPRESSION_METADATA (0x1C)

```
[0] version major  [1] version minor
[2] active cols    [3] active rows        Morph: 47 × 27
[4] col interp     [5] row interp         Morph: 4 × 4
```
Upsampled size = `(cols − 1) × interp + 1` → 185 × 105 on the Morph.

### Pressure payload

Varints: one byte, or if bit 7 is set, `(b0 & 0x7F) | (b1 << 7)` (15 bits).

```
base      = varint
state     = ZERO_RUN
while n < cols × rows:
  v = varint
  ZERO_RUN: emit v zeros; state = VALUES
  VALUES:   v > 0 → emit (v + base) / force_value_scale
            v = 0 → state = ZERO_RUN
```
`force_value_scale` = `1 << UNIT_SHIFT_FORCE` (from the device); results are grams.

### Labels payload

```
state = NULL_RUN
while n < cols × rows:
  run = varint
  NULL_RUN: emit run × 0xFF; state = LABELLED
  LABELLED: b = byte; emit run × (b & 0x7F); if !(b & 0x80) state = NULL_RUN
```

### Upsampling

Both images are upsampled with a separable 4-tap kernel. Tap weights for each of the
`interp + 1` sub-positions are computed at init by numerically convolving a
triangle (linear) basis with a smooth `(1 − x²)²` window, step 0.001 over [−1, 2], then
normalising to sum 1. Out-of-range neighbours are zero. Negative outputs clamp to 0.

Labels are upsampled by interpolating each label's masked force image separately and
taking, per output pixel, the label with the largest interpolated force above
`10 / 4095` g.

The senseless implementation reproduces Sensel's binary **bit-exactly** on 8 000
randomised frames across four metadata configurations (`tests/run_decompress_diff.sh`).
