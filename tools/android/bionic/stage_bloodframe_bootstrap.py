#!/usr/bin/env python3
"""Recover only the unchanged Bloodframe library/licenses from our pinned APK.

Opt-in recovery for the removed PancakeTAS/dxbc source repository. No renderer,
driver, CPU runtime, shaders or launcher are recovered from the old package.
All archive layers and every output are pinned before any output is written.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import tarfile
import urllib.request
from urllib.parse import urlsplit
import zipfile

ROOT = Path(__file__).resolve().parents[3]
MANIFEST = Path(__file__).with_name("bloodframe-bootstrap.json")


class ArtifactRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, *args, **kwargs):
        redirected = super().redirect_request(request, *args, **kwargs)
        if redirected and urlsplit(redirected.full_url).netloc != urlsplit(request.full_url).netloc:
            redirected.remove_header("Authorization")
        return redirected


def verify(data, expected, label):
    if hashlib.sha256(data).hexdigest() != expected:
        raise RuntimeError(f"Bloodframe bootstrap rejected: {label} SHA256 mismatch")
    return data


def check_inputs(manifest, commit, root=ROOT):
    if commit != manifest["bloodframe_commit"]:
        raise RuntimeError("Bloodframe pin changed; rebuild from source")
    for name, digest in manifest["compatibility_inputs"].items():
        verify((root / name).read_bytes(), digest, name)


def payloads(archive, manifest):
    verify(archive, manifest["artifact_sha256"], "Actions artifact")
    with zipfile.ZipFile(io.BytesIO(archive)) as artifact:
        apk = verify(artifact.read(manifest["apk"]), manifest["apk_sha256"], "APK")
        metadata = json.loads(artifact.read("RELEASE-METADATA.json"))
        if metadata["source_commit"] != manifest["source_commit"]:
            raise RuntimeError("Bloodframe bootstrap source commit mismatch")
    with zipfile.ZipFile(io.BytesIO(apk)) as package:
        runtime = verify(package.read("assets/runtime.tar.gz"), manifest["runtime_sha256"], "runtime")
    result = {}
    with tarfile.open(fileobj=io.BytesIO(runtime), mode="r:gz") as tar:
        for name, pin in manifest["files"].items():
            members = [m for m in tar.getmembers() if m.name == name]
            if len(members) != 1 or not members[0].isfile() or members[0].size != pin["size"]:
                raise RuntimeError(f"Bloodframe bootstrap invalid member: {name}")
            data = verify(tar.extractfile(members[0]).read(), pin["sha256"], name)
            output = Path(pin["output"])
            if output.is_absolute() or ".." in output.parts:
                raise RuntimeError("Bloodframe bootstrap invalid output path")
            result[output] = data
    elf = result[Path("lib/libbloodframe.so")]
    # ELF64, little endian, ET_DYN, EM_AARCH64.
    if elf[:6] != b"\x7fELF\x02\x01" or elf[16:20] != b"\x03\x00\xb7\x00":
        raise RuntimeError("Bloodframe bootstrap is not an AArch64 shared library")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--archive", type=Path, help="Already downloaded pinned Actions ZIP")
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text())
    check_inputs(manifest, args.commit)
    if args.archive:
        archive = args.archive.read_bytes()
    else:
        token = os.environ.get("GH_TOKEN")
        if not token:
            raise RuntimeError("Supply --archive or GH_TOKEN with Actions read access")
        url = (f'https://api.github.com/repos/{manifest["repository"]}/actions/artifacts/'
               f'{manifest["artifact_id"]}/zip')
        request = urllib.request.Request(url, headers={"Authorization": f"Bearer {token}",
                                                      "Accept": "application/vnd.github+json"})
        with urllib.request.build_opener(ArtifactRedirect()).open(request, timeout=120) as response:
            archive = response.read()
    verified = payloads(archive, manifest)
    for relative, data in verified.items():
        target = args.output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_suffix(target.suffix + ".tmp")
        temporary.write_bytes(data)
        temporary.replace(target)
    print(f'Bloodframe: verified unchanged {manifest["bloodframe_commit"][:12]} '
          f'from build 71 ({len(verified)} library/license files)')


if __name__ == "__main__":
    main()
