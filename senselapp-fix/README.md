# Fix "SenselApp is damaged and can't be opened"

SenselApp, the configuration app for the Sensel Morph, no longer opens on current
versions of macOS. Instead you get:

> **"SenselApp" is damaged and can't be opened. You should move it to the Trash.**

The app isn't actually damaged. Sensel's last installer (`SenselApp0.19.32.pkg`, from
2019) installs an app whose code signature lists six small interface files (`.nib`) that
aren't in the package. Recent macOS checks every file a signature lists, finds them
missing, and refuses to open the app.

`fix-senselapp.sh` repairs it in about a minute:

1. It takes SenselApp out of Sensel's installer package. The installer itself is not run.
2. It checks that the package is the genuine 0.19.32 installer (by SHA-256) and that the
   app is signed by Sensel, Inc.
3. It copies the app to the **Applications folder in your home folder**
   (`~/Applications`), so no administrator password is needed.
4. It re-signs the app on your Mac so macOS will open it.

No Sensel software is included here. The script uses your copy of Sensel's installer,
or downloads it from the Internet Archive.

## Requirements

- macOS on Apple Silicon or Intel.
- **Rosetta 2** on Apple Silicon Macs, because SenselApp is an Intel-only app. If it's
  missing, the script says so. To install it, open Terminal and run:
  ```sh
  softwareupdate --install-rosetta --agree-to-license
  ```

## How to use it

1. **Remove the broken app** if you already ran Sensel's installer: drag
   `/Applications/SenselApp.app` to the Trash. Finder may ask for your password.
2. **Open Terminal** (Applications → Utilities → Terminal) and download the script into
   your Downloads folder:
   ```sh
   curl -fsSL -o ~/Downloads/fix-senselapp.sh https://raw.githubusercontent.com/tymcode/senselapp-fix/main/fix-senselapp.sh
   ```
   You can also download it from this page (`fix-senselapp.sh` → download button) and
   save it to Downloads.
3. **Run the script.**

   If you already have `SenselApp0.19.32.pkg` in your Downloads folder:
   ```sh
   sh ~/Downloads/fix-senselapp.sh
   ```
   If you don't, let the script download it from the Internet Archive:
   ```sh
   sh ~/Downloads/fix-senselapp.sh --download
   ```
   If your installer is somewhere else, give its path:
   ```sh
   sh ~/Downloads/fix-senselapp.sh /path/to/SenselApp0.19.32.pkg
   ```
4. **Open SenselApp** from the Applications folder in your home folder
   (Finder → Go → Home → Applications), or with Spotlight.

You should see something like:

```
Installer: /Users/you/Downloads/SenselApp0.19.32.pkg
Checksum OK: this is Sensel's SenselApp 0.19.32 installer.
Unpacking (the installer itself is not run)…
Found SenselApp version 0.19.32, signed by Sensel, Inc.

Done. SenselApp is at: /Users/you/Applications/SenselApp.app
```

**Don't run Sensel's installer again afterwards.** It would install the broken copy
into `/Applications` again.

### Options

| Option | What it does |
|---|---|
| `--download` | Download the installer from the Internet Archive instead of using a local file |
| `--dest DIR` | Install somewhere other than `~/Applications`, e.g. `--dest /Applications` (you need permission to write there) |
| `--force` | Replace an existing copy. The old one is renamed (`SenselApp.app.old-<date>`), not deleted |
| `--skip-hash` | Accept an installer that isn't byte-identical to the known 0.19.32 package. It must still be signed by Sensel |
| `--help` | Show usage |

## What works in SenselApp after the fix

Tested on macOS 26 with a Morph on firmware 0.19.216:

- connecting to the Morph and showing firmware and device information;
- the Visualizer;
- viewing overlays and maps, with taps highlighted on keyboard-style overlays (e.g. Video
  Editing / Premiere).

Limitations of SenselApp itself, not of the fix:

- **Updates don't work.** senselapp.com is gone, so the app's update and firmware checks
  fail. Ignore those prompts. Firmware updates are no longer possible through SenselApp.
- **Thunder (and other MIDI) overlays don't light up when you tap them.** SenselApp
  highlights MIDI buttons only by listening to the Morph's MIDI output, and the Mac build
  of 0.19.32 shipped with that MIDI input disabled. The Morph itself still sends MIDI
  normally; check with your DAW's MIDI monitor.
- **One program at a time.** While SenselApp is open, other software can't use the
  Morph's API connection, and the Morph pauses its own MIDI output. Quit SenselApp before
  playing.

## Is this safe?

- The script never runs Sensel's installer and never asks for your password.
- It refuses to continue unless the package matches the SHA-256 of Sensel's 0.19.32
  installer (`3f3654bd6eb96d8c5114f72f6596aa015b3e2217cdba179889d5e67b927f3797`) and the
  app inside is signed by Sensel, Inc. (team `Q3CDE494RG`).
- It only writes `SenselApp.app` into the destination folder, plus a temporary folder that
  it deletes afterwards.
- The local "ad hoc" signature replaces Sensel's expired one, so macOS shows the app as
  coming from you rather than from Sensel. To undo the fix, delete `~/Applications/SenselApp.app`.

The script is short; please read it before running it.

## Where the installer comes from

senselapp.com is offline. The script downloads the last macOS installer from the
Internet Archive's copy of Sensel's download link:

<https://web.archive.org/web/20240327074839/http://senselapp.com/download/mac.php>

That file is byte-identical to the original `SenselApp0.19.32.pkg` and carries Sensel's
2019 installer signature.

## License

`fix-senselapp.sh` and this README are MIT-licensed. SenselApp is © Sensel, Inc. This
project isn't affiliated with Sensel.
