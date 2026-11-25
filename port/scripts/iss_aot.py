"""Compile an unpatched inFAMOUS linked image for Android ARM64 with bb-recomp 1.3.

Usage: python3 port/scripts/iss_aot.py GAME NEW_OUTPUT --compiler /path/to/bb-recomp
The compiler resolves its own bundle by default. The output is a development
artifact for native CPU backend integration and contains private game code.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile

from link_modules import link
from patches import game_profile, game_title_id
from prepare import prepare


def compile_image(game, work, compiler, jobs, bundle):
    library = work / 'libIssAot.so'
    stats = work / 'compile.txt'
    command = [str(compiler), '--boot', str(work / 'boot-linked.bin'),
               '--elf', str(work / 'eboot.elf'), '--prx-dir', str(game / 'sce_module'),
               '--so', str(library), '--stats', str(stats), '--target', 'aarch64',
               '--host', 'armv8.2', '--resume-entries', '--no-callee-saved',
               '-O1', '--jobs', str(jobs), '--shards', '64', '--no-nice']
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
