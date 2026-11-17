# How this was worked out

## Sources

| Source | What it gave us |
|--------|-----------------|
| `sensel/sensel-api` (MIT) | LibSensel C sources: register protocol, contacts, device enumeration |
| `SenselLibMac0.8.2.pkg` | `libSensel.dylib`, `libSenselDecompress.dylib` (x86_64 only), headers |
| `SenselApp0.19.32.pkg` | Unity 2018 / Mono app; `Contents/Resources/Data/Managed/Assembly-CSharp.dll` holds all app logic in IL |
| `sensel/sensel-controller-scripts` | Real `.senselmap` files (incl. MPE maps) used as test inputs |

## Steps

1. **Expand the packages:** `pkgutil --expand-full <pkg> <dir>`.
2. **Decompile SenselApp:** `ilspycmd -p -o out -r Managed Managed/Assembly-CSharp.dll`
   (ILSpy CLI; needs `DOTNET_ROLL_FORWARD=Major` on newer .NET). This gives readable C#
   for `Sensel/SenselDevice.cs` (its own serial protocol stack), `Sensel/SenselDecompress.cs`
   (a managed port of the force upsampler), `SenselApp/SenselAppMapper.cs` (map
   flashing), `SenselAppMap.cs` (`.senselmap` parser), and the per-widget editors.
3. **Disassemble `libSenselDecompress.dylib`:** `otool -tV`. It's small (~10 KB), unstripped
   (`__senselDecompressForces`, `__senselDecompressLabels`, `_upsamplePressureMap2`, …),
   and built without optimisation, so it maps almost line-for-line onto the C# port.
   Two details came only from the disassembly:
   - forces are divided by the device's `force_value_scale` (struct offset 0x44); the C#
     instead hard-codes `/16` in the upsampler;
   - the label decoder's run/continuation encoding (the C# app never decoded labels).
4. **Reimplement** in C (`lib/sensel/src/sensel_decompress.c`). No decompiled code was
   copied; the implementation follows the documented behaviour.
5. **Verify** by differential testing against the original binary (below).

## Verification

`tests/run_decompress_diff.sh <path to SenselLibMac pkg payload>` builds an x86_64 harness
that `dlopen`s both Sensel's `libSenselDecompress.dylib` and ours. It feeds identical
randomised compressed frames (pressure only, labels only, both; four metadata
configurations, random base offsets and force scales) to each and compares the output:

```
frames compared: 8000  status/length mismatches: 0
force max |diff| = 0  max rel diff = 0
label pixel mismatches: 0
MATCH
```

ABI drop-in was checked by linking Sensel's example 3 against the **original**
`/usr/local/lib/libSensel.dylib` (x86_64) and running it under Rosetta with
`DYLD_LIBRARY_PATH` pointing at the senseless build: it loads and runs on our library.

## Known deliberate differences from Sensel's binaries

- **Labels-only frames:** Sensel zeroed only the first `cols × rows` entries of the
  upsampled force array and left the rest stale. We zero all of it. As in the original,
  label upsampling then uses the most recent coarse pressure image.
- **Bounds checks** were added throughout the decoders (the originals trusted the device).
- **Device discovery** uses IOKit by vendor ID instead of probing every `usbmodem` node.

## Confidence

| Area | Confidence | Basis |
|------|------------|-------|
| Force/label decompression | **verified** | bit-exact vs original binary |
| LibSensel ABI compatibility | **verified** | old binary runs on new library |
| Contacts / register protocol | high | unchanged upstream MIT code |
| macOS serial backend | high, **needs hardware test** | standard CDC-ACM usage |
| MPE output (`senseless-mpe`) | high, **needs hardware test** | MIDI path verified with `--demo` |
| Overlay map flashing | medium, **needs hardware test** | direct port of SenselApp logic |
| Firmware update | not implemented | firmware files were only on senselapp.com, which now returns 404 |

## Legal note

LibSensel is MIT-licensed by Sensel, Inc.; its notices are kept in the vendored files.
The decompression and flash-map code was reimplemented independently, for
interoperability with hardware the user owns, after the vendor discontinued support.
Sensel's binaries, firmware and `.senselmap` files are **not** redistributed here.
