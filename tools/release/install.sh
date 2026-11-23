#!/usr/bin/env bash
# Linux Installer / Backup / Restorer for Actions ATJ2157 ZX Spectrum Next TZX Mode
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UDEV_RULE="/etc/udev/rules.d/70-actions.rules"

usage() {
  cat <<'EOF'
Usage:
  ./install.sh --check             Read-only compatibility check (writes nothing)
  ./install.sh                     Backup stock firmware, verify, and install TZX Mode
  ./install.sh --backup            Read-only full 4 MiB flash backup (no writes)
  ./install.sh --restore <dir>     Restore factory stock firmware from a backup folder
  ./install.sh --update <dir>      Update TZX Mode using an existing stock backup folder
  ./install.sh --yes               Skip confirmation prompt (combine with above)
EOF
}

# 1. Check Python 3 and libusb-1.0
if ! command -v python3 >/dev/null 2>&1; then
  echo "Error: python3 is required." >&2
  exit 1
fi

if ! python3 -c 'import ctypes; ctypes.CDLL("libusb-1.0.so.0")' 2>/dev/null; then
  echo "Error: libusb-1.0.so.0 not found." >&2
  echo "Install it with:" >&2
  echo "  Fedora/RHEL : sudo dnf install libusb1" >&2
  echo "  Debian/Ubuntu: sudo apt install libusb-1.0-0" >&2
  echo "  Arch Linux  : sudo pacman -S libusb" >&2
  exit 1
fi

# 2. Ensure non-root USB access to Actions MSC (10d6:1101) and ADFU (10d6:10d6)
if [[ ! -f "$UDEV_RULE" ]]; then
  echo "[Linux] Installing udev rule $UDEV_RULE for non-root USB access (sudo required once)..."
  sudo tee "$UDEV_RULE" >/dev/null <<'RULE'
SUBSYSTEM=="usb", ATTR{idVendor}=="10d6", ATTR{idProduct}=="1101", MODE="0666", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="10d6", ATTR{idProduct}=="10d6", MODE="0666", TAG+="uaccess"
RULE
  sudo udevadm control --reload-rules
  sudo udevadm trigger --subsystem-match=usb
  echo "[Linux] udev rule installed. If the recorder is already plugged in, replug it once if access fails."
fi

# 3. Parse flags and invoke tzx_installer.py
ACTION="install"
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --check)
      ACTION="check"
      shift
      ;;
    --backup)
      ACTION="backup"
      shift
      ;;
    --restore)
      ACTION="restore"
      shift
      [[ $# -ge 1 ]] || { echo "Error: --restore requires a backup directory." >&2; exit 1; }
      EXTRA_ARGS+=("$1")
      shift
      ;;
    --update)
      ACTION="install"
      shift
      [[ $# -ge 1 ]] || { echo "Error: --update requires a stock backup directory." >&2; exit 1; }
      EXTRA_ARGS+=("--backup-dir" "$1")
      shift
      ;;
    --yes|-y)
      EXTRA_ARGS+=("--yes")
      shift
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

exec python3 "$SCRIPT_DIR/tzx_installer.py" "$ACTION" "${EXTRA_ARGS[@]}"
