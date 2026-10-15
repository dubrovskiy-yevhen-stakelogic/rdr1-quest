"""GameManager null guard in NativeGameActivity.activateGameMode()V.

The method is located through the DEX tables and its exact instruction pattern is
checked before a same-size rewrite. Native declarations have no DEX body to
rewrite and are preserved. Every other byte except the header checksum and
signature stays unchanged.
"""

from __future__ import annotations

import hashlib
import struct
import zlib

OWNER = "Lcom/rockstargames/rdr/NativeGameActivity;"
METHOD = "activateGameMode"
GETTER = "getContext"
CONTEXT_FIELD = "s_context"
CONTEXT = "Landroid/content/Context;"
GAME_MANAGER = "Landroid/app/GameManager;"
ACC_STATIC = 0x0008
ACC_NATIVE = 0x0100
ACC_ABSTRACT = 0x0400
HEADER_SIZE = 0x70
NO_INDEX = 0xFFFFFFFF


class DexError(ValueError):
    """DEX file that does not have the expected structure."""


def _uleb128(data: bytes, pos: int) -> tuple[int, int]:
    value = 0
    for shift in range(0, 35, 7):
        if pos >= len(data):
            raise DexError("truncated ULEB128 value")
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            return value, pos
    raise DexError("invalid ULEB128 value")


class Dex:
    """Read-only view of the DEX tables used by the patch and by input checks."""

    def __init__(self, data: bytes):
        if len(data) < HEADER_SIZE or data[:4] != b"dex\n" or data[7] != 0:
            raise DexError("not a DEX file")
        self.data = data
        if self.u32(32) != len(data) or self.u32(36) != HEADER_SIZE or self.u32(40) != 0x12345678:
            raise DexError("unsupported DEX header")
        self.string_count, self.string_offset = self.u32(56), self.u32(60)
        self.type_count, self.type_offset = self.u32(64), self.u32(68)
        self.proto_count, self.proto_offset = self.u32(72), self.u32(76)
        self.field_count, self.field_offset = self.u32(80), self.u32(84)
        self.method_count, self.method_offset = self.u32(88), self.u32(92)
        self.class_count, self.class_offset = self.u32(96), self.u32(100)
        for count, offset, size in (
            (self.string_count, self.string_offset, 4),
            (self.type_count, self.type_offset, 4),
            (self.proto_count, self.proto_offset, 12),
            (self.field_count, self.field_offset, 8),
            (self.method_count, self.method_offset, 8),
            (self.class_count, self.class_offset, 32),
        ):
            if count and (offset < HEADER_SIZE or offset + count * size > len(data)):
                raise DexError("DEX table exceeds the file")
        self._strings: dict[int, str] = {}

    def u32(self, pos: int) -> int:
        return struct.unpack_from("<I", self.data, pos)[0]

    def u16(self, pos: int) -> int:
        return struct.unpack_from("<H", self.data, pos)[0]

    def string(self, index: int) -> str:
        if index >= self.string_count:
            raise DexError("string index out of range")
        if index not in self._strings:
            offset = self.u32(self.string_offset + 4 * index)
            _, pos = _uleb128(self.data, offset)
            end = self.data.find(b"\0", pos)
            if end < 0:
                raise DexError("unterminated DEX string")
            self._strings[index] = self.data[pos:end].decode("utf-8", errors="replace")
        return self._strings[index]

    def type(self, index: int) -> str:
        if index >= self.type_count:
            raise DexError("type index out of range")
        return self.string(self.u32(self.type_offset + 4 * index))

    def types(self) -> list[str]:
        return [self.type(i) for i in range(self.type_count)]

    def proto(self, index: int) -> tuple[str, tuple[str, ...]]:
        if index >= self.proto_count:
            raise DexError("prototype index out of range")
        base = self.proto_offset + 12 * index
        return_type = self.type(self.u32(base + 4))
        parameters_offset = self.u32(base + 8)
        parameters: tuple[str, ...] = ()
        if parameters_offset:
            count = self.u32(parameters_offset)
            parameters = tuple(self.type(self.u16(parameters_offset + 4 + 2 * i)) for i in range(count))
        return return_type, parameters

    def method(self, index: int) -> tuple[str, str, tuple[str, tuple[str, ...]]]:
        if index >= self.method_count:
            raise DexError("method index out of range")
        owner, proto, name = struct.unpack_from("<HHI", self.data, self.method_offset + 8 * index)
        return self.type(owner), self.string(name), self.proto(proto)

    def field(self, index: int) -> tuple[str, str, str]:
        if index >= self.field_count:
            raise DexError("field index out of range")
        owner, kind, name = struct.unpack_from("<HHI", self.data, self.field_offset + 8 * index)
        return self.type(owner), self.type(kind), self.string(name)

    def class_def(self, descriptor: str) -> int | None:
        """Offset of the class_def_item that defines descriptor, or None."""
        found = [
            self.class_offset + 32 * i
            for i in range(self.class_count)
            if self.type(self.u32(self.class_offset + 32 * i)) == descriptor
        ]
        if len(found) > 1:
            raise DexError(f"{descriptor} is defined more than once")
        return found[0] if found else None

    def class_info(self, descriptor: str) -> tuple[int, str | None] | None:
        """(access flags, superclass descriptor) of a defined class, or None."""
        offset = self.class_def(descriptor)
        if offset is None:
            return None
        superclass = self.u32(offset + 8)
        return self.u32(offset + 4), None if superclass == NO_INDEX else self.type(superclass)

    def methods(self, descriptor: str) -> list[tuple[int, int, int]]:
        """(method index, access flags, code offset) for every method of a class."""
        offset = self.class_def(descriptor)
        if offset is None:
            return []
        data_offset = self.u32(offset + 24)
        if not data_offset:
            return []
        pos = data_offset
        counts = []
        for _ in range(4):
            value, pos = _uleb128(self.data, pos)
            counts.append(value)
        for _ in range(counts[0] + counts[1]):
            _, pos = _uleb128(self.data, pos)
            _, pos = _uleb128(self.data, pos)
        result = []
        for count in counts[2:]:
            index = 0
            for _ in range(count):
                delta, pos = _uleb128(self.data, pos)
                index += delta
                flags, pos = _uleb128(self.data, pos)
                code, pos = _uleb128(self.data, pos)
                result.append((index, flags, code))
        return result

    def code(self, descriptor: str, name: str, proto: tuple[str, tuple[str, ...]]) -> tuple[int, int, int]:
        found = [
            entry
            for entry in self.methods(descriptor)
            if self.method(entry[0]) == (descriptor, name, proto)
        ]
        if len(found) != 1 or not found[0][2]:
            raise DexError(f"{descriptor}->{name} is missing or has no code")
        return found[0]

    def units(self, code_offset: int) -> list[int]:
        if code_offset + 16 > len(self.data):
            raise DexError("code item exceeds the file")
        count = self.u32(code_offset + 12)
        if code_offset + 16 + 2 * count > len(self.data):
            raise DexError("instructions exceed the file")
        return list(struct.unpack_from("<" + "H" * count, self.data, code_offset + 16))


def checksums_valid(data: bytes) -> bool:
    if len(data) < HEADER_SIZE:
        return False
    adler = struct.unpack_from("<I", data, 8)[0]
    return data[12:32] == hashlib.sha1(data[32:]).digest() and adler == zlib.adler32(data[12:]) & 0xFFFFFFFF


def defines_owner(data: bytes) -> bool:
    return Dex(data).class_def(OWNER) is not None


def patch_game_manager(data: bytes) -> tuple[bytes, dict]:
    """Return the patched DEX and a short description of the edit.

    activateGameMode() reads the context through a static accessor and calls
    getSystemService(GameManager.class).getGameMode(). The service is null on
    Quest, so the accessor is inlined (4 -> 2 code units) and the two freed units
    hold if-eqz v0, +5, which skips the query and keeps every later address.
    """
    if not checksums_valid(data):
        raise DexError("the game DEX checksum or signature does not match its contents")
    dex = Dex(data)
    if dex.class_def(OWNER) is None:
        raise DexError(f"this DEX does not define {OWNER}")
    matches = [entry for entry in dex.methods(OWNER)
               if dex.method(entry[0]) == (OWNER, METHOD, ("V", ()))]
    if len(matches) == 1 and matches[0][1] & ACC_NATIVE:
        _, flags, code = matches[0]
        if code or not flags & ACC_STATIC or flags & ACC_ABSTRACT:
            raise DexError(f"unexpected native {METHOD} declaration")
        digest = hashlib.sha256(data).hexdigest()
        return data, {
            "owner": OWNER,
            "method": f"{METHOD}()V",
            "status": "native-method-unchanged",
            "runtimeGuard": "skip-native-query-when-game-manager-unavailable",
            "codeItemOffset": 0,
            "changedRange": None,
            "inputSha256": digest,
            "outputSha256": digest,
        }
    _, flags, code = dex.code(OWNER, METHOD, ("V", ()))
    _, _, getter = dex.code(OWNER, GETTER, (CONTEXT, ()))
    getter_words = dex.units(getter)
    if len(getter_words) != 3 or getter_words[0] != 0x0062 or getter_words[2] != 0x0011:
        raise DexError(f"{GETTER} is not a plain static field accessor")
    field = getter_words[1]
    if dex.field(field) != (OWNER, CONTEXT, CONTEXT_FIELD):
        raise DexError(f"{GETTER} does not read {CONTEXT_FIELD}")
    words = dex.units(code)
    if not flags & ACC_STATIC or len(words) != 24 or struct.unpack_from("<4H", data, code) != (2, 0, 2, 1):
        raise DexError(f"unexpected {METHOD} code layout")
    # invoke-static getContext; move-result-object v0; const-class v1, GameManager;
    # invoke-virtual getSystemService; move-result-object v0; check-cast v0, GameManager;
    # invoke-virtual getGameMode; return-void.
    if (
        words[0] != 0x0071
        or words[2:5] != [0x0000, 0x000C, 0x011C]
        or words[6] != 0x206E
        or words[8:11] != [0x0010, 0x000C, 0x001F]
        or words[12] != 0x106E
        or words[14:16] != [0x0000, 0x000E]
    ):
        raise DexError(f"unexpected {METHOD} instructions")
    if dex.method(words[1]) != (OWNER, GETTER, (CONTEXT, ())):
        raise DexError(f"{METHOD} does not call {GETTER}")
    if dex.type(words[5]) != GAME_MANAGER or words[11] != words[5]:
        raise DexError(f"{METHOD} does not query {GAME_MANAGER}")
    if dex.method(words[7]) != (CONTEXT, "getSystemService", ("Ljava/lang/Object;", ("Ljava/lang/Class;",))):
        raise DexError(f"{METHOD} does not call getSystemService(Class)")
    if dex.method(words[13]) != (GAME_MANAGER, "getGameMode", ("I", ())):
        raise DexError(f"{METHOD} does not call GameManager.getGameMode()")
    replacement = [0x0062, field, *words[4:12], 0x0038, 0x0005]
    if len(replacement) != 12:
        raise DexError("replacement has the wrong size")
    start = code + 16
    result = bytearray(data)
    result[start:start + 24] = struct.pack("<12H", *replacement)
    result[12:32] = hashlib.sha1(result[32:]).digest()
    struct.pack_into("<I", result, 8, zlib.adler32(result[12:]) & 0xFFFFFFFF)
    if (
        len(result) != len(data)
        or result[:8] != data[:8]
        or result[32:start] != data[32:start]
        or result[start + 24:] != data[start + 24:]
    ):
        raise DexError("the DEX edit escaped its instruction range")
    return bytes(result), {
        "owner": OWNER,
        "method": f"{METHOD}()V",
        "status": "null-guard-applied",
        "codeItemOffset": code,
        "changedRange": [start, start + 24],
        "inputSha256": hashlib.sha256(data).hexdigest(),
        "outputSha256": hashlib.sha256(result).hexdigest(),
    }
