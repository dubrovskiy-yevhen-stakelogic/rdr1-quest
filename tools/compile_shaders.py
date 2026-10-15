"""Rebuild the SPIR-V headers of the native library from their GLSL sources.

The VR shaders are compiled with glslc and spirv-val from the Vulkan SDK 1.4.357.0 (optimised); the
tonemap and harness shaders with the glslc of NDK r27c (not optimised), exactly as the shipped headers.
Usage: python tools/compile_shaders.py --sdk-glslc PATH --ndk-glslc PATH [--spirv-val PATH] [--check]
"""
import argparse
import hashlib
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# header, namespace, shader dir, label (None: plain header), optimise/validate, -Werror, modules
HEADERS = [
    ('native/src/vr/HandsShaders.h', 'rdrvr::vr::hands::shaders', 'native/src/vr/shaders', 'hands', True, False,
     [('hands.vert', 'vertex'), ('hands.frag', 'fragment'), ('hands_marker.vert', 'markerVertex'),
      ('hands_marker.frag', 'markerFragment'), ('hands_laser.vert', 'laserVertex'), ('hands_laser.frag', 'laserFragment'),
      ('hands_arc.vert', 'arcVertex'), ('hands_arc.frag', 'arcFragment'), ('hands_arc_marker.vert', 'arcMarkerVertex'),
      ('hands_arc_marker.frag', 'arcMarkerFragment'), ('hands_cartridge.vert', 'cartridgeVertex'),
      ('hands_cartridge.frag', 'cartridgeFragment')]),
    ('native/src/vr/TaaShaders.h', 'rdrvr::vr::taa::shaders', 'native/src/vr/shaders', 'TAA', True, True,
     [('taa.vert', 'vertex'), ('taa_resolve.frag', 'resolve'), ('taa_apply.frag', 'apply'), ('taa_lite.frag', 'lite')]),
    ('native/src/runtime/SpaceWarpShaders.h', 'rdrvr::runtime::spacewarp::shaders', 'native/src/runtime/shaders', 'SpaceWarp',
     True, False, [('spacewarp.vert', 'vertex'), ('spacewarp.frag', 'fragment'), ('spacewarp_depth.frag', 'depthDownsample')]),
    ('native/src/runtime/StereoAntiAliasingShaders.h', 'rdrvr::runtime::aa', 'native/src/runtime/shaders', 'anti-aliasing',
     True, False, [('fullscreen.vert', 'vertex'), ('fxaa.frag', 'fxaaFragment')]),
    ('native/generated/tonemap-shaders.h', 'rdrvr::runtime::tonemap', 'native/src/runtime/shaders', None, False, False,
     [('tonemap.vert', 'vertex'), ('tonemap.frag', 'fragment'), ('overlay.frag', 'overlayFragment')]),
    ('native/generated/shaders.h', 'rdrvr::harness::shaders', 'native/src/harness/shaders', None, False, False,
     [('lines.vert', 'vertex'), ('lines.frag', 'fragment')]),
]


def lf_sha(path):
    return hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest()


def compile_words(glslc, spirv_val, source, optimise, werror, tmp):
    spv = Path(tmp) / (source.name + '.spv')
    cmd = [str(glslc), '--target-env=vulkan1.0']
    if optimise:
        cmd.append('-O')
    if werror:
        cmd.append('-Werror')
    subprocess.run(cmd + [str(source), '-o', str(spv)], check=True)
    if optimise and spirv_val:
        subprocess.run([str(spirv_val), '--target-env', 'vulkan1.0', str(spv)], check=True)
    data = spv.read_bytes()
    if len(data) % 4 or data[:4] != b'\x03\x02\x23\x07':
        raise ValueError('invalid SPIR-V ' + source.name)
    return struct.unpack('<' + 'I' * (len(data) // 4), data)


def build(entry, sdk_glslc, ndk_glslc, spirv_val):
    _header, namespace, shader_dir, label, optimise, werror, modules = entry
    out = ['#pragma once']
    if label:
        out.append(f'// SPIR-V of the {label} shaders in {shader_dir}, compiled by tools/compile_shaders.py. Do not edit.')
    out += ['#include <cstdint>', f'namespace {namespace} {{']
    glslc = sdk_glslc if optimise else ndk_glslc
    with tempfile.TemporaryDirectory() as tmp:
        for filename, name in modules:
            source = ROOT / shader_dir / filename
            words = compile_words(glslc, spirv_val, source, optimise, werror, tmp)
            if label:
                out.append(f'// {filename} sha256(LF)={lf_sha(source)} words={len(words)}')
            out.append(f'inline constexpr std::uint32_t {name}[] = {{' + ','.join(hex(x) for x in words) + '};')
    out.append('}')
    return '\n'.join(out) + '\n'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--sdk-glslc', required=True, help='glslc of the Vulkan SDK 1.4.357.0')
    parser.add_argument('--ndk-glslc', required=True, help='shader-tools glslc of NDK r27c')
    parser.add_argument('--spirv-val', help='spirv-val of the Vulkan SDK 1.4.357.0')
    parser.add_argument('--check', action='store_true', help='report stale headers without writing them')
    args = parser.parse_args(argv)
    stale = 0
    for entry in HEADERS:
        header = ROOT / entry[0]
        text = build(entry, args.sdk_glslc, args.ndk_glslc, args.spirv_val)
        current = header.read_bytes().decode('utf-8') if header.exists() else ''
        if args.check:
            if current != text:
                stale += 1
                print('stale:', entry[0])
            continue
        if current != text:
            header.write_bytes(text.encode('utf-8'))
            print('wrote', entry[0])
    if args.check:
        print('all headers up to date' if not stale else f'{stale} stale headers')
        return 1 if stale else 0
    return 0


if __name__ == '__main__':
    sys.exit(main())
