"""Read and edit Android binary XML (AXML) manifests.

Edits are narrow: existing chunks keep their bytes, new strings are appended to the
string pool and new nodes are inserted as complete start/end element pairs.
Layout follows ResStringPool_header, ResXMLTree_node and ResXMLTree_attrExt.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

ANDROID_NS = "http://schemas.android.com/apk/res/android"
NO_INDEX = 0xFFFFFFFF

CHUNK_STRING_POOL = 0x0001
CHUNK_XML = 0x0003
CHUNK_START_NAMESPACE = 0x0100
CHUNK_END_NAMESPACE = 0x0101
CHUNK_START_ELEMENT = 0x0102
CHUNK_END_ELEMENT = 0x0103
CHUNK_CDATA = 0x0104
CHUNK_RESOURCE_MAP = 0x0180

TYPE_STRING = 0x03
TYPE_INT_DEC = 0x10
TYPE_INT_HEX = 0x11
TYPE_BOOLEAN = 0x12

BOOL_TRUE = 0xFFFFFFFF

# Framework attribute resource IDs (android.R.attr).
ATTRIBUTE_IDS = {
    "theme": 0x01010000,
    "label": 0x01010001,
    "name": 0x01010003,
    "exported": 0x01010010,
    "screenOrientation": 0x0101001E,
    "authorities": 0x01010018,
    "value": 0x01010024,
    "targetActivity": 0x01010202,
    "versionCode": 0x0101021B,
    "versionName": 0x0101021C,
    "version": 0x01010219,
    "required": 0x0101028E,
    "extractNativeLibs": 0x010104EA,
}

UTF8_FLAG = 0x100
SORTED_FLAG = 0x1
ATTRIBUTE_SIZE = 20


class AxmlError(ValueError):
    """Malformed or unsupported binary XML."""


@dataclass
class Attribute:
    offset: int
    namespace: str | None
    name: str
    resource_id: int
    raw: int
    type: int
    data: int
    string: str | None

    def value(self):
        if self.type == TYPE_STRING:
            return self.string
        if self.type == TYPE_BOOLEAN:
            return self.data != 0
        if self.type == TYPE_INT_DEC:
            return self.data - (1 << 32) if self.data & 0x80000000 else self.data
        return self.data


@dataclass
class Element:
    tag: str
    start: int
    end: int = -1
    parent: Element | None = None
    children: list[Element] = field(default_factory=list)
    attributes: list[Attribute] = field(default_factory=list)

    def attribute(self, name: str) -> Attribute | None:
        wanted = ATTRIBUTE_IDS.get(name)
        matches = [
            attr
            for attr in self.attributes
            if (wanted is not None and attr.resource_id == wanted)
            or (attr.resource_id == 0 and attr.namespace == ANDROID_NS and attr.name == name)
        ]
        if len(matches) > 1:
            raise AxmlError(f"duplicate android:{name} on <{self.tag}>")
        return matches[0] if matches else None

    def get(self, name: str):
        attr = self.attribute(name)
        return None if attr is None else attr.value()

    def find(self, tag: str) -> list[Element]:
        return [child for child in self.children if child.tag == tag]

    def walk(self):
        yield self
        for child in self.children:
            yield from child.walk()


def _read_length(data, pos: int, utf8: bool) -> tuple[int, int]:
    if utf8:
        first = data[pos]
        if first & 0x80:
            return ((first & 0x7F) << 8) | data[pos + 1], pos + 2
        return first, pos + 1
    first = struct.unpack_from("<H", data, pos)[0]
    if first & 0x8000:
        second = struct.unpack_from("<H", data, pos + 2)[0]
        return ((first & 0x7FFF) << 16) | second, pos + 4
    return first, pos + 2


def _encode_length(value: int, utf8: bool) -> bytes:
    if utf8:
        if value > 0x7FFF:
            raise AxmlError("string is too long for a UTF-8 pool")
        return bytes([value]) if value < 0x80 else bytes([0x80 | (value >> 8), value & 0xFF])
    if value > 0x7FFFFFFF:
        raise AxmlError("string is too long")
    if value < 0x8000:
        return struct.pack("<H", value)
    return struct.pack("<HH", 0x8000 | (value >> 16), value & 0xFFFF)


def _encode_string(value: str, utf8: bool) -> bytes:
    if utf8:
        encoded = value.encode("utf-8")
        units = len(value.encode("utf-16-le")) // 2
        return _encode_length(units, True) + _encode_length(len(encoded), True) + encoded + b"\0"
    encoded = value.encode("utf-16-le")
    return _encode_length(len(encoded) // 2, False) + encoded + b"\0\0"


def _decode_pool(pool: bytes | bytearray) -> tuple[list[str], bool]:
    if len(pool) < 28:
        raise AxmlError("truncated string pool")
    _, header, size, count, styles, flags, start, style_start = struct.unpack_from("<HHIIIIII", pool)
    if header != 28 or size != len(pool):
        raise AxmlError("unsupported string pool header")
    if styles or style_start:
        raise AxmlError("styled string pools are not supported")
    if 28 + 4 * count > len(pool) or start > len(pool):
        raise AxmlError("string pool offsets exceed the chunk")
    utf8 = bool(flags & UTF8_FLAG)
    result = []
    for index in range(count):
        offset = struct.unpack_from("<I", pool, 28 + 4 * index)[0]
        pos = start + offset
        if pos >= len(pool):
            raise AxmlError("string offset exceeds the pool")
        length, pos = _read_length(pool, pos, utf8)
        if utf8:
            length, pos = _read_length(pool, pos, True)
            end = pos + length
        else:
            end = pos + 2 * length
        if end > len(pool):
            raise AxmlError("string data exceeds the pool")
        raw = bytes(pool[pos:end])
        result.append(raw.decode("utf-8" if utf8 else "utf-16-le", errors="replace"))
    return result, utf8


class Document:
    """Chunk list of one binary XML file with a single string pool and resource map."""

    def __init__(self, data: bytes):
        if len(data) < 8:
            raise AxmlError("not an Android binary XML file")
        kind, header, size = struct.unpack_from("<HHI", data)
        if kind != CHUNK_XML or header != 8 or size != len(data):
            raise AxmlError("not a complete Android binary XML file")
        self.parts: list[list] = []
        pos = 8
        while pos < len(data):
            if pos + 8 > len(data):
                raise AxmlError("truncated binary XML chunk")
            kind, header, size = struct.unpack_from("<HHI", data, pos)
            if header < 8 or size < header or size % 4 or pos + size > len(data):
                raise AxmlError("invalid binary XML chunk")
            self.parts.append([kind, bytearray(data[pos:pos + size])])
            pos += size
        pools = [i for i, (kind, _) in enumerate(self.parts) if kind == CHUNK_STRING_POOL]
        maps = [i for i, (kind, _) in enumerate(self.parts) if kind == CHUNK_RESOURCE_MAP]
        if len(pools) != 1 or pools[0] != 0:
            raise AxmlError("expected one leading string pool")
        if len(maps) != 1:
            raise AxmlError("expected one resource map")
        self.pool_index = pools[0]
        self.map_index = maps[0]
        self.strings, self.utf8 = _decode_pool(self.parts[self.pool_index][1])
        resource_map = self.parts[self.map_index][1]
        self.ids = list(struct.unpack_from("<" + "I" * ((len(resource_map) - 8) // 4), resource_map, 8))
        for kind, chunk in self.parts:
            if kind == CHUNK_START_ELEMENT:
                start, size = struct.unpack_from("<HH", chunk, 24)
                if start != 20 or size != ATTRIBUTE_SIZE:
                    raise AxmlError("unsupported attribute layout")
                count = struct.unpack_from("<H", chunk, 28)[0]
                if 36 + count * ATTRIBUTE_SIZE > len(chunk):
                    raise AxmlError("attributes exceed the element chunk")

    def to_bytes(self) -> bytes:
        body = b"".join(bytes(chunk) for _, chunk in self.parts)
        return struct.pack("<HHI", CHUNK_XML, 8, len(body) + 8) + body

    # String pool and resource map.

    def resource_id(self, index: int) -> int:
        return self.ids[index] if 0 <= index < len(self.ids) else 0

    def text(self, index: int) -> str | None:
        if index == NO_INDEX:
            return None
        if index >= len(self.strings):
            raise AxmlError("string index out of range")
        return self.strings[index]

    def string(self, value: str, resource_id: int = 0) -> int:
        """Index of value with the given resource ID, appending it when absent."""
        for index, existing in enumerate(self.strings):
            if existing == value and self.resource_id(index) == resource_id:
                return index
        pool = self.parts[self.pool_index][1]
        _, _, _, count, _, flags, start, _ = struct.unpack_from("<HHIIIIII", pool)
        offsets = bytes(pool[28:28 + 4 * count])
        payload = bytes(pool[start:])
        new_start = 28 + 4 * (count + 1)
        output = bytearray(
            struct.pack("<HHIIIIII", CHUNK_STRING_POOL, 28, 0, count + 1, 0, flags & ~SORTED_FLAG, new_start, 0)
        )
        output += offsets + struct.pack("<I", len(payload)) + payload + _encode_string(value, self.utf8)
        output += b"\0" * (-len(output) % 4)
        struct.pack_into("<I", output, 4, len(output))
        self.parts[self.pool_index][1] = output
        self.strings.append(value)
        index = count
        if resource_id:
            if index < len(self.ids):
                raise AxmlError("resource map already covers a new string")
            self.ids.extend([0] * (index + 1 - len(self.ids)))
            self.ids[index] = resource_id
            self._write_map()
        return index

    def _write_map(self) -> None:
        chunk = struct.pack("<HHI", CHUNK_RESOURCE_MAP, 8, 8 + 4 * len(self.ids))
        chunk += struct.pack("<" + "I" * len(self.ids), *self.ids)
        self.parts[self.map_index][1] = bytearray(chunk)

    def attribute_name(self, name: str) -> int:
        resource_id = ATTRIBUTE_IDS[name]
        for index in range(len(self.strings)):
            if self.resource_id(index) == resource_id:
                return index
        return self.string(name, resource_id)

    # Tree view. Indices refer to self.parts and change after every insertion.

    def _attributes(self, chunk: bytearray) -> list[Attribute]:
        count = struct.unpack_from("<H", chunk, 28)[0]
        result = []
        for j in range(count):
            offset = 36 + j * ATTRIBUTE_SIZE
            ns, name, raw, _, _, kind, data = struct.unpack_from("<IIIHBBI", chunk, offset)
            result.append(
                Attribute(
                    offset=offset,
                    namespace=self.text(ns),
                    name=self.text(name) or "",
                    resource_id=self.resource_id(name),
                    raw=raw,
                    type=kind,
                    data=data,
                    string=self.text(data) if kind == TYPE_STRING else None,
                )
            )
        return result

    def tree(self) -> Element:
        roots: list[Element] = []
        stack: list[Element] = []
        for index, (kind, chunk) in enumerate(self.parts):
            if kind == CHUNK_START_ELEMENT:
                element = Element(
                    tag=self.text(struct.unpack_from("<I", chunk, 20)[0]) or "",
                    start=index,
                    parent=stack[-1] if stack else None,
                    attributes=self._attributes(chunk),
                )
                (stack[-1].children if stack else roots).append(element)
                stack.append(element)
            elif kind == CHUNK_END_ELEMENT:
                tag = self.text(struct.unpack_from("<I", chunk, 20)[0])
                if not stack or stack[-1].tag != tag:
                    raise AxmlError("unbalanced binary XML elements")
                stack.pop().end = index
        if stack or len(roots) != 1 or roots[0].tag != "manifest":
            raise AxmlError("expected one <manifest> root element")
        return roots[0]

    # Node construction and editing.

    def element_chunks(self, tag: str, attributes=(), line: int = 0) -> list[list]:
        """Start/end chunks for <tag> with android: attributes (name, type, value)."""
        namespace = self.string(ANDROID_NS)
        tag_index = self.string(tag)
        encoded = []
        for name, kind, value in attributes:
            key = self.attribute_name(name)
            if kind == TYPE_STRING:
                data = self.string(value)
                raw = data
            else:
                data = value & 0xFFFFFFFF
                raw = NO_INDEX
            encoded.append((ATTRIBUTE_IDS[name], struct.pack("<IIIHBBI", namespace, key, raw, 8, 0, kind, data)))
        encoded.sort(key=lambda item: item[0])
        start = struct.pack("<HHIII", CHUNK_START_ELEMENT, 16, 36 + ATTRIBUTE_SIZE * len(encoded), line, NO_INDEX)
        start += struct.pack("<IIHHHHHH", NO_INDEX, tag_index, 20, ATTRIBUTE_SIZE, len(encoded), 0, 0, 0)
        start += b"".join(item for _, item in encoded)
        end = struct.pack("<HHIIIII", CHUNK_END_ELEMENT, 16, 24, line, NO_INDEX, NO_INDEX, tag_index)
        return [[CHUNK_START_ELEMENT, bytearray(start)], [CHUNK_END_ELEMENT, bytearray(end)]]

    def insert(self, index: int, chunks: list[list]) -> None:
        self.parts[index:index] = chunks

    def remove(self, element: Element) -> None:
        if element.end < element.start:
            raise AxmlError("element has no end chunk")
        del self.parts[element.start:element.end + 1]

    def set_attribute(self, element: Element, name: str, kind: int, value) -> None:
        """Set android:name-style attribute on element, inserting it in resource ID order."""
        chunk = self.parts[element.start][1]
        if kind == TYPE_STRING:
            data = self.string(value)
            raw = data
        else:
            data = value & 0xFFFFFFFF
            raw = NO_INDEX
        existing = element.attribute(name)
        if existing is not None:
            struct.pack_into("<IHBBI", chunk, existing.offset + 8, raw, 8, 0, kind, data)
            return
        namespace = self.string(ANDROID_NS)
        key = self.attribute_name(name)
        resource_id = ATTRIBUTE_IDS[name]
        attributes = self._attributes(chunk)
        position = next((j for j, attr in enumerate(attributes) if attr.resource_id > resource_id), len(attributes))
        offset = 36 + position * ATTRIBUTE_SIZE
        record = struct.pack("<IIIHBBI", namespace, key, raw, 8, 0, kind, data)
        updated = chunk[:offset] + record + chunk[offset:]
        struct.pack_into("<I", updated, 4, len(updated))
        struct.pack_into("<H", updated, 28, len(attributes) + 1)
        for location in (30, 32, 34):
            special = struct.unpack_from("<H", updated, location)[0]
            if special and special > position:
                struct.pack_into("<H", updated, location, special + 1)
        self.parts[element.start][1] = updated


def qualify(name: str, package: str) -> str:
    """Fully qualified class name as PackageParser resolves android:name."""
    if name.startswith("."):
        return package + name
    if "." not in name:
        return f"{package}.{name}"
    return name
