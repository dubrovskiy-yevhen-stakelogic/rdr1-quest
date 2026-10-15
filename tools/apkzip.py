"""Raw-copy APK writer, payload verification and APK signing block reader.

Retained entries are copied with their local headers and compressed data unchanged,
so game assets are never recompressed. Rebuilding the central directory drops the
old APK signing block. ZIP64 archives are refused in both directions.
"""

from __future__ import annotations

import hashlib
import io
import os
import re
import struct
import zipfile
from pathlib import Path

ZIP32_LIMIT = 0xFFFFFFFF
BLOCK = 4 * 1024 * 1024
# A constant timestamp keeps added entries deterministic.
ENTRY_TIME = (1981, 1, 1, 1, 1, 2)

APK_SIG_BLOCK_MAGIC = b"APK Sig Block 42"
SIGNATURE_SCHEMES = (0xF05368C0, 0x1B93AD61, 0x7109871A)  # v3, v3.1, v2

_SIGNATURE_ENTRY = re.compile(r"META-INF/(?:MANIFEST\.MF|[^/]+\.(?:SF|RSA|DSA|EC))", re.IGNORECASE)


class ApkZipError(ValueError):
    """APK that cannot be copied or checked safely."""


def signature_entry(name: str) -> bool:
    """Entries that belong to the old signature and are not copied."""
    return bool(_SIGNATURE_ENTRY.fullmatch(name)) or name == "stamp-cert-sha256"


def inventory(archive: zipfile.ZipFile) -> dict[str, zipfile.ZipInfo]:
    entries = archive.infolist()
    names = [entry.filename for entry in entries]
    if len(names) != len(set(names)):
        raise ApkZipError("duplicate ZIP entries")
    for entry in entries:
        if entry.flag_bits & 1:
            raise ApkZipError(f"encrypted ZIP entry: {entry.filename}")
        if max(entry.header_offset, entry.file_size, entry.compress_size) >= ZIP32_LIMIT:
            raise ApkZipError(f"ZIP64 entry is not supported: {entry.filename}")
        if entry.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
            raise ApkZipError(f"unsupported ZIP compression: {entry.filename}")
    return {entry.filename: entry for entry in entries}


def _central_records(stream, archive: zipfile.ZipFile) -> dict[str, bytes]:
    stream.seek(archive.start_dir)
    records = {}
    for entry in archive.infolist():
        header = stream.read(46)
        if len(header) != 46 or header[:4] != b"PK\x01\x02":
            raise ApkZipError("invalid ZIP central directory record")
        name_length, extra_length, comment_length = struct.unpack_from("<HHH", header, 28)
        record = header + stream.read(name_length + extra_length + comment_length)
        if struct.unpack_from("<I", record, 42)[0] != entry.header_offset:
            raise ApkZipError("ZIP central directory order does not match")
        records[entry.filename] = record
    return records


def _data_start(stream, entry: zipfile.ZipInfo, central: bytes) -> int:
    stream.seek(entry.header_offset)
    header = stream.read(30)
    if len(header) != 30 or header[:4] != b"PK\x03\x04":
        raise ApkZipError(f"invalid ZIP local header: {entry.filename}")
    name_length, extra_length = struct.unpack_from("<HH", header, 26)
    local_name = stream.read(name_length)
    central_name_length = struct.unpack_from("<H", central, 28)[0]
    if local_name != central[46:46 + central_name_length]:
        raise ApkZipError(f"ZIP local and central names differ: {entry.filename}")
    return entry.header_offset + 30 + name_length + extra_length


def _raw_extent(stream, entry: zipfile.ZipInfo, central: bytes) -> tuple[int, int]:
    end = _data_start(stream, entry, central) + entry.compress_size
    if entry.flag_bits & 8:
        stream.seek(end)
        end += 16 if stream.read(4) == b"PK\x07\x08" else 12
    return entry.header_offset, end


def _check_extents(stream, archive: zipfile.ZipFile, central: dict[str, bytes]) -> None:
    extents = sorted(_raw_extent(stream, e, central[e.filename]) for e in archive.infolist())
    last_end = 0
    for start, end in extents:
        if start < last_end or end > archive.start_dir:
            raise ApkZipError("overlapping or misplaced ZIP entries")
        last_end = end


def compressed_sha256(stream, entry: zipfile.ZipInfo, central: bytes) -> str:
    digest = hashlib.sha256()
    stream.seek(_data_start(stream, entry, central))
    remaining = entry.compress_size
    while remaining:
        block = stream.read(min(remaining, BLOCK))
        if not block:
            raise ApkZipError(f"truncated ZIP entry: {entry.filename}")
        digest.update(block)
        remaining -= len(block)
    return digest.hexdigest()


def write_apk(
    source: Path,
    target: Path,
    changes: dict[str, tuple[bytes, int]],
    *,
    limit: int = ZIP32_LIMIT,
) -> None:
    """Copy source to target raw, replacing or adding the entries in changes.

    changes maps an entry name to (data, compression). Replaced entries and new
    entries are written after the retained ones. Old signature files are dropped.
    """
    if Path(source).resolve() == Path(target).resolve():
        raise ApkZipError("refusing to overwrite the source APK")
    for name in changes:
        if signature_entry(name) or not name or name.startswith("/") or "\\" in name:
            raise ApkZipError(f"invalid entry name for an APK: {name}")
    staged = io.BytesIO()
    with zipfile.ZipFile(staged, "w") as additions:
        for name, (data, compression) in changes.items():
            if compression not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                raise ApkZipError(f"unsupported compression for {name}")
            info = zipfile.ZipInfo(name, ENTRY_TIME)
            info.compress_type = compression
            info.external_attr = 0o100644 << 16
            additions.writestr(info, data)
    staged.seek(0)
    records = []
    created = False
    try:
        with open(source, "rb") as original, zipfile.ZipFile(original) as archive, zipfile.ZipFile(staged) as extra:
            inventory(archive)
            original_central = _central_records(original, archive)
            _check_extents(original, archive, original_central)
            with open(target, "xb") as output:
                created = True
                for stream, zip_file, central in (
                    (original, archive, original_central),
                    (staged, extra, _central_records(staged, extra)),
                ):
                    for entry in zip_file.infolist():
                        if stream is original and (entry.filename in changes or signature_entry(entry.filename)):
                            continue
                        offset = output.tell()
                        start, end = _raw_extent(stream, entry, central[entry.filename])
                        if end - start + offset >= limit:
                            raise ApkZipError("the APK would exceed 4 GiB; ZIP64 is not supported")
                        stream.seek(start)
                        while start < end:
                            block = stream.read(min(end - start, BLOCK))
                            if not block:
                                raise ApkZipError(f"truncated ZIP entry: {entry.filename}")
                            output.write(block)
                            start += len(block)
                        record = bytearray(central[entry.filename])
                        struct.pack_into("<I", record, 42, offset)
                        records.append(record)
                directory = output.tell()
                for record in records:
                    output.write(record)
                size = output.tell() - directory
                if output.tell() >= limit or len(records) >= 0xFFFF:
                    raise ApkZipError("the APK would need ZIP64; it is not supported")
                output.write(
                    struct.pack("<4s4H2IH", b"PK\x05\x06", 0, 0, len(records), len(records), size, directory, 0)
                )
    except BaseException:
        if created:
            try:
                os.remove(target)
            except OSError:
                pass
        raise


def verify_payload(source: Path, candidate: Path, changes: dict[str, bytes]) -> dict:
    """Check that candidate equals source except for changes and old signature files."""
    with open(source, "rb") as before_stream, open(candidate, "rb") as after_stream, zipfile.ZipFile(
        before_stream
    ) as before, zipfile.ZipFile(after_stream) as after:
        before_entries = inventory(before)
        after_entries = inventory(after)
        before_central = _central_records(before_stream, before)
        after_central = _central_records(after_stream, after)
        removed = {name for name in before_entries if signature_entry(name)}
        leftover = sorted(name for name in after_entries if signature_entry(name))
        if leftover:
            raise ApkZipError(f"signature files left in {Path(candidate).name}: {leftover}")
        expected = (set(before_entries) - removed) | set(changes)
        if set(after_entries) != expected:
            difference = sorted(set(after_entries) ^ expected)
            raise ApkZipError(f"unexpected entry set in {Path(candidate).name}: {difference[:20]}")
        unchanged = sorted(set(before_entries) - removed - set(changes))
        for name in unchanged:
            old, new = before_entries[name], after_entries[name]
            if (old.file_size, old.compress_size, old.CRC, old.compress_type) != (
                new.file_size,
                new.compress_size,
                new.CRC,
                new.compress_type,
            ):
                raise ApkZipError(f"entry metadata changed: {name}")
            if compressed_sha256(before_stream, old, before_central[name]) != compressed_sha256(
                after_stream, new, after_central[name]
            ):
                raise ApkZipError(f"entry bytes changed: {name}")
        for name, data in changes.items():
            digest = hashlib.sha256()
            size = 0
            with after.open(name) as stream:
                for block in iter(lambda: stream.read(BLOCK), b""):
                    digest.update(block)
                    size += len(block)
            if size != len(data) or digest.digest() != hashlib.sha256(data).digest():
                raise ApkZipError(f"entry content differs from the build: {name}")
        return {
            "unchanged": len(unchanged),
            "replaced": sorted(set(changes) & set(before_entries)),
            "added": sorted(set(changes) - set(before_entries)),
            "removed": sorted(removed),
        }


def entry_digest(path: Path, name: str, destination: Path | None = None) -> tuple[int, str]:
    """Size and sha256 of an entry, optionally streaming it to destination."""
    digest = hashlib.sha256()
    size = 0
    with zipfile.ZipFile(path) as archive:
        try:
            info = archive.getinfo(name)
        except KeyError as error:
            raise ApkZipError(f"{Path(path).name} has no {name}") from error
        output = open(destination, "wb") if destination is not None else None
        try:
            with archive.open(info) as stream:
                for block in iter(lambda: stream.read(BLOCK), b""):
                    digest.update(block)
                    size += len(block)
                    if output is not None:
                        output.write(block)
        finally:
            if output is not None:
                output.close()
    return size, digest.hexdigest()


def _length_prefixed(data: bytes, pos: int) -> tuple[bytes, int]:
    if pos + 4 > len(data):
        raise ApkZipError("truncated APK signature record")
    length = struct.unpack_from("<I", data, pos)[0]
    start = pos + 4
    if start + length > len(data):
        raise ApkZipError("APK signature record exceeds its block")
    return data[start:start + length], start + length


def _signing_block(path: Path) -> bytes:
    with open(path, "rb") as stream:
        stream.seek(0, os.SEEK_END)
        size = stream.tell()
        tail_size = min(size, 0xFFFF + 22)
        stream.seek(size - tail_size)
        tail = stream.read(tail_size)
        eocd = tail.rfind(b"PK\x05\x06")
        if eocd < 0 or eocd + 22 > len(tail):
            raise ApkZipError(f"{Path(path).name} has no ZIP end record")
        directory = struct.unpack_from("<I", tail, eocd + 16)[0]
        if directory < 32:
            raise ApkZipError(f"{Path(path).name} has no APK signing block")
        stream.seek(directory - 24)
        footer = stream.read(24)
        if footer[8:] != APK_SIG_BLOCK_MAGIC:
            raise ApkZipError(f"{Path(path).name} has no APK signing block")
        block_size = struct.unpack_from("<Q", footer, 0)[0]
        if block_size < 24 or block_size + 8 > directory:
            raise ApkZipError(f"{Path(path).name} has an invalid APK signing block")
        stream.seek(directory - block_size - 8)
        block = stream.read(block_size + 8)
        if struct.unpack_from("<Q", block, 0)[0] != block_size:
            raise ApkZipError(f"{Path(path).name} has an inconsistent APK signing block")
        return block[8:-24]


def signer_certificate(path: Path) -> bytes:
    """DER certificate of the first signer in the v3, v3.1 or v2 signature scheme block."""
    pairs = _signing_block(path)
    schemes: dict[int, bytes] = {}
    pos = 0
    while pos < len(pairs):
        if pos + 12 > len(pairs):
            raise ApkZipError("truncated APK signing block entry")
        length = struct.unpack_from("<Q", pairs, pos)[0]
        if length < 4 or pos + 8 + length > len(pairs):
            raise ApkZipError("invalid APK signing block entry")
        identifier = struct.unpack_from("<I", pairs, pos + 8)[0]
        schemes[identifier] = pairs[pos + 12:pos + 8 + length]
        pos += 8 + length
    for scheme in SIGNATURE_SCHEMES:
        if scheme in schemes:
            signers, _ = _length_prefixed(schemes[scheme], 0)
            signer, _ = _length_prefixed(signers, 0)
            signed_data, _ = _length_prefixed(signer, 0)
            _, after_digests = _length_prefixed(signed_data, 0)
            certificates, _ = _length_prefixed(signed_data, after_digests)
            certificate, _ = _length_prefixed(certificates, 0)
            return certificate
    raise ApkZipError(f"{Path(path).name} has no v2 or v3 signature")


def java_hash_code(data: bytes) -> str:
    """Java Arrays.hashCode(byte[]) as Integer.toHexString prints it."""
    value = 1
    for byte in data:
        value = (31 * value + (byte - 256 if byte > 127 else byte)) & 0xFFFFFFFF
    return format(value, "x")
