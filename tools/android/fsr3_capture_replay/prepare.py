#!/usr/bin/env python3
"""Build-source preparation for offline captured-buffer experiments, never APK code."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
PIN = 'c64f093404125e960813f95f1996c14780f0dd69'
PATCHES = [
    '0004-ffx-vk-compact-descriptor-pool-for-mobile.patch',
    '0007-fsr3-direct-hal-dispatch.patch',
    '0008-ffx-vk-compatible-memory-fallback.patch',
    '0009-fsr3-on-demand-history-introspection.patch',
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    source = ROOT / 'port/gpu/third_party/fsr-vulkan'
    actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != PIN:
        raise SystemExit(f'Expected pinned SDK {PIN}, found {actual}')
    patches = [ROOT / 'port/gpu/patches/fsr-vulkan' / name for name in PATCHES]
    identity = {'sdk': PIN, 'patches': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in patches}}
    marker = output / 'replay-sdk.json'
    sdk = output / 'sdk-source'
    output.mkdir(parents=True, exist_ok=True)
    if sdk.exists():
        if not marker.exists() or json.loads(marker.read_text()) != identity:
            raise SystemExit('Use a new output directory: existing SDK identity differs')
    else:
        sdk.mkdir()
        archive = subprocess.check_output(['git', '-C', str(source), 'archive', PIN])
        with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
            tar.extractall(sdk, filter='data')
        # Isolate git-apply from any enclosing player repository/worktree.
        subprocess.run(['git', 'init', '-q', str(sdk)], check=True)
        for patch in patches:
            subprocess.run(['git', '-C', str(sdk), 'apply', '--check', str(patch)], check=True)
            subprocess.run(['git', '-C', str(sdk), 'apply', str(patch)], check=True)
        private = sdk / 'src/ffx_vk_fsr3_portable.cpp'
        private.write_text(private.read_text() + '''
// Offline replay only. This source copy is not used to build the Android app.
extern "C" void replaySetFrameIndex(FfxVkPortableUpscaleContext* c,float index){
    auto* s=reinterpret_cast<FfxFsr3UpscalerContext_Private*>(&c->ffxContext);
    s->constants.frameIndex=index;
}
''')
        marker.write_text(json.dumps(identity, indent=2) + '\n')
    text = (sdk / 'tests/fsr3_vk_backend_smoke.cpp').read_text()
    prefix = text[:text.index('int main()')]
    old = 'createInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;'
    assert prefix.count(old) == 1
    prefix = prefix.replace(old, 'createInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;')
    setup = text[text.index('int main()'):text.index('    std::uint64_t memoryUsageBytes = 0;')]
    setup = setup.replace('int main()', 'int main(int argc,char** argv)')
    helpers = (HERE / 'helpers.inc').read_text()
    replay = (HERE / 'replay-body.inc').read_text()
    common = replay[replay.index('    ReadbackBuffer staging'):replay.index('    for(int frame=0;frame<2;')]
    common = common.replace('std::array<VkImageLayout,4>', 'std::array<VkImageLayout,3>')
    motion = (HERE / 'motion-body.inc').read_text().replace('COMMON_TRANSFER', common)
    (output / 'replay.cpp').write_text(prefix + helpers + setup + replay)
    (output / 'motion.cpp').write_text(prefix + helpers + setup + motion)
    (output / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(iss_capture_replay LANGUAGES C CXX)
set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_FSR3_3_1_5_SCAFFOLD OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_FSR4_V07_VULKAN OFF CACHE BOOL "" FORCE)
add_subdirectory("{sdk.as_posix()}" sdk)
add_executable(iss_replay "{(output / 'replay.cpp').as_posix()}")
add_executable(iss_motion "{(output / 'motion.cpp').as_posix()}")
foreach(target iss_replay iss_motion)
  set_property(TARGET ${{target}} PROPERTY CXX_STANDARD 17)
  target_compile_definitions(${{target}} PRIVATE FFX_VK_PORTABLE_ONLY=1)
  target_link_libraries(${{target}} PRIVATE ffx-vulkan::portable)
endforeach()
''')
    print(output)


if __name__ == '__main__':
    main()
