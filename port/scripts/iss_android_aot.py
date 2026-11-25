"""Select native units for the exact linked dump and selected executable patches."""
import bisect
import fcntl
import hashlib
import json
import mmap
from pathlib import Path
import shutil
import struct
import tempfile

from iss_aot import compile_image
from iss_mask_patches import image_range


def patch_image(source, destination, patches):
    shutil.copyfile(source, destination)
    with destination.open('r+b') as stream, mmap.mmap(stream.fileno(), 0) as blob:
        offset, size = image_range(blob)
        _, _, _, _, reloc_count, _, _ = struct.unpack_from('<8s6Q', blob)
        reloc_start = offset - 32 * reloc_count
        relocs = sorted(struct.unpack_from('<Q', blob, i)[0]
                        for i in range(reloc_start, offset, 32))
        magic, _, count = struct.unpack_from('<8sQQ', patches)
        if magic != b'G3PATCH2':
            raise ValueError('Unknown patch format')
        pos = 24
        for _ in range(count):
            dest, length = struct.unpack_from('<QQ', patches, pos)
            pos += 16
            if dest + length > size or pos + length > len(patches):
                raise ValueError('Patch exceeds image or payload')
            i = bisect.bisect_right(relocs, dest - 8)
            if i < len(relocs) and relocs[i] < dest + length:
                raise ValueError('AOT patch overlaps a runtime relocation')
            blob[offset+dest:offset+dest+length] = patches[pos:pos+length]
            pos += length
        if pos != len(patches):
            raise ValueError('Trailing patch data')
        blob[:8] = b'BBPROBE5'


def select(game, out, runtime):
    profile = json.loads((runtime/'aot/profile.json').read_text())
    actual = json.loads((out/'analysis.json').read_text())['source_sha256']
    modules = json.loads((out/'link.json').read_text())['modules']
    if actual != profile['eboot'] or {m['file']:m['sha256'] for m in modules} != profile['modules']:
        raise ValueError('AOT requires the matching CUSA00004 eboot, libc and Fios2')
    patches = (out/'patches.bin').read_bytes()
    entries = Path(__file__).with_name('iss_cusa00004_entries.txt')
    key = hashlib.sha256((runtime/'aot/profile.json').read_bytes()+patches+entries.read_bytes()).hexdigest()
    cache = out/'aot-cache'
    cache.mkdir(exist_ok=True)
    library = cache/(key+'.so')
    with (cache/'compile.lock').open('a') as lock:
        print('ISS-AOT waiting', flush=True)
        fcntl.flock(lock, fcntl.LOCK_EX)
        digest = library.with_suffix('.sha256')
        if library.is_file() and digest.is_file():
            print('ISS-AOT checking-cache', flush=True)
            with library.open('rb') as stream:
                if hashlib.file_digest(stream, 'sha256').hexdigest() == digest.read_text().strip():
                    print('ISS-AOT cached', flush=True)
                    return library
        print('Compiling native ARM64 units for the selected patches. The result will be cached.', flush=True)
        with tempfile.TemporaryDirectory(dir=cache) as directory:
            work = Path(directory)
            print('ISS-AOT patch-image', flush=True)
            patch_image(out/'boot-linked.bin', work/'boot-linked.bin', patches)
            (work/'eboot.elf').symlink_to(out/'eboot.elf')
            compile_image(game, work, runtime/'aot/bin/bb-recomp', 2, runtime/'aot/bundle', entries)
            print('ISS-AOT verifying', flush=True)
            with (work/'libIssAot.so').open('rb') as stream:
                checksum = hashlib.file_digest(stream, 'sha256').hexdigest()
            (work/'verified.sha256').write_text(checksum)
            (work/'libIssAot.so').replace(library)
            (work/'verified.sha256').replace(digest)
    return library
