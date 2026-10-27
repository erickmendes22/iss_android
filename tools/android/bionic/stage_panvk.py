#!/usr/bin/env python3
"""Pin and stage Bloodborne-Android PanVK v3.2 for Mali CSF / Android 15+."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import urllib.request
import zipfile

NAME = "PanVK-android-v3.2-universal-2a9358a.zip"
SHA256 = "68d4eccc1442cb0bef08228c90127388e68bf6087898abc492c717b9a06ca61f"
URL = "https://github.com/ducvd89/Bloodborne-Android/releases/download/panvk-android-v3.2-2a9358a/" + NAME
MAX_SIZE = 128 * 1024 * 1024


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def stage(archive, target):
    archive = Path(archive)
    if archive.stat().st_size > MAX_SIZE or sha256(archive.read_bytes()) != SHA256:
        raise ValueError("PanVK archive is oversized or its SHA256 does not match")
    with zipfile.ZipFile(archive) as z:
        meta = json.loads(z.read("meta.json"))
        library = meta.get("libraryName", "")
        if (not isinstance(library, str) or not library.endswith(".so")
                or "/" in library or "\\" in library):
            raise ValueError("Unsafe PanVK libraryName")
        if int(meta.get("minApi", 35)) > 35:
            raise ValueError("This PanVK package requires Android newer than 15")
        if z.getinfo(library).file_size > MAX_SIZE:
            raise ValueError("PanVK library too large")
        elf = z.read(library)
    with tempfile.TemporaryDirectory() as tmp:
        file = Path(tmp) / "panvk.so"
        file.write_bytes(elf)
        hdr = subprocess.check_output(["readelf", "-h", str(file)], text=True)
        dyn = subprocess.check_output(["readelf", "-d", str(file)], text=True)
        syms = subprocess.check_output(["readelf", "--dyn-syms", "--wide", str(file)], text=True)
        if not all(x in hdr for x in ("ELF64", "AArch64", "DYN ")):
            raise ValueError("PanVK must be ARM64 ELF shared library")
        if "[libc.so]" not in dyn or "[libc.so.6]" in dyn:
            raise ValueError("PanVK must be linked for Android Bionic")
        exports = ("HMI", "vk_icdGetInstanceProcAddr", "vkGetInstanceProcAddr")
        if not any(line.split()[-1:] == [e] and " UND " not in line
                   for line in syms.splitlines() for e in exports):
            raise ValueError("PanVK must export Android HAL or Vulkan ICD entrypoint")
    target = Path(target)
    target.mkdir(parents=True, exist_ok=True)
    (target / "libvulkan_panvk.so").write_bytes(elf)
    (target / "PINNED-DRIVER.json").write_text(json.dumps({
        "id": "bloodborne-panvk-v3.2",
        "name": "PanVK 3.2 (Mali CSF experimental)",
        "source": URL,
        "archive_sha256": SHA256,
        "upstream_library": library,
        "library": "libvulkan_panvk.so",
        "library_sha256": sha256(elf),
        "min_api": 35,
        "experimental": True,
    }, indent=2) + "\n")
    print("Bloodborne PanVK v3.2 validated and staged")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--archive", type=Path)
    p.add_argument("--output", type=Path, default=Path("out/bionic/drivers/panvk"))
    a = p.parse_args()
    if a.archive:
        stage(a.archive, a.output)
        return
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / NAME
        with urllib.request.urlopen(URL, timeout=120) as response:
            with archive.open("wb") as target:
                size = 0
                while True:
                    block = response.read(1024 * 1024)
                    if not block:
                        break
                    size += len(block)
                    if size > MAX_SIZE:
                        raise ValueError("PanVK download too large")
                    target.write(block)
        stage(archive, a.output)


if __name__ == "__main__":
    main()
