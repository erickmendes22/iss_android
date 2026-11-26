"""Compile an unpatched inFAMOUS linked image for Android ARM64 with bb-recomp 1.3.

Usage: python3 port/scripts/iss_aot.py GAME NEW_OUTPUT --compiler /path/to/bb-recomp
The compiler resolves its own bundle by default. The output is a development
artifact for native CPU backend integration and contains private game code.
"""
import argparse
import json
import mmap
from pathlib import Path
import re
import struct
import subprocess
import tempfile

from link_modules import link
from patches import game_profile, game_title_id
from prepare import prepare


def verify_hook_sites(library, catalog):
    expected = {}
    for line in catalog.read_text().splitlines():
        fields = line.partition('#')[0].split()
        if fields and fields[0] == 'hook':
            expected[int(fields[1], 0)] = int(fields[2], 0)
    with library.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as elf:
        offset, = struct.unpack_from('<Q', elf, 40)
        stride, count = struct.unpack_from('<HH', elf, 58)
        sections = [struct.unpack_from('<II4QII2Q', elf, offset + i * stride) for i in range(count)]
        for section in sections:
            if section[1] != 11:  # SHT_DYNSYM
                continue
            strings = sections[section[6]][4]
            for pos in range(section[4], section[4] + section[5], section[9]):
                name, _, _, index, address, size = struct.unpack_from('<IBBHQQ', elf, pos)
                if elf[strings + name:strings + name + 15] != b'bbr_hook_sites\0':
                    continue
                data = sections[index]
                start = data[4] + address - data[3]
                sites = {addr: length for addr, length, flags, _, _ in
                         struct.iter_unpack('<Q4I', elf[start:start + size]) if flags & 5 == 5}
                if expected and sites == expected:
                    return
    raise ValueError('AOT library lacks the required compiled camera and AA hooks')


def compile_image(game, work, compiler, jobs, bundle, extra_entries=None, hooks=None):
    library = work / 'libIssAot.so'
    stats = work / 'compile.txt'
    command = [str(compiler), '--boot', str(work / 'boot-linked.bin'),
               '--elf', str(work / 'eboot.elf'), '--prx-dir', str(game / 'sce_module'),
               '--so', str(library), '--stats', str(stats), '--target', 'aarch64',
               '--host', 'armv8.2', '--resume-entries', '--no-callee-saved',
               '-O1', '--jobs', str(jobs), '--shards', '128', '--no-nice', '--progress']
    if extra_entries is not None:
        command.extend(['--extra-entries', str(extra_entries)])
    if hooks is not None:
        command.extend(['--variants', str(hooks)])
    if bundle:
        command.extend(['--bundle-dir', str(bundle)])
    subprocess.run(command, cwd=work, check=True)
    with library.open('rb') as stream:
        header = stream.read(64)
    if (len(header) < 64 or header[:6] != b'\x7fELF\x02\x01'
            or struct.unpack_from('<HH', header, 16) != (3, 183)):
        raise ValueError('expected a 64-bit little-endian AArch64 shared object')
    report = stats.read_text()
    lifted = re.search(r'units (\d+) \(lift failures (\d+)\)', report)
    traps = re.search(r'units with bbr_trap \(unsupported ops\):\s*(\d+)', report)
    if not lifted or not traps or not int(lifted[1]) or int(lifted[2]) or int(traps[1]):
        raise ValueError('incomplete AOT translation; consult compile.txt in the output directory')
    if hooks is not None:
        verify_hook_sites(library, hooks)
    return int(lifted[1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('out', type=Path, help='new private output directory')
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--bundle-dir', type=Path)
    parser.add_argument('--jobs', type=int, default=3)
    args = parser.parse_args()
    game, out = args.game.resolve(), args.out.resolve()
    profile = game_profile(game_title_id(game))
    if not profile or profile[1].name != 'InfamousSecondSon.xml':
        parser.error('select an extracted inFAMOUS Second Son game directory')
    if args.jobs < 1 or out == game or game in out.parents:
        parser.error('use positive jobs and a new output directory outside the dump')
    out.mkdir(parents=True, exist_ok=False)
    # Only publish the library after codegen and the coverage checks succeed.
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        work = Path(tmp)
        prepare(game, work)
        link(game, work, native_aot=True)
        try:
            count = compile_image(game, work, args.compiler.resolve(), args.jobs,
                                  args.bundle_dir.resolve() if args.bundle_dir else None)
        finally:
            stats = work / 'compile.txt'
            if stats.is_file():
                stats.replace(out / stats.name)
        analysis = json.loads((work / 'analysis.json').read_text())
        print(f"{analysis['sfo']['TITLE_ID']} | eboot {analysis['source_sha256']} | {count} units")
        (work / 'libIssAot.so').replace(out / 'libIssAot.so')


if __name__ == '__main__':
    main()
