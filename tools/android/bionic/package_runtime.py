#!/usr/bin/env python3
"""Package the ISS Android/Bionic runtime; no glibc, PS4 image or extracted game files."""
from pathlib import Path
import hashlib
import json
import subprocess
import shutil
import tarfile
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / "out/bionic"
OUT = ROOT / "out/release"

def main():
    OUT.mkdir(parents=True, exist_ok=True)
    manifest = ET.parse(ROOT / "android/app/BionicAndroidManifest.xml").getroot()
    version = manifest.get("{http://schemas.android.com/apk/res/android}versionName")
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    with tempfile.TemporaryDirectory(prefix="iss-bionic-stage-") as temp:
        stage = Path(temp)
        base = stage / "issport"
        runtime = base / "arm64"
        for kind, names in {
            "bin": ("iss-probe", "kgsl-chip-id", "orbis-pkg-util"),
            "lib": ("libisscpu.so", "libtlggpu.so", "libc++_shared.so", "libbloodframe.so"),
        }.items():
            directory = runtime / kind
            directory.mkdir(parents=True)
            for name in names:
                origin = SRC / kind / name
                if not origin.is_file() or not origin.stat().st_size:
                    raise RuntimeError(f"Missing Bionic binary: {origin}")
                shutil.copy2(origin, directory / name)
        shutil.copytree(SRC / "drivers", runtime / "drivers")
        python = runtime / "python"
        (python / "bin").mkdir(parents=True)
        shutil.copy2(SRC / "python/bin/python3.13", python / "bin/python3.13")
        shutil.copytree(SRC / "python/lib", python / "lib",
                        ignore=shutil.ignore_patterns("__pycache__", "test", "tests",
                                                       "idlelib", "tkinter", "ensurepip",
                                                       "config-3.13*", "site-packages"))
        shutil.copy2(SRC / "python/LICENSE-Python.txt", python / "LICENSE-Python.txt")
        shutil.copytree(ROOT / "port/scripts", base / "scripts",
                        ignore=shutil.ignore_patterns("__pycache__", "test_*.py"))
        shutil.copytree(ROOT / "port/patches", base / "patches")
        shutil.copy2(ROOT / "tools/android/runtime/launch.py", base / "launch.py")
        shutil.copy2(ROOT / "tools/android/bionic/run-bionic.sh", base / "run-bionic.sh")
        # This launcher is executed by Android's /system/bin/sh on the device.
        # A compiler cannot detect unmatched quotes or malformed shell syntax:
        # reject those *before* producing and signing another unusable APK.
        for shell in ("sh", "bash"):
            subprocess.run([shell, "-n", str(base / "run-bionic.sh")], check=True)

        for name in ("user", "tmp", "out"):
            (base / name).mkdir(parents=True)
        shutil.copytree(SRC / "licenses", base / "licenses", dirs_exist_ok=True)
        (base / "licenses").mkdir(exist_ok=True)
        shutil.copy2(ROOT / "tools/android/bionic/NOTICE.txt", base / "licenses/BIONIC-NOTICE.txt")
        shutil.copy2(ROOT / "third_party/FEX/LICENSE", base / "licenses/FEX-LICENSE.txt")
        shutil.copy2(ROOT / ".local-deps/android/bionic-prefix/share/licenses/libucontext/LICENSE",
                     base / "licenses/libucontext-LICENSE.txt")
        (stage / "runtime-manifest.json").write_text(
            json.dumps({"schema": 1, "runtime_backend": "android-bionic",
                        "application_id": "com.daedalus.iss.bionic",
                        "version": version, "source_commit": commit,
                        "game": "inFAMOUS Second Son",
                        "drivers": {"gen8": json.loads((ROOT / "tools/android/bionic/driver.json").read_text()),
                                    "gen7": json.loads((ROOT / "tools/android/bionic/driver-gen7.json").read_text()),
                                    "panvk": json.loads((SRC / "drivers/panvk/PINNED-DRIVER.json").read_text())}},
                       indent=2) + "\n")
        archive = OUT / "iss-bionic-runtime.tar.gz"
        with tarfile.open(archive, "w:gz", compresslevel=6) as tar:
            for path in sorted(stage.iterdir()):
                tar.add(path, arcname=path.name)
        (OUT / "iss-bionic-runtime.sha256").write_text(
            hashlib.sha256(archive.read_bytes()).hexdigest() + "\n")
        print(f"Packaged game-free Bionic runtime: {archive}, {archive.stat().st_size} bytes")

if __name__ == "__main__":
    main()
