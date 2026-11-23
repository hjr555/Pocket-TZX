# TZX Mode Installer for Digital Voice Recorder

Adds a dedicated **TZX Mode** to your voice recorder so you can load `.TZX` tape files directly into a **ZX Spectrum** or **ZX Spectrum Next** over a 3.5 mm audio cable, while keeping all 5 built-in recorder modes (`Music`, `Voice`, `Record`, `Folder`, `Settings`) working as normal.

## Compatible Model

This installer is made for the generic **"DIGITAL VOICE RECORDER"** with:
- **Chip:** Actions Semiconductor `ATJ2157` (USB ID `10d6:1101`, stock firmware `1.101.56`)
- **Storage:** MicroSD card slot
- **Screen:** Small monochrome LCD (`128x64`)
- **Buttons:**
  - Front: `Record`, `Play/Pause`, `Stop`
  - Right side: `M`, `FWD`, `RWD`
  - Left side: `+`, `-`, `Power`

Not sure if yours matches? Run the **read-only compatibility check** first — it checks your recorder without changing anything.

## Before You Start

1. Insert a microSD card into the recorder.
2. Plug the recorder into your computer with a USB data cable and wait until the screen shows the **connected (device to PC)** icon.
3. Make sure **Python 3** is installed:
   - **Linux:** `python3` + `libusb` (`sudo dnf install libusb1` on Fedora, `sudo apt install libusb-1.0-0` on Ubuntu/Debian).
   - **Windows 10/11:** Python 3 (`winget install Python.Python.3.12`). The PowerShell script sets up the USB driver automatically — no extra tools needed.

## Running the Installer

### Linux
Open a terminal in this folder:
```bash
# 1. Optional: check compatibility first (read-only, changes nothing)
./install.sh --check

# 2. Back up your recorder and install TZX Mode
./install.sh
```

### Windows 10/11
Open PowerShell in this folder:
```powershell
# 1. Optional: check compatibility first (read-only, changes nothing)
powershell -ExecutionPolicy Bypass -File .\install.ps1 -Check

# 2. Back up your recorder and install TZX Mode
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

The installer automatically saves a full backup of your original firmware in the `backups/` folder before making any changes, then verifies the install and reboots the recorder.

## Using TZX Mode

1. Copy your `.TZX` files into a folder called **`/TZX`** on the microSD card (subfolders are supported too).
2. Unplug the recorder, scroll to **TZX Mode** (cassette icon) on the main menu, and press **Play/Pause**.
3. Plug a 3.5 mm audio cable from the recorder's headphone jack to your ZX Spectrum / Next `EAR` input, set volume to around `28–31`, and press **Play/Pause** on a tape.

### Controls

| Button | In File List | During Playback |
|---|---|---|
| **Play/Pause** | Open folder / load tape | Play / Pause (or resume after a tape stop) |
| **Stop** | Go up a folder / back to Main Menu | Stop and return to file list |
| **FWD / RWD** | Move up / down | Skip to next / previous block |
| **+ / -** | Volume | Volume |
| **M** | Switch **48K / 128K** mode | Switch **48K / 128K** mode |
| **Record** | — | Flip audio polarity (`NORM` / `INV`) |
| **Power** | Power off | Power off |

- **Countdown timer (`-MM:SS`):** Shows estimated time remaining on the load.
- **48K / 128K mode (`M` button):** In `48K` mode, tapes with a "Stop if 48K" block will auto-pause; switch to `128K` mode to play straight through.

## Restoring Factory Firmware

To undo everything and return your recorder to its exact original state, pass the backup folder created during install:

- **Linux:** `./install.sh --restore backups/backup_YYYYMMDD_HHMMSS`
- **Windows:** `powershell -ExecutionPolicy Bypass -File .\install.ps1 -Restore .\backups\backup_YYYYMMDD_HHMMSS`
