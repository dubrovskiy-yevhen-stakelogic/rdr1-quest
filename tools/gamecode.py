"""Generate the engine-code include files from the player's own game engine library.

Some native checks compare live engine code with expected bytes before they install a hook.
The kit stores only the location, length and SHA-256 of each such range in
native/gamecode/ranges.json; this tool reads the bytes from the player's librdr.so and writes
<out>/gamecode/<id>.inc for the native and layer builds.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path


def engine_digest(path):
    h = hashlib.sha256()
    size = 0
    with open(path, 'rb') as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
            size += len(chunk)
    return h.hexdigest(), size


def load_segments(f):
    f.seek(0)
    header = f.read(64)
    if header[:4] != b'\x7fELF' or header[4] != 2 or header[5] != 1:
        raise ValueError('the engine is not a 64-bit little-endian ELF file')
    phoff, = struct.unpack_from('<Q', header, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', header, 0x36)
    f.seek(phoff)
    table = f.read(phentsize * phnum)
    segments = []
    for i in range(phnum):
        typ, _flags, off, vaddr, _paddr, filesz, _memsz, _align = struct.unpack_from('<IIQQQQQQ', table, i * phentsize)
        if typ == 1 and filesz:
            segments.append((vaddr, off, filesz))
    return segments


def read_range(f, segments, vaddr, length):
    for base, off, size in segments:
        if base <= vaddr and vaddr + length <= base + size:
            f.seek(off + vaddr - base)
            return f.read(length)
    raise ValueError(f'range 0x{vaddr:x}+{length} is outside the loaded engine segments')


def render(rec, data):
    """Format one range as a comma-separated literal list for a braced initializer."""
    width = rec['width']
    values = [int.from_bytes(data[i:i + width], 'little') for i in range(0, len(data), width)]
    if rec['style'] == 'dec':
        items = [f'{v}{rec["suffix"]}' for v in values]
    else:
        digits = 2 * width
        fmt = '0x{:0%dX}' % digits if rec['upper'] else '0x{:0%dx}' % digits
        items = [fmt.format(v) + rec['suffix'] for v in values]
    per = 16 if width == 1 else 8
    rows = [','.join(items[i:i + per]) for i in range(0, len(items), per)]
    return ',\n'.join(rows) + '\n'


def generate(engine, ranges_path, out):
    doc = json.loads(Path(ranges_path).read_text(encoding='utf-8'))
    expected = doc['engine']
    sha, size = engine_digest(engine)
    if size != expected['size'] or sha != expected['sha256']:
        raise SystemExit(f'ERROR: unsupported game engine (sha256 {sha}, {size} bytes); '
                         f'expected {expected["sha256"]}, {expected["size"]} bytes')
    target = Path(out) / 'gamecode'
    target.mkdir(parents=True, exist_ok=True)
    written = 0
    with open(engine, 'rb') as f:
        segments = load_segments(f)
        for rec in doc['ranges']:
            data = read_range(f, segments, int(rec['vaddr'], 16), rec['length'])
            if hashlib.sha256(data).hexdigest() != rec['sha256']:
                raise SystemExit(f'ERROR: engine range {rec["id"]} does not match the supported engine')
            (target / f'{rec["id"]}.inc').write_bytes(render(rec, data).encode('ascii'))
            written += 1
    return written


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--engine', required=True, help='lib/arm64-v8a/librdr.so taken from the game package')
    parser.add_argument('--ranges', required=True, help='native/gamecode/ranges.json')
    parser.add_argument('--out', required=True, help='generated source directory')
    args = parser.parse_args(argv)
    count = generate(args.engine, args.ranges, args.out)
    print(f'Generated {count} engine-code ranges in {Path(args.out) / "gamecode"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
