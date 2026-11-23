# Pocket-TZX

Custom firmware mode for cheap Actions Semiconductor ATJ2157 digital voice recorders (`10d6:1101`).

It adds a 6th **TZX Mode** entry to the main menu so you can play ZX Spectrum `.TZX` tape files straight from a microSD card over a 3.5 mm audio cable, while keeping all 5 original recorder modes intact.

<!-- Drop your photos into images/ and uncomment these lines:
![Recorder](images/device.jpg)
![TZX Mode menu](images/menu.jpg)
![Playing a TZX file](images/playing.jpg)
-->

## Compatible Hardware

Tested on the generic unbranded "DIGITAL VOICE RECORDER" with:
- Actions `ATJ2157` chip (USB `10d6:1101`, stock firmware `1.101.56`)
- MicroSD card slot
- Small 128x64 monochrome LCD
- Front buttons: Record, Play/Pause, Stop
- Right side: M, FWD, RWD
- Left side: +, -, Power

If you are not sure whether yours matches, run the read-only compatibility check first.

## Installing

Plug the recorder into USB (with a microSD card inserted) and wait for the "connected" icon on screen. Python 3 is required (`libusb` on Linux; Windows installs its own WinUSB driver automatically).

From the release installer folder (built with `python3 tools/make_release_bundle.py`):

**Linux:**
```bash
./install.sh --check   # optional, read-only
./install.sh           # backs up stock firmware, then installs
```

**Windows (PowerShell):**
```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1 -Check
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

A full backup of your original firmware is saved to `backups/` before anything is written.

To go back to stock firmware at any time:
```bash
./install.sh --restore backups/<backup_folder>
```
```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1 -Restore .\backups\<backup_folder>
```

## Usage

Put your `.TZX` files in a `/TZX` folder on the microSD card (subfolders work too). Select **TZX Mode** from the main menu, plug a 3.5 mm cable into the EAR input on your Spectrum / Spectrum Next, and press Play.

| Button | Browser | Playback |
|---|---|---|
| Play/Pause | Open folder / load tape | Play / pause / resume |
| Stop | Up folder / exit to menu | Back to file list |
| FWD / RWD | Move selection | Next / previous block |
| + / - | Volume | Volume |
| M | Toggle 48K / 128K mode | Toggle 48K / 128K mode |
| Record | - | Invert polarity (NORM / INV) |
| Power | Power off | Power off |

## Technical Notes

Hardware notes, memory maps, build scripts, and recovery instructions are in [docs/](docs/) and [firmware-stock/RECOVERY.md](firmware-stock/RECOVERY.md).
