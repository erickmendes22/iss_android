"""Assemble the game-free runtime from pinned CI inputs and current native builds."""
import json
import shutil
import sys
import tarfile
import tempfile
import zipfile
from hashlib import sha256
from pathlib import Path

base_apk, shadow_apk = map(Path, sys.argv[1:])
out = Path('out/aot-apk')
assets = out/'package/assets'
libs = out/'package/lib/arm64-v8a'
assets.mkdir(parents=True, exist_ok=True)
libs.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory() as tmp:
    stage = Path(tmp)
    port = stage/'issport'
    native = port/'arm64'
    with zipfile.ZipFile(base_apk) as apk, apk.open('assets/runtime.tar.gz') as src:
        with tarfile.open(fileobj=src, mode='r|gz') as archive:
            archive.extractall(stage, filter='data')
    for name in ('libisscpu.so','libbbcpu.so','libtlggpu.so'):
        shutil.copy2(Path('out/bionic/lib')/name, native/'lib'/name)
    shutil.copy2('out/bionic/bin/iss-probe', native/'bin')
    shutil.copy2('out/bionic/lib/libisspresent.so', libs)
    with zipfile.ZipFile(shadow_apk) as apk:
        tool = apk.read('lib/arm64-v8a/libbbrecomp.so')
        profile = Path('tools/android/aot/profile.json')
        if sha256(tool).hexdigest() != json.loads(profile.read_text())['compiler']:
            raise RuntimeError('Unexpected AOT compiler')
        binary = native/'aot/bin/bb-recomp'
        binary.parent.mkdir(parents=True)
        binary.write_bytes(tool)
        binary.chmod(0o755)
        with apk.open('assets/runtime.payload') as src, tarfile.open(fileobj=src, mode='r|gz') as archive:
            for entry in archive:
                parts = Path(entry.name).parts
                if entry.isfile() and parts[:3] == ('sotcport','arm64','aot') and len(parts)>4 and parts[3] in ('licenses','bundle') and '..' not in parts:
                    target = native.joinpath(*parts[2:])
                    target.parent.mkdir(exist_ok=True, parents=True)
                    with target.open('wb') as dst:
                        shutil.copyfileobj(archive.extractfile(entry), dst)
        assert (native/'aot/bundle/helpers-aarch64.o').is_file()
        shutil.copy2(profile, native/'aot/profile.json')
    shutil.copytree('port/scripts',port/'scripts',dirs_exist_ok=True,ignore=shutil.ignore_patterns('__pycache__','test_*'))
    shutil.copytree('port/patches',port/'patches',dirs_exist_ok=True)
    shutil.copy2('tools/android/runtime/launch.py',port)
    shutil.copy2('tools/android/bionic/run-bionic.sh',port)
    manifest = stage/'runtime-manifest.json'
    meta = json.loads(manifest.read_text())
    meta.update(application_id='com.daedalus.iss.aot.local',version='0.2-aot.20',runtime_backend='android-bionic-arm64-aot',tso=False)
    manifest.write_text(json.dumps(meta,indent=2))
    archive = assets/'runtime.tar.gz'
    with tarfile.open(archive,'w:gz',compresslevel=5) as tar:
        for item in sorted(stage.iterdir()):
            tar.add(item, arcname=item.name)
    (assets/'runtime.sha256').write_text(sha256(archive.read_bytes()).hexdigest()+'\n')
