#!/usr/bin/env python3
"""
Assemble the standalone Linux + Windows release bundle at:
  dist/tzx-mode-installer/

Contains zero copyrighted factory firmware — only our compiled TZX Mode payload
(TOOLS.AP), ARM ADFU SRAM server (adfus.bin), SPI NOR helper (spiflash_atj2157.bin),
cross-platform installer (tzx_installer.py), Linux install.sh, Windows install.ps1
+ inbox WinUSB INF, and README.md.
"""
import hashlib
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REL_SRC = ROOT / "tools/release"
DIST = ROOT / "dist/tzx-mode-installer"


def main() -> None:
    if DIST.exists():
        shutil.rmtree(DIST)
    (DIST / "payloads").mkdir(parents=True)
    (DIST / "drivers").mkdir(parents=True)

    copies = [
        (REL_SRC / "tzx_installer.py", DIST / "tzx_installer.py"),
        (REL_SRC / "install.sh", DIST / "install.sh"),
        (REL_SRC / "install.ps1", DIST / "install.ps1"),
        (REL_SRC / "README.md", DIST / "README.md"),
        (REL_SRC / "atj2157_adfu.inf", DIST / "drivers/atj2157_adfu.inf"),
        (ROOT / "build/TOOLS.AP", DIST / "payloads/TOOLS.AP"),
        (ROOT / "build/spiflash_atj2157.bin", DIST / "payloads/spiflash_atj2157.bin"),
        (ROOT / "vendor/kilicdev_actions_flash/payload_arm/adfus.bin", DIST / "payloads/adfus.bin"),
    ]
    for src, dst in copies:
        if not src.exists():
            raise SystemExit(f"Missing required artifact: {src} (run tools/tzx_app/build.sh first)")
        shutil.copy2(src, dst)

    (DIST / "install.sh").chmod(0o755)
    (DIST / "tzx_installer.py").chmod(0o755)

    lines = []
    for p in sorted(DIST.rglob("*")):
        if p.is_file():
            rel = p.relative_to(DIST).as_posix()
            lines.append(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {rel}")
    (DIST / "SHA256SUMS").write_text("\n".join(lines) + "\n")
    print(f"[RELEASE] Assembled {DIST} ({len(lines)} files)")


if __name__ == "__main__":
    main()
