#!/usr/bin/env python3
"""Self-tests for the APK tools used by build.py.

Without arguments, run the offline tests of the AXML editor, the manifest transform,
the DEX patch, the raw ZIP writer and the build helpers. With --sdk and --java-home,
also run aapt2, javac, d8 and apksigner checks and the whole build pipeline on a
synthetic game package and a synthetic source tree.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path

sys.dont_write_bytecode = True
TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import apkzip  # noqa: E402
import axml  # noqa: E402
import dexpatch  # noqa: E402
import xrmanifest  # noqa: E402

_spec = importlib.util.spec_from_file_location("rdrvr_build", TOOLS / "build.py")
build = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = build
_spec.loader.exec_module(build)

SETTINGS: dict[str, Path | None] = {"sdk": None, "java_home": None, "work": None}
EXE = ".exe" if os.name == "nt" else ""
PACKAGE = "com.rockstargames.rdr"
LAUNCHER = "com.rockstargames.rdr.LauncherActivity"
NATIVE = xrmanifest.NATIVE_ACTIVITY
EXTRA_IDS = {
    "minSdkVersion": 0x0101020C,
    "targetSdkVersion": 0x01010270,
    "hasCode": 0x0101000C,
    "configChanges": 0x0101001F,
    "isFeatureSplit": 0x0101055B,
}
IDS = {**axml.ATTRIBUTE_IDS, **EXTRA_IDS}


# Minimal binary XML encoder for synthetic manifests.


class Node:
    def __init__(self, tag, attrs=None, children=(), plain=None):
        self.tag = tag
        self.attrs = dict(attrs or {})
        self.children = list(children)
        self.plain = dict(plain or {})

    def walk(self):
        yield self
        for child in self.children:
            yield from child.walk()


def _pool(strings, utf8):
    offsets, data = [], bytearray()
    for value in strings:
        offsets.append(len(data))
        if utf8:
            raw = value.encode("utf-8")
            data += bytes([len(value), len(raw)]) + raw + b"\0"
        else:
            raw = value.encode("utf-16-le")
            data += struct.pack("<H", len(raw) // 2) + raw + b"\0\0"
    data += b"\0" * (-len(data) % 4)
    start = 28 + 4 * len(strings)
    body = struct.pack("<" + "I" * len(offsets), *offsets) + bytes(data)
    header = struct.pack("<HHIIIIII", 1, 28, 28 + len(body), len(strings), 0, 0x100 if utf8 else 0, start, 0)
    return header + body


def encode_manifest(root: Node, utf8: bool = True) -> bytes:
    names = sorted({name for node in root.walk() for name in node.attrs}, key=lambda n: IDS[n])
    strings = list(names)
    other: dict[str, int] = {}

    def index(value):
        if value not in other:
            other[value] = len(strings)
            strings.append(value)
        return other[value]

    prefix, uri = index("android"), index(axml.ANDROID_NS)
    for node in root.walk():
        index(node.tag)
        for key, value in node.plain.items():
            index(key)
            index(value)
        for value in node.attrs.values():
            if isinstance(value, str):
                index(value)
    no = axml.NO_INDEX
    chunks = [_pool(strings, utf8)]
    resource_map = b"".join(struct.pack("<I", IDS[n]) for n in names)
    chunks.append(struct.pack("<HHI", 0x180, 8, 8 + len(resource_map)) + resource_map)
    chunks.append(struct.pack("<HHIIIII", 0x100, 16, 24, 1, no, prefix, uri))

    def element(node, line):
        attrs = []
        for key, value in node.plain.items():
            attrs.append((0, struct.pack("<IIIHBBI", no, index(key), index(value), 8, 0, 3, index(value))))
        for key, value in node.attrs.items():
            if isinstance(value, bool):
                record = (no, axml.TYPE_BOOLEAN, 0xFFFFFFFF if value else 0)
            elif isinstance(value, int):
                record = (no, axml.TYPE_INT_DEC, value & 0xFFFFFFFF)
            else:
                record = (index(value), axml.TYPE_STRING, index(value))
            encoded = struct.pack("<IIIHBBI", uri, names.index(key), record[0], 8, 0, record[1], record[2])
            attrs.append((IDS[key], encoded))
        attrs.sort(key=lambda item: item[0])
        tag = index(node.tag)
        start = struct.pack("<HHIII", 0x102, 16, 36 + 20 * len(attrs), line, no)
        start += struct.pack("<IIHHHHHH", no, tag, 20, 20, len(attrs), 0, 0, 0) + b"".join(a for _, a in attrs)
        chunks.append(start)
        for child in node.children:
            element(child, line + 1)
        chunks.append(struct.pack("<HHIIIII", 0x103, 16, 24, line, no, no, tag))

    element(root, 2)
    chunks.append(struct.pack("<HHIIIII", 0x101, 16, 24, 1, no, prefix, uri))
    body = b"".join(chunks)
    return struct.pack("<HHI", 3, 8, 8 + len(body)) + body


def main_filter(*categories):
    nodes = [Node("action", {"name": xrmanifest.ACTION_MAIN})]
    nodes += [Node("category", {"name": c}) for c in categories]
    return Node("intent-filter", children=nodes)


def play_manifest(application=None, extract=False, alias=False, utf8=True, native_library="rdr",
                  second_launcher=False, native_exported=True) -> bytes:
    app_attrs = {"label": "RDR", "hasCode": True}
    if application:
        app_attrs["name"] = application
    if extract is not None:
        app_attrs["extractNativeLibs"] = extract
    launcher_children = [] if alias else [main_filter(xrmanifest.CATEGORY_LAUNCHER)]
    activities = [
        Node("activity", {"name": LAUNCHER, "exported": True, "screenOrientation": 6, "configChanges": 0xFA0},
             launcher_children),
    ]
    if alias:
        activities.append(
            Node("activity-alias", {"name": ".Main", "targetActivity": LAUNCHER, "exported": True},
                 [main_filter(xrmanifest.CATEGORY_LAUNCHER)])
        )
    native_attrs = {"name": NATIVE, "screenOrientation": 6}
    if native_exported:
        native_attrs["exported"] = False
    activities.append(
        Node("activity", native_attrs, [Node("meta-data", {"name": "android.app.lib_name", "value": native_library})])
    )
    if second_launcher:
        activities.append(
            Node("activity", {"name": "com.example.Other", "exported": True},
                 [main_filter(xrmanifest.CATEGORY_LAUNCHER)])
        )
    root = Node(
        "manifest",
        {"versionCode": 63226194, "versionName": "1.58.63226194"},
        [
            Node("uses-sdk", {"minSdkVersion": 29, "targetSdkVersion": 35}),
            Node("uses-permission", {"name": "android.permission.INTERNET"}),
            Node("application", app_attrs, activities),
        ],
        plain={"package": PACKAGE},
    )
    return encode_manifest(root, utf8)


def split_manifest(split: str) -> bytes:
    root = Node(
        "manifest",
        {"versionCode": 63226194},
        [Node("application", {"hasCode": False})],
        plain={"package": PACKAGE, "split": split},
    )
    return encode_manifest(root)


def access_manifest(trade_value: int = -1) -> bytes:
    root = Node(
        "manifest",
        {},
        [
            Node("uses-permission", {"name": xrmanifest.OPENXR_PERMISSION}),
            Node("queries", children=[
                Node("provider", {"authorities": xrmanifest.OPENXR_AUTHORITIES}),
                Node("intent", children=[Node("action", {"name": xrmanifest.OPENXR_SERVICE})]),
            ]),
            Node("application", children=[
                Node("meta-data", {"name": xrmanifest.TRADE_META, "value": trade_value}),
            ]),
        ],
        plain={"package": "org.rdrvr.access"},
    )
    return encode_manifest(root)


def component(element, package=PACKAGE):
    name = element.get("name")
    return axml.qualify(name, package) if name else None


def child_names(element, tag):
    return [c.get("name") for c in element.find(tag)]


# Minimal DEX writer for the GameManager pattern.


def _uleb(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def mini_dex(mutate=None, owner=dexpatch.OWNER, getter=(0x0062, 0, 0x0011),
             method_flags=0x000A, method_has_code=True) -> bytes:
    types = [owner, "Landroid/app/GameManager;", "Landroid/content/Context;", "Ljava/lang/Class;",
             "Ljava/lang/Object;", "V", "I", "Landroid/app/NativeActivity;"]
    names = ["activateGameMode", "getContext", "getSystemService", "getGameMode", "s_context"]
    strings = sorted(set(types + names + ["L", "LL"]))
    sid = {s: i for i, s in enumerate(strings)}
    protos = [("V", "V", None), ("L", types[2], None), ("LL", types[4], [3]), ("I", "I", None)]
    methods = [(0, 0, "activateGameMode"), (0, 1, "getContext"), (2, 2, "getSystemService"), (1, 3, "getGameMode")]
    words = [0x0071, 1, 0, 0x000C, 0x011C, 1, 0x206E, 2, 0x0010, 0x000C, 0x001F, 1,
             0x106E, 3, 0, 0x000E, 0x0062, 0, 0x011A, 0, 0x206E, 2, 0x0010, 0x000E]
    if mutate:
        mutate(words)
    layout = {"strings": 0x70}
    layout["types"] = layout["strings"] + 4 * len(strings)
    layout["protos"] = layout["types"] + 4 * len(types)
    layout["fields"] = layout["protos"] + 12 * len(protos)
    layout["methods"] = layout["fields"] + 8
    layout["classes"] = layout["methods"] + 8 * len(methods)
    data_start = layout["classes"] + 32
    data = bytearray()
    string_offsets = []
    for value in strings:
        string_offsets.append(data_start + len(data))
        data += _uleb(len(value)) + value.encode() + b"\0"
    data += b"\0" * (-(data_start + len(data)) % 4)
    type_list = data_start + len(data)
    data += struct.pack("<IH", 1, 3) + b"\0\0"
    code_a = data_start + len(data)
    data += struct.pack("<HHHHII", 2, 0, 2, 1, 0, len(words)) + struct.pack("<" + "H" * len(words), *words)
    data += struct.pack("<IHH", 0, 0x0F, 1) + bytes([1, 0, 0x10])
    data += b"\0" * (-(data_start + len(data)) % 4)
    code_b = data_start + len(data)
    data += struct.pack("<HHHHII", 1, 0, 0, 0, 0, 3) + struct.pack("<3H", *getter)
    class_data = data_start + len(data)
    data += _uleb(1) + _uleb(0) + _uleb(2) + _uleb(0) + _uleb(0) + _uleb(0x8)
    data += _uleb(0) + _uleb(method_flags) + _uleb(code_a if method_has_code else 0)
    data += _uleb(1) + _uleb(0x0009) + _uleb(code_b)
    out = bytearray(data_start)
    for i, offset in enumerate(string_offsets):
        struct.pack_into("<I", out, layout["strings"] + 4 * i, offset)
    for i, value in enumerate(types):
        struct.pack_into("<I", out, layout["types"] + 4 * i, sid[value])
    for i, (shorty, ret, params) in enumerate(protos):
        struct.pack_into("<III", out, layout["protos"] + 12 * i, sid[shorty], types.index(ret),
                         type_list if params else 0)
    struct.pack_into("<HHI", out, layout["fields"], 0, 2, sid["s_context"])
    for i, (cls, proto, name) in enumerate(methods):
        struct.pack_into("<HHI", out, layout["methods"] + 8 * i, cls, proto, sid[name])
    struct.pack_into("<8I", out, layout["classes"], 0, 1, 7, 0, axml.NO_INDEX, 0, class_data, 0)
    out += data
    header = struct.pack(
        "<8sI20sIIIIIIIIIIIIIIIIIIII", b"dex\n035\0", 0, bytes(20), len(out), 0x70, 0x12345678, 0, 0, 0,
        len(strings), layout["strings"], len(types), layout["types"], len(protos), layout["protos"],
        1, layout["fields"], len(methods), layout["methods"], 1, layout["classes"],
        len(out) - data_start, data_start,
    )
    out[:0x70] = header
    out[12:32] = hashlib.sha1(out[32:]).digest()
    struct.pack_into("<I", out, 8, zlib.adler32(bytes(out[12:])) & 0xFFFFFFFF)
    return bytes(out)


def mini_elf(build_id: bytes, machine: int = 183) -> bytes:
    note = struct.pack("<III", 4, len(build_id), 3) + b"GNU\0" + build_id + b"\0" * (-len(build_id) % 4)
    header = struct.pack("<4sBBBBB7sHHIQQQIHHHHHH", b"\x7fELF", 2, 1, 1, 0, 0, bytes(7), 3, machine, 1,
                         0, 64, 0, 0, 64, 56, 1, 64, 0, 0)
    program = struct.pack("<IIQQQQQQ", 4, 4, 120, 120, 120, len(note), len(note), 4)
    return header + program + note


def zip_bytes(entries: dict[str, bytes], method=zipfile.ZIP_DEFLATED) -> bytes:
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w", method) as archive:
        for name, data in entries.items():
            archive.writestr(name, data)
    return stream.getvalue()


def write_zip(path: Path, entries: dict[str, bytes], method=zipfile.ZIP_DEFLATED) -> Path:
    path.write_bytes(zip_bytes(entries, method))
    return path


def with_signing_block(data: bytes, certificate: bytes, scheme: int = 0xF05368C0) -> bytes:
    def lp(value):
        return struct.pack("<I", len(value)) + value

    signed_data = lp(b"") + lp(lp(certificate)) + b"\x1d\0\0\0"
    value = lp(lp(lp(signed_data) + lp(b"") + lp(b"")))
    pairs = struct.pack("<QI", 4 + len(value), scheme) + value
    size = len(pairs) + 24
    block = struct.pack("<Q", size) + pairs + struct.pack("<Q", size) + apkzip.APK_SIG_BLOCK_MAGIC
    eocd = data.rfind(b"PK\x05\x06")
    directory = struct.unpack_from("<I", data, eocd + 16)[0]
    result = bytearray(data[:directory] + block + data[directory:])
    struct.pack_into("<I", result, eocd + len(block) + 16, directory + len(block))
    return bytes(result)


class Unseekable(io.RawIOBase):
    """Write-only stream that makes zipfile emit data descriptors."""

    def __init__(self):
        self.buffer = bytearray()

    def writable(self):
        return True

    def write(self, data):
        self.buffer += data
        return len(data)


# Offline tests.


class AxmlTests(unittest.TestCase):
    def test_read_package_info(self):
        info = xrmanifest.read_package_info(play_manifest())
        self.assertEqual(info, xrmanifest.PackageInfo(PACKAGE, "63226194", "1.58.63226194", None))
        split = xrmanifest.read_package_info(split_manifest("config.arm64_v8a"))
        self.assertEqual((split.split, split.version_name), ("config.arm64_v8a", None))

    def test_string_append_keeps_existing_indices(self):
        for utf8 in (True, False):
            with self.subTest(utf8=utf8):
                doc = axml.Document(play_manifest(utf8=utf8))
                before = list(doc.strings)
                value = "org.example.Name" + chr(0xE9) + chr(0x4E2D)
                index = doc.string(value)
                again = axml.Document(doc.to_bytes())
                self.assertEqual(again.strings[: len(before)], before)
                self.assertEqual(again.strings[index], value)
                self.assertEqual(again.ids, doc.ids)

    def test_attribute_insert_order(self):
        doc = axml.Document(play_manifest(extract=None))
        application = doc.tree().find("application")[0]
        doc.set_attribute(application, "extractNativeLibs", axml.TYPE_BOOLEAN, axml.BOOL_TRUE)
        application = axml.Document(doc.to_bytes()).tree().find("application")[0]
        ids = [a.resource_id for a in application.attributes]
        self.assertEqual(ids, sorted(ids))
        self.assertIs(application.get("extractNativeLibs"), True)

    def test_qualify(self):
        self.assertEqual(axml.qualify(".Main", PACKAGE), PACKAGE + ".Main")
        self.assertEqual(axml.qualify("Main", PACKAGE), PACKAGE + ".Main")
        self.assertEqual(axml.qualify("a.b.C", PACKAGE), "a.b.C")

    def test_malformed_input_refused(self):
        data = play_manifest()
        for broken in (data[:-4], b"\x03\x00\x10\x00" + data[4:], data[:8] + b"\x99" + data[9:]):
            with self.assertRaises(axml.AxmlError):
                axml.Document(broken).tree()


class ManifestTests(unittest.TestCase):
    def transform(self, data, trade=0):
        result, info = xrmanifest.transform(data, access_manifest(), trade)
        return axml.Document(result).tree(), info

    def activity(self, application, name):
        found = [a for a in application.find("activity") if component(a) == name]
        self.assertEqual(len(found), 1, name)
        return found[0]

    def assert_immersive(self, activity):
        filters = [f for f in activity.find("intent-filter") if xrmanifest.ACTION_MAIN in child_names(f, "action")]
        self.assertEqual(len(filters), 1)
        categories = child_names(filters[0], "category")
        for category in xrmanifest.VR_CATEGORIES:
            self.assertEqual(categories.count(category), 1)
        focus = [m for m in activity.find("meta-data") if m.get("name") == xrmanifest.FOCUS_AWARE_META]
        self.assertEqual(len(focus), 1)
        self.assertIs(focus[0].get("value"), True)
        return categories

    def test_play_layout(self):
        for utf8 in (True, False):
            with self.subTest(utf8=utf8):
                root, info = self.transform(play_manifest(utf8=utf8))
                self.assertEqual(info.launcher, LAUNCHER)
                self.assertIsNone(info.application_class)
                application = root.find("application")[0]
                self.assertEqual(application.get("name"), xrmanifest.APPLICATION_CLASS)
                self.assertIs(application.get("extractNativeLibs"), True)
                tags = [c.tag for c in root.children]
                self.assertEqual(tags, ["uses-sdk", "uses-permission", "uses-permission", "queries", "uses-feature",
                                        "uses-permission", "uses-feature", "application"])
                children = [component(c) for c in application.children if c.tag == "activity"]
                self.assertEqual(children, [LAUNCHER, xrmanifest.PREFLIGHT_ACTIVITY, NATIVE])
                launcher = self.activity(application, LAUNCHER)
                self.assertNotIn(xrmanifest.CATEGORY_LAUNCHER, self.assert_immersive(launcher))
                preflight = self.activity(application, xrmanifest.PREFLIGHT_ACTIVITY)
                self.assertEqual(
                    {a.name: a.value() for a in preflight.attributes},
                    {"name": xrmanifest.PREFLIGHT_ACTIVITY, "exported": True, "screenOrientation": 6},
                )
                self.assertEqual(
                    self.assert_immersive(preflight),
                    [xrmanifest.CATEGORY_LAUNCHER, *xrmanifest.VR_CATEGORIES],
                )
                native = self.activity(application, NATIVE)
                self.assert_immersive(native)
                library = [m for m in native.find("meta-data") if m.get("name") == "android.app.lib_name"]
                self.assertEqual(library[0].get("value"), "rdrvr")
                trade = application.children[0]
                self.assertEqual((trade.tag, trade.get("name"), trade.get("value")),
                                 ("meta-data", xrmanifest.TRADE_META, 0))
                feature = [c for c in root.find("uses-feature") if c.get("name") == xrmanifest.HEADTRACKING_FEATURE]
                self.assertEqual((feature[0].get("required"), feature[0].get("version")), (True, 1))
                hand = [c for c in root.find("uses-feature") if c.get("name") == xrmanifest.HAND_TRACKING_FEATURE]
                self.assertIs(hand[0].get("required"), False)
                launchers = [c for c in root.walk() if c.tag == "category"
                             and c.get("name") == xrmanifest.CATEGORY_LAUNCHER]
                self.assertEqual(len(launchers), 1)

    def test_trade_values(self):
        for trade in (-1, 0, 1):
            root, _ = self.transform(play_manifest(), trade)
            self.assertEqual(root.find("application")[0].children[0].get("value"), trade)
        with self.assertRaises(xrmanifest.ManifestError):
            xrmanifest.transform(play_manifest(), access_manifest(), 2)

    def test_application_class_and_extract_flag(self):
        root, info = self.transform(play_manifest(application=".GameApp", extract=None))
        self.assertEqual(info.application_class, PACKAGE + ".GameApp")
        self.assertIsNone(info.extract_native_libs)
        application = root.find("application")[0]
        self.assertEqual(application.get("name"), xrmanifest.APPLICATION_CLASS)
        self.assertIs(application.get("extractNativeLibs"), True)

    def test_alias_launcher(self):
        root, info = self.transform(play_manifest(alias=True))
        self.assertEqual((info.launcher, info.launcher_tag), (LAUNCHER, "activity-alias"))
        application = root.find("application")[0]
        order = [(c.tag, component(c)) for c in application.children if c.tag.startswith("activity")]
        self.assertEqual(order, [("activity", LAUNCHER), ("activity-alias", PACKAGE + ".Main"),
                                 ("activity", xrmanifest.PREFLIGHT_ACTIVITY), ("activity", NATIVE)])
        alias = [c for c in application.children if c.tag == "activity-alias"][0]
        self.assertEqual(child_names(alias.find("intent-filter")[0], "category"), [])
        launcher = self.activity(application, LAUNCHER)
        self.assertEqual(self.assert_immersive(launcher), list(xrmanifest.VR_CATEGORIES))

    def test_refusals(self):
        cases = {
            "two launchers": play_manifest(second_launcher=True),
            "wrong native library": play_manifest(native_library="other"),
            "no exported flag": play_manifest(native_exported=False),
            "split": split_manifest("config.arm64_v8a"),
        }
        for label, data in cases.items():
            with self.subTest(label), self.assertRaises(xrmanifest.ManifestError):
                xrmanifest.transform(data, access_manifest(), 0)
        result, _ = xrmanifest.transform(play_manifest(), access_manifest(), 0)
        with self.assertRaises(xrmanifest.ManifestError):
            xrmanifest.transform(result, access_manifest(), 0)
        with self.assertRaises(xrmanifest.ManifestError):
            xrmanifest.inspect_base(play_manifest(application="org.rdrvr.Other"))

    def test_fragment_shape_enforced(self):
        bad = encode_manifest(Node("manifest", {}, [Node("application")], plain={"package": "x.y"}))
        with self.assertRaises(xrmanifest.ManifestError):
            xrmanifest.transform(play_manifest(), bad, 0)


class ApkZipTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def source(self) -> Path:
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, "w") as archive:
            archive.writestr("AndroidManifest.xml", b"manifest", zipfile.ZIP_DEFLATED)
            archive.writestr("resources.arsc", b"\x02" * 4000, zipfile.ZIP_STORED)
            archive.writestr("classes.dex", b"dex\n" + bytes(300), zipfile.ZIP_DEFLATED)
            archive.writestr("assets/data/a.rpf", os.urandom(5000), zipfile.ZIP_STORED)
            archive.writestr("META-INF/CERT.SF", b"sf")
            archive.writestr("META-INF/CERT.RSA", b"rsa")
            archive.writestr("META-INF/MANIFEST.MF", b"mf")
            archive.writestr("META-INF/services/x", b"kept")
            archive.writestr("stamp-cert-sha256", b"stamp")
        unseekable = Unseekable()
        with zipfile.ZipFile(unseekable, "w") as archive:
            archive.writestr("lib/arm64-v8a/libgame.so", b"\x7fELF" * 1000, zipfile.ZIP_DEFLATED)
        path = self.root / "source.apk"
        path.write_bytes(stream.getvalue())
        descriptor = self.root / "descriptor.apk"
        descriptor.write_bytes(bytes(unseekable.buffer))
        self.descriptor = descriptor
        return path

    def test_raw_copy_and_verify(self):
        source = self.source()
        changes = {
            "AndroidManifest.xml": (b"new manifest", zipfile.ZIP_DEFLATED),
            "classes2.dex": (b"dex\n" + bytes(64), zipfile.ZIP_STORED),
        }
        target = self.root / "out.apk"
        apkzip.write_apk(source, target, changes)
        report = apkzip.verify_payload(source, target, {k: v[0] for k, v in changes.items()})
        self.assertEqual(report["added"], ["classes2.dex"])
        self.assertEqual(report["replaced"], ["AndroidManifest.xml"])
        self.assertEqual(report["removed"], ["META-INF/CERT.RSA", "META-INF/CERT.SF", "META-INF/MANIFEST.MF",
                                             "stamp-cert-sha256"])
        with zipfile.ZipFile(target) as archive:
            self.assertEqual(archive.read("META-INF/services/x"), b"kept")
            self.assertEqual(archive.getinfo("resources.arsc").compress_type, zipfile.ZIP_STORED)
            self.assertIsNone(archive.testzip())
        target2 = self.root / "descriptor-out.apk"
        apkzip.write_apk(self.descriptor, target2, {"x.txt": (b"x", zipfile.ZIP_DEFLATED)})
        apkzip.verify_payload(self.descriptor, target2, {"x.txt": b"x"})

    def test_tamper_detected(self):
        source = self.source()
        target = self.root / "out.apk"
        apkzip.write_apk(source, target, {})
        data = bytearray(target.read_bytes())
        position = data.find(b"\x02" * 64)
        data[position] = 3
        target.write_bytes(bytes(data))
        with self.assertRaises(apkzip.ApkZipError):
            apkzip.verify_payload(source, target, {})

    def test_limits_and_targets(self):
        source = self.source()
        target = self.root / "big.apk"
        with self.assertRaises(apkzip.ApkZipError):
            apkzip.write_apk(source, target, {}, limit=2000)
        self.assertFalse(target.exists())
        existing = self.root / "existing.apk"
        existing.write_bytes(b"keep")
        with self.assertRaises(FileExistsError):
            apkzip.write_apk(source, existing, {})
        self.assertEqual(existing.read_bytes(), b"keep")
        with self.assertRaises(apkzip.ApkZipError):
            apkzip.write_apk(source, source, {})
        with self.assertRaises(apkzip.ApkZipError):
            apkzip.write_apk(source, self.root / "sig.apk", {"META-INF/A.RSA": (b"", zipfile.ZIP_STORED)})

    def test_signature_entries(self):
        for name in ("META-INF/MANIFEST.MF", "META-INF/KEY0.SF", "META-INF/x.rsa", "META-INF/B.EC",
                     "stamp-cert-sha256"):
            self.assertTrue(apkzip.signature_entry(name), name)
        for name in ("META-INF/services/a.RSA", "META-INF/x.version", "assets/META-INF/A.SF"):
            self.assertFalse(apkzip.signature_entry(name), name)

    def test_signing_block_and_hash_code(self):
        certificate = os.urandom(700)
        for scheme in (0xF05368C0, 0x7109871A):
            path = self.root / f"signed-{scheme:x}.apk"
            path.write_bytes(with_signing_block(zip_bytes({"a": b"b"}), certificate, scheme))
            self.assertEqual(apkzip.signer_certificate(path), certificate)
        unsigned = write_zip(self.root / "unsigned.apk", {"a": b"b"})
        with self.assertRaises(apkzip.ApkZipError):
            apkzip.signer_certificate(unsigned)
        self.assertEqual(apkzip.java_hash_code(b""), "1")
        self.assertEqual(apkzip.java_hash_code(bytes([1, 2, 3])), format(30817, "x"))
        self.assertEqual(apkzip.java_hash_code(b"\xff"), format((31 - 1) & 0xFFFFFFFF, "x"))
        self.assertEqual(apkzip.java_hash_code(b"\x80" * 8), format(self.java_reference(b"\x80" * 8), "x"))

    @staticmethod
    def java_reference(data):
        value = 1
        for byte in data:
            value = (value * 31 + (byte if byte < 128 else byte - 256)) % (1 << 32)
        return value


class DexTests(unittest.TestCase):
    def test_native_declaration_is_unchanged(self):
        data = mini_dex(method_flags=0x010A, method_has_code=False)
        result, report = dexpatch.patch_game_manager(data)
        self.assertEqual(result, data)
        self.assertEqual(report['status'], 'native-method-unchanged')
        self.assertIsNone(report['changedRange'])
        self.assertEqual(report['inputSha256'], report['outputSha256'])
        self.assertTrue(dexpatch.checksums_valid(result))

    def test_invalid_bodyless_declarations_are_refused(self):
        for flags, has_code in ((0x000A, False), (0x040A, False), (0x010A, True),
                                (0x0102, False), (0x050A, False)):
            with self.subTest(flags=flags, has_code=has_code), self.assertRaises(dexpatch.DexError):
                dexpatch.patch_game_manager(mini_dex(method_flags=flags, method_has_code=has_code))

    def test_patch(self):
        data = mini_dex()
        result, report = dexpatch.patch_game_manager(data)
        start, end = report["changedRange"]
        words = struct.unpack_from("<12H", result, start)
        self.assertEqual(words, (0x0062, 0, 0x011C, 1, 0x206E, 2, 0x0010, 0x000C, 0x001F, 1, 0x0038, 5))
        self.assertTrue(dexpatch.checksums_valid(result))
        self.assertEqual(len(result), len(data))
        self.assertEqual(result[32:start], data[32:start])
        self.assertEqual(result[end:], data[end:])
        with self.assertRaises(dexpatch.DexError):
            dexpatch.patch_game_manager(result)

    def test_refusals(self):
        def change(index, value):
            def mutate(words):
                words[index] = value
            return mutate

        cases = {
            "invoke": mini_dex(change(0, 0x0072)),
            "return": mini_dex(change(15, 0x0828)),
            "service": mini_dex(change(7, 3)),
            "getter": mini_dex(getter=(0x0062, 0, 0x0010)),
            "owner": mini_dex(owner="Lcom/example/Other;"),
        }
        for label, data in cases.items():
            with self.subTest(label), self.assertRaises(dexpatch.DexError):
                dexpatch.patch_game_manager(data)
        corrupt = bytearray(mini_dex())
        corrupt[-1] ^= 1
        with self.assertRaises(dexpatch.DexError):
            dexpatch.patch_game_manager(bytes(corrupt))
        self.assertFalse(dexpatch.defines_owner(cases["owner"]))
        self.assertIn("Landroid/app/GameManager;", dexpatch.Dex(mini_dex()).types())


class BuildHelperTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_dex_names(self):
        self.assertEqual(build.dex_names(["classes.dex", "classes2.dex", "a.dex"]), ["classes.dex", "classes2.dex"])
        for names in (["classes.dex", "classes3.dex"], ["classes2.dex"], ["classes.dex", "classes1.dex"]):
            with self.subTest(names=names), self.assertRaises(build.KitError):
                build.dex_names(names)

    def test_build_id_header_and_elf(self):
        identifier = bytes(range(0x50, 0x64))
        path = self.root / "lib.so"
        path.write_bytes(mini_elf(identifier))
        self.assertEqual(build.elf_build_id(path), identifier)
        header = build.build_id_header(identifier)
        expected = ",".join(f"0x{b:02x}" for b in identifier)
        self.assertEqual(
            header,
            "#pragma once\nnamespace rdr2_game_worker {\n"
            f"inline constexpr unsigned char ExpectedLayerBuildId[]={{{expected}}};\n}}\n",
        )
        path.write_bytes(mini_elf(identifier, machine=62))
        with self.assertRaises(build.KitError):
            build.elf_build_id(path)
        path.write_bytes(mini_elf(identifier[:16]))
        with self.assertRaises(build.KitError):
            build.elf_build_id(path)

    def test_source_build_id(self):
        files = {"native/a.cpp": b"a", "layer/b.cpp": b"b", "loader/src/C.java": b"c", "assets/d/e.txt": b"e",
                 "tools/ignored.py": b"x"}
        for name, data in files.items():
            (self.root / name).parent.mkdir(parents=True, exist_ok=True)
            (self.root / name).write_bytes(data)
        lines = sorted(f"{n}\t{hashlib.sha256(d).hexdigest()}\n" for n, d in files.items() if not n.startswith("tools"))
        self.assertEqual(build.source_build_id(self.root), hashlib.sha256("".join(lines).encode()).hexdigest())

    def test_build_config_and_superclass(self):
        text = build.build_config_source("ab" * 32, "cd" * 32, LAUNCHER)
        self.assertEqual(
            text,
            "package org.rdrvr.bootstrap;\nfinal class BuildConfig {\nstatic final boolean PROBE=false;\n"
            "static final boolean BENCH_CONTROL=true;\nstatic final boolean TRACE=true;\n"
            f'static final String BUILD_ID="{"ab" * 32}";\nstatic final String ENGINE_SHA="{"cd" * 32}";\n'
            f'static final String LAUNCHER="{LAUNCHER}";\nstatic final String MOD_VERSION="0.1.1";\n}}\n',
        )
        source = "import android.app.Application;\npublic final class R extends  Application {}\n"
        self.assertIn("extends a.b.App {}", build.rewrite_superclass(source, "a.b.App"))
        with self.assertRaises(build.KitError):
            build.rewrite_superclass(source + "class S extends Application {}\n", "a.b.App")
        with self.assertRaises(build.KitError):
            build.java_class_name("Outer$Inner")

    def make_set(self, folder: Path, extra=None) -> Path:
        folder.mkdir(parents=True, exist_ok=True)
        write_zip(folder / "base.apk", {"AndroidManifest.xml": play_manifest(), "classes.dex": mini_dex()})
        write_zip(folder / "split_config.arm64_v8a.apk",
                  {"AndroidManifest.xml": split_manifest("config.arm64_v8a"), **(extra or {})})
        write_zip(folder / "split_data0.apk", {"AndroidManifest.xml": split_manifest("data0")})
        return folder

    def test_select_inputs(self):
        folder = self.make_set(self.root / "export")
        (folder / "notes.apk").write_bytes(b"x")
        (folder / "external" / "obb").mkdir(parents=True)
        (folder / "external" / "obb" / "main.obb").write_bytes(b"obb")
        work = self.root / "build"
        work.mkdir()
        with contextlib.redirect_stdout(io.StringIO()):
            kind, files, external = build.select_inputs(folder, work)
        self.assertEqual((kind, [n for _, n in files]), ("folder", ["base.apk", "split_config.arm64_v8a.apk",
                                                                    "split_data0.apk"]))
        self.assertEqual(external, folder / "external")
        kind, files, _ = build.select_inputs(folder / "base.apk", work)
        self.assertEqual((kind, len(files)), ("base", 3))
        single = write_zip(self.root / "game.apk", {"AndroidManifest.xml": play_manifest()})
        kind, files, _ = build.select_inputs(single, work)
        self.assertEqual((kind, files), ("single", [(single, "base.apk")]))
        with self.assertRaises(build.KitError):
            build.select_inputs(folder / "split_data0.apk", work)
        archive = self.root / "game.apks"
        with zipfile.ZipFile(archive, "w") as out:
            for path in sorted(folder.glob("*.apk")):
                out.write(path, f"set/{path.name}")
            out.writestr("toc.pb", b"x")
        kind, files, external = build.select_inputs(archive, work)
        self.assertEqual((kind, external), ("archive", None))
        self.assertEqual(sorted(n for _, n in files), ["base.apk", "split_config.arm64_v8a.apk", "split_data0.apk"])
        self.assertTrue(all(path.parent == work / "input" for path, _ in files))
        bad = self.root / "bad.zip"
        with zipfile.ZipFile(bad, "w") as out:
            out.writestr("../base.apk", b"x")
        with self.assertRaises(build.KitError):
            build.select_inputs(bad, work)

    def test_corrupt_archive_reports_member_and_removes_partial(self):
        for method in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
            with self.subTest(compression=method):
                source = self.root / f"damaged-{method}.apks"
                original = zip_bytes({"base.apk": b"base", "split_data0.apk": b"x" * (build.BLOCK + 128)}, method)
                damaged = bytearray(original)
                with zipfile.ZipFile(io.BytesIO(original)) as archive:
                    pos = archive.start_dir
                while damaged[pos:pos + 4] == b"PK\x01\x02":
                    n, e, c = struct.unpack_from("<HHH", damaged, pos + 28)
                    if damaged[pos + 46:pos + 46 + n] == b"split_data0.apk":
                        damaged[pos + 16] ^= 1
                        break
                    pos += 46 + n + e + c
                else:
                    self.fail("split entry not found")
                source.write_bytes(damaged)
                work = self.root / f"work-{method}"
                work.mkdir()
                with self.assertRaises(build.KitError) as caught:
                    build.select_inputs(source, work)
                self.assertIn("damaged or incomplete", caught.exception.message)
                self.assertIn("split_data0.apk", caught.exception.message)
                self.assertIn("EXPORT_RDR_APKS.bat", caught.exception.message)
                self.assertIn("Bad CRC-32", caught.exception.details)
                self.assertFalse((work / "input/split_data0.apk").exists())
                self.assertEqual(list((work / "input").glob("*.partial")), [])
                self.assertEqual(source.read_bytes(), damaged)
                source.write_bytes(original)
                _, files, _ = build.select_inputs(source, work)
                self.assertEqual([name for _, name in files], ["base.apk", "split_data0.apk"])

    def test_third_party_markers(self):
        folder = self.make_set(self.root / "modified", {"lib/arm64-v8a/liba.so": b"x"})
        apks = [build.open_apk(path, path.name) for path in sorted(folder.glob("*.apk"))]
        with self.assertRaises(build.KitError) as caught:
            build.check_third_party_markers(apks)
        self.assertEqual(caught.exception.message, build.THIRD_PARTY_MESSAGE)
        folder = self.make_set(self.root / "clean")
        base = folder / "base.apk"
        base.unlink()
        write_zip(base, {"AndroidManifest.xml": play_manifest(),
                         "classes.dex": mini_dex(owner="Lcom/an1/unpackdata/MainActivity;")})
        apks = [build.open_apk(path, path.name) for path in sorted(folder.glob("*.apk"))]
        with self.assertRaises(build.KitError) as caught:
            build.check_third_party_markers(apks)
        self.assertEqual(caught.exception.message, build.THIRD_PARTY_MESSAGE)

    def test_list_external(self):
        root = self.root / "external"
        (root / "obb" / PACKAGE).mkdir(parents=True)
        (root / "obb" / PACKAGE / "main.obb").write_bytes(b"obb")
        (root / "other").mkdir()
        (root / "other" / "x").write_bytes(b"x")
        result = build.list_external(root)
        self.assertEqual(result["root"], str(root.resolve()))
        self.assertEqual(result["obb"], [{"path": f"obb/{PACKAGE}/main.obb", "size": 3,
                                          "sha256": hashlib.sha256(b"obb").hexdigest()}])
        self.assertEqual(result["assetpacks"], [])
        self.assertEqual(build.list_external(None), {"root": None, "obb": [], "assetpacks": []})

    def test_cli_has_no_self_test_options(self):
        parser_options = build.parse_args([
            "--game-package", str(self.root), "--build-dir", str(self.root / "b"), "--sdk", str(self.root),
            "--java-home", str(self.root),
        ])
        self.assertEqual(parser_options.trade, 0)
        self.assertFalse(parser_options.validate_only)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            build.parse_args(["--game-package", "x", "--build-dir", "y", "--sdk", "z", "--java-home", "w",
                              "--trade", "2"])


# Toolchain tests.

BASE_XML = """<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="com.rockstargames.rdr"
    android:versionCode="63226194" android:versionName="1.58.63226194">
    <uses-sdk android:minSdkVersion="29" android:targetSdkVersion="35"/>
    <uses-permission android:name="android.permission.INTERNET"/>
    <application android:label="RDR" android:hasCode="true" android:extractNativeLibs="false"{application}>
        <activity android:name="com.rockstargames.rdr.LauncherActivity" android:exported="true"
            android:screenOrientation="sensorLandscape" android:configChanges="orientation|screenSize">
{launcher_filter}
        </activity>
{alias}
        <activity android:name="com.rockstargames.rdr.NativeGameActivity" android:exported="false"
            android:screenOrientation="sensorLandscape">
            <meta-data android:name="android.app.lib_name" android:value="rdr"/>
        </activity>
    </application>
</manifest>
"""
LAUNCHER_FILTER = """            <intent-filter>
                <action android:name="android.intent.action.MAIN"/>
                <category android:name="android.intent.category.LAUNCHER"/>
            </intent-filter>"""
ALIAS_XML = """        <activity-alias android:name=".Main" android:exported="true"
            android:targetActivity="com.rockstargames.rdr.LauncherActivity">
""" + LAUNCHER_FILTER + """
        </activity-alias>"""
SPLIT_XML = """<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="com.rockstargames.rdr"
    split="{split}" android:versionCode="63226194" android:isFeatureSplit="false">
    <application android:hasCode="false"/>
</manifest>
"""
ACCESS_XML = """<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="org.rdrvr.access">
    <uses-permission android:name="org.khronos.openxr.permission.OPENXR_SYSTEM" />
    <queries>
        <provider android:authorities="org.khronos.openxr.runtime_broker;org.khronos.openxr.system_runtime_broker" />
        <intent><action android:name="org.khronos.openxr.OpenXRRuntimeService" /></intent>
    </queries>
    <application>
        <meta-data android:name="com.oculus.trade_cpu_for_gpu_amount" android:value="-1" />
    </application>
</manifest>
"""
GAME_JAVA = {
    "com/rockstargames/rdr/NativeGameActivity.java": """package com.rockstargames.rdr;

import android.app.GameManager;
import android.content.Context;

public class NativeGameActivity extends android.app.NativeActivity {
    static Context s_context;

    public static Context getContext() {
        return s_context;
    }

    private static void activateGameMode() {
        try {
            ((GameManager) getContext().getSystemService(GameManager.class)).getGameMode();
            return;
        } catch (NoClassDefFoundError e) {
            System.err.println("game mode unavailable");
        }
    }
}
""",
    "com/rockstargames/rdr/LauncherActivity.java": """package com.rockstargames.rdr;

public class LauncherActivity extends android.app.Activity {
}
""",
    "com/rockstargames/rdr/GameApp.java": """package com.rockstargames.rdr;

public class GameApp extends android.app.Application {
    public GameApp() {
    }
}
""",
}
KIT_FILES = {
    "layer/CMakeLists.txt": """cmake_minimum_required(VERSION 3.22)
project(selftest_layer CXX)
foreach(name VULKAN_HEADERS_DIR RDRVR_GENERATED_DIR)
  if(NOT DEFINED ${name})
    message(FATAL_ERROR "${name} is required")
  endif()
endforeach()
add_library(VkLayer_RDR2_raw_worker28 SHARED layer.cpp)
target_include_directories(VkLayer_RDR2_raw_worker28 PRIVATE "${VULKAN_HEADERS_DIR}/include")
target_link_options(VkLayer_RDR2_raw_worker28 PRIVATE -Wl,--build-id=sha1)
""",
    "layer/layer.cpp": """#include <vulkan/vulkan_core.h>
extern "C" int selftest_layer() { return SELFTEST_VULKAN; }
""",
    "native/CMakeLists.txt": """cmake_minimum_required(VERSION 3.22)
project(selftest_native CXX)
foreach(name RDRVR_BUILD_ID OPENXR_LOADER_SO RDRVR_GENERATED_DIR)
  if(NOT DEFINED ${name})
    message(FATAL_ERROR "${name} is required")
  endif()
endforeach()
foreach(name WorkerLayerBuildId.h gamecode.stamp hands.stamp)
  if(NOT EXISTS "${RDRVR_GENERATED_DIR}/${name}")
    message(FATAL_ERROR "${name} was not generated")
  endif()
endforeach()
add_library(rdrvr SHARED main.cpp)
target_include_directories(rdrvr PRIVATE "${RDRVR_GENERATED_DIR}")
target_compile_definitions(rdrvr PRIVATE "RDRVR_BUILD_ID=\\"${RDRVR_BUILD_ID}\\"")
target_link_options(rdrvr PRIVATE -Wl,--build-id=sha1)
""",
    "native/main.cpp": """#include "WorkerLayerBuildId.h"
extern "C" const char* selftest_build_id() { return RDRVR_BUILD_ID; }
extern "C" unsigned selftest_layer_id() { return rdr2_game_worker::ExpectedLayerBuildId[0]; }
""",
    "native/gamecode/ranges.json": "{}\n",
    "loader/src/org/rdrvr/bootstrap/RdrVrApplication.java": """package org.rdrvr.bootstrap;

import android.app.Application;

public final class RdrVrApplication extends Application {
    static String launcher() {
        return BuildConfig.LAUNCHER;
    }
}
""",
    "loader/src/org/rdrvr/bootstrap/PreflightActivity.java": """package org.rdrvr.bootstrap;

public final class PreflightActivity extends android.app.Activity {
}
""",
    "loader/openxr-access/AndroidManifest.xml": ACCESS_XML,
    "assets/licenses/OpenXR-LICENSE.txt": "OpenXR license text\n",
    "assets/licenses/MiamiVR-Quest-LICENSE.txt": "MiamiVR license text\n",
    "assets/licenses/UltimateXR-LICENSE.txt": "UltimateXR license text\n",
    "assets/licenses/NVIDIA-FXAA-3.11.txt": "FXAA notice text\n",
    "assets/vrhands/hand.bin": "hand\n",
    "tools/gamecode.py": """import argparse, json, pathlib, sys
p = argparse.ArgumentParser()
p.add_argument('--engine', required=True)
p.add_argument('--ranges', required=True)
p.add_argument('--out', required=True)
a = p.parse_args()
assert pathlib.Path(a.engine).is_file() and pathlib.Path(a.ranges).is_file()
pathlib.Path(a.out, 'gamecode.stamp').write_text(json.dumps(sys.argv[1:]))
""",
    "tools/hands_assets.py": """import argparse, json, pathlib, sys
p = argparse.ArgumentParser()
p.add_argument('--assets', required=True)
p.add_argument('--out', required=True)
a = p.parse_args()
assert pathlib.Path(a.assets).is_dir()
pathlib.Path(a.out, 'hands.stamp').write_text(json.dumps(sys.argv[1:]))
""",
}


def toolchain_ready() -> bool:
    return SETTINGS["sdk"] is not None and SETTINGS["java_home"] is not None


class ToolchainBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sdk = SETTINGS["sdk"]
        cls.jdk = SETTINGS["java_home"]
        cls.tools = build.resolve_toolchain(cls.sdk, cls.jdk, native=False)
        base = SETTINGS["work"]
        if base is not None:
            base.mkdir(parents=True, exist_ok=True)
            cls.temp = None
            cls.root = Path(tempfile.mkdtemp(prefix="apk-tools-", dir=base)).resolve()
        else:
            cls.temp = tempfile.TemporaryDirectory()
            cls.root = Path(cls.temp.name).resolve()

    @classmethod
    def tearDownClass(cls):
        if cls.temp is not None:
            cls.temp.cleanup()

    def run_tool(self, *command, cwd=None) -> str:
        result = subprocess.run([str(c) for c in command], cwd=cwd, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        text = result.stdout.decode("utf-8", errors="replace")
        self.assertEqual(result.returncode, 0, f"{command[0]} failed:\n{text}")
        return text

    def link_manifest(self, xml: str, name: str) -> Path:
        source = self.root / f"{name}.xml"
        source.write_text(xml, encoding="utf-8")
        output = self.root / f"{name}.apk"
        self.run_tool(self.tools.aapt2, "link", "--manifest", source, "-I", self.tools.android_jar, "-o", output)
        return output

    def xmltree(self, apk: Path) -> str:
        return self.run_tool(self.tools.aapt2, "dump", "xmltree", "--file", "AndroidManifest.xml", apk)

    def compile_java(self, sources: dict[str, str], name: str) -> bytes:
        src = self.root / name / "src"
        classes = self.root / name / "classes"
        dex = self.root / name / "dex"
        for path, text in sources.items():
            (src / path).parent.mkdir(parents=True, exist_ok=True)
            (src / path).write_text(text, encoding="utf-8")
        classes.mkdir(parents=True)
        dex.mkdir()
        self.run_tool(self.jdk / "bin" / f"javac{EXE}", "-encoding", "UTF-8", "-source", "8", "-target", "8",
                      "-Xlint:-options", "-bootclasspath", self.tools.android_jar, "-d", classes,
                      *sorted(src.rglob("*.java")))
        self.run_tool(self.tools.java, "-cp", self.tools.d8, "com.android.tools.r8.D8", "--release",
                      "--min-api", "29", "--lib", self.tools.android_jar, "--output", dex,
                      *sorted(classes.rglob("*.class")))
        return (dex / "classes.dex").read_bytes()

    def base_xml(self, application=None, alias=False) -> str:
        return BASE_XML.format(
            application=f' android:name="{application}"' if application else "",
            launcher_filter="" if alias else LAUNCHER_FILTER,
            alias=ALIAS_XML if alias else "",
        )


@unittest.skipUnless(toolchain_ready(), "pass --sdk and --java-home to run toolchain tests")
class ToolchainTests(ToolchainBase):
    def test_manifest_against_aapt2(self):
        fragment_apk = self.link_manifest(ACCESS_XML, "access")
        with zipfile.ZipFile(fragment_apk) as archive:
            fragment = archive.read("AndroidManifest.xml")
        variants = {
            "play": self.base_xml(),
            "alias-application": self.base_xml(application="com.rockstargames.rdr.GameApp", alias=True),
        }
        for label, xml in variants.items():
            for trade in (-1, 0, 1):
                with self.subTest(label=label, trade=trade):
                    original_apk = self.link_manifest(xml, f"base-{label}")
                    with zipfile.ZipFile(original_apk) as archive:
                        original = archive.read("AndroidManifest.xml")
                    result, info = xrmanifest.transform(original, fragment, trade)
                    self.assertEqual(info.launcher, LAUNCHER)
                    result_apk = write_zip(self.root / f"result-{label}-{trade}.apk", {"AndroidManifest.xml": result})
                    original_tree = self.xmltree(original_apk)
                    result_tree = self.xmltree(result_apk)
                    xrmanifest.verify_tree(original_tree, result_tree, trade)
                    with self.assertRaises(xrmanifest.ManifestError):
                        xrmanifest.verify_tree(original_tree, result_tree, 2)
                    lines = result_tree.splitlines()
                    changed = next(i for i, line in enumerate(lines) if line.endswith("(0x0101001e)=6"))
                    lines[changed] = lines[changed][:-1] + "7"
                    with self.assertRaises(xrmanifest.ManifestError):
                        xrmanifest.verify_tree(original_tree, "\n".join(lines), trade)
                    with self.assertRaises(xrmanifest.ManifestError):
                        xrmanifest.verify_tree(original_tree, original_tree, trade)

    def test_d8_output_patch(self):
        data = self.compile_java({k: v for k, v in GAME_JAVA.items() if "Native" in k}, "dexpatch")
        result, report = dexpatch.patch_game_manager(data)
        self.assertTrue(dexpatch.checksums_valid(result))
        start, end = report["changedRange"]
        self.assertEqual(result[end:], data[end:])
        self.assertEqual(struct.unpack_from("<H", result, start + 20)[0], 0x0038)


@unittest.skipUnless(toolchain_ready(), "pass --sdk and --java-home to run toolchain tests")
class PipelineTests(ToolchainBase):
    """Run build.run() end to end with SelfTest hooks."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.native_tools = None
        try:
            cls.native_tools = build.resolve_toolchain(cls.sdk, cls.jdk, native=True)
        except build.KitError:
            pass

    def setUp(self):
        if self.native_tools is None:
            self.skipTest("the SDK has no NDK/CMake for the pipeline test")

    # Test inputs.

    def input_key(self) -> tuple[Path, str]:
        key = self.root / "input-key.p12"
        if not key.exists():
            self.run_tool(self.native_tools.keytool, "-genkeypair", "-noprompt", "-keystore", key, "-storetype",
                          "PKCS12", "-alias", "input", "-keyalg", "RSA", "-keysize", "2048", "-validity", "365",
                          "-storepass", "inputpass", "-keypass", "inputpass", "-dname", "CN=Self Test Input")
        return key, "inputpass"

    def sign_input(self, path: Path) -> str:
        key, password = self.input_key()
        signed = path.with_suffix(".signed")
        self.run_tool(self.tools.java, "-jar", self.tools.apksigner, "sign", "--ks", key, "--ks-pass",
                      f"pass:{password}", "--min-sdk-version", "29", "--v1-signing-enabled", "false",
                      "--out", signed, path)
        os.replace(signed, path)
        return hashlib.sha256(apkzip.signer_certificate(path)).hexdigest()

    def engine_stub(self) -> bytes:
        return b"".join(hashlib.sha256(i.to_bytes(4, "little")).digest() for i in range(8192))

    def game_dex(self, name: str, native_game_mode=False) -> bytes:
        sources = GAME_JAVA
        if native_game_mode:
            sources = {**GAME_JAVA, "com/rockstargames/rdr/NativeGameActivity.java": """
package com.rockstargames.rdr;
public class NativeGameActivity extends android.app.NativeActivity {
    private static native void activateGameMode();
    public static native android.content.Context getContext();
}
"""}
        return self.compile_java(sources, name)

    def make_package(self, name: str, *, application=None, alias=False, single=False,
                     native_game_mode=False) -> tuple[Path, str]:
        folder = self.root / name
        folder.mkdir()
        dex = self.game_dex(f"{name}-java", native_game_mode)
        base = self.link_manifest(self.base_xml(application, alias), f"{name}-base")
        data_entries = {path: f"data {path}".encode() * 64 for path in build.DATA_FILES}
        library_entries = {build.ENGINE_ENTRY: self.engine_stub(), "lib/arm64-v8a/libc++_shared.so": b"\x7fELF" * 64}
        with zipfile.ZipFile(base, "a") as archive:
            archive.writestr("classes.dex", dex, zipfile.ZIP_DEFLATED)
            if single:
                for path, data in {**library_entries, **data_entries}.items():
                    entry = zipfile.ZipInfo(path)
                    if path == build.ENGINE_ENTRY:
                        entry.extra = struct.pack("<HHH", 0xD935, 2, 4096)
                    archive.writestr(entry, data, zipfile.ZIP_STORED)
        target = folder / ("game.apk" if single else "base.apk")
        shutil.move(str(base), str(target))
        signer = self.sign_input(target)
        if single:
            return target, signer
        for split, entries in (("config.arm64_v8a", library_entries), ("data0", data_entries)):
            apk = self.link_manifest(SPLIT_XML.format(split=split), f"{name}-{split}")
            with zipfile.ZipFile(apk, "a") as archive:
                for path, data in entries.items():
                    entry = zipfile.ZipInfo(path)
                    if path == build.ENGINE_ENTRY:
                        entry.extra = struct.pack("<HHH", 0xD935, 2, 4096)
                    archive.writestr(entry, data, zipfile.ZIP_STORED)
            target = folder / f"split_{split}.apk"
            shutil.move(str(apk), str(target))
            self.sign_input(target)
        obb = folder / "external" / "obb" / PACKAGE
        obb.mkdir(parents=True)
        (obb / f"main.63226194.{PACKAGE}.obb").write_bytes(b"obb data")
        return folder, signer

    def make_kit(self) -> Path:
        kit = self.root / "kit"
        if not kit.exists():
            for path, text in KIT_FILES.items():
                (kit / path).parent.mkdir(parents=True, exist_ok=True)
                (kit / path).write_text(text, encoding="utf-8", newline="\n")
        return kit

    def externals(self) -> tuple[Path, Path]:
        loader = self.root / "openxr" / "libopenxr_loader.so"
        if not loader.exists():
            loader.parent.mkdir(parents=True)
            loader.write_bytes(mini_elf(bytes(20)) + b"openxr loader stub")
        headers = self.root / "vulkan-headers"
        core = headers / "include" / "vulkan" / "vulkan_core.h"
        if not core.exists():
            core.parent.mkdir(parents=True)
            core.write_text("#pragma once\n#define SELFTEST_VULKAN 357\n", encoding="utf-8")
        return loader, headers

    def self_test(self, *, signer: str | None = None, engine_sha256: str | None = None) -> build.SelfTest:
        engine = self.engine_stub()
        loader, _ = self.externals()
        return build.SelfTest(
            engine_sha256=engine_sha256 or hashlib.sha256(engine).hexdigest(),
            engine_size=len(engine),
            openxr_loader_sha256=hashlib.sha256(loader.read_bytes()).hexdigest(),
            kit_root=self.make_kit(),
            third_party_signer_sha256=signer or build.THIRD_PARTY_SIGNER_SHA256,
        )

    def options(self, package: Path, name: str, *, trade=0, validate_only=False) -> build.Options:
        loader, headers = self.externals()
        return build.Options(
            game_package=package.resolve(),
            build_dir=(self.root / f"build-{name}").resolve(),
            sdk=self.sdk,
            java_home=self.jdk,
            openxr_loader=loader.resolve(),
            vulkan_headers=headers.resolve(),
            keystore=(self.root / "keys" / "rdrvr.keystore").resolve(),
            trade=trade,
            validate_only=validate_only,
            jobs=2,
        )

    def run_build(self, options: build.Options, self_test: build.SelfTest) -> str:
        output = io.StringIO()
        try:
            with contextlib.redirect_stdout(output):
                self.assertEqual(build.run(options, self_test), 0)
        except build.KitError as error:
            self.fail(f"{error.message}\n{error.details}\n{output.getvalue()}")
        return output.getvalue()

    def expect_failure(self, options: build.Options, self_test: build.SelfTest) -> build.KitError:
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(build.KitError) as caught:
            build.run(options, self_test)
        return caught.exception

    def check_outputs(self, options: build.Options, manifest: dict, signer: str, launcher: str,
                      native_game_mode=False) -> bytes:
        out = options.build_dir / "out"
        self.assertEqual(manifest["formatVersion"], 1)
        self.assertEqual(manifest["modVersion"], "0.1.1")
        self.assertEqual(manifest["buildId"], build.source_build_id(self.make_kit()))
        self.assertEqual((manifest["package"], manifest["versionCode"], manifest["versionName"]),
                         (PACKAGE, "63226194", "1.58.63226194"))
        self.assertEqual(manifest["engineSha256"], hashlib.sha256(self.engine_stub()).hexdigest())
        self.assertEqual(manifest["trade"], options.trade)
        self.assertEqual(manifest["launcher"], launcher)
        self.assertEqual(manifest["sourceSignerSha256"], signer)
        certificate = apkzip.signer_certificate(out / "base.apk")
        self.assertEqual(manifest["outputSignerSha256"], hashlib.sha256(certificate).hexdigest())
        self.assertEqual(manifest["outputSignerHashCode"], apkzip.java_hash_code(certificate))
        for record in manifest["outputs"]:
            path = out / record["file"]
            self.assertEqual((record["size"], record["sha256"]), (path.stat().st_size, build.sha256_file(path)))
            self.assertEqual(apkzip.signer_certificate(path), certificate)
        self.assertEqual(sorted(p.name for p in out.iterdir()), sorted(r["file"] for r in manifest["outputs"]))
        self.assertFalse((options.build_dir / "engine" / "librdr.so").exists())
        self.assertFalse((options.build_dir / "apk").exists())
        generated = options.build_dir / "generated"
        layer_id = build.elf_build_id(options.build_dir / "layer" / build.LAYER_LIBRARY)
        self.assertEqual((generated / "WorkerLayerBuildId.h").read_text(), build.build_id_header(layer_id))
        stamp = json.loads((generated / "gamecode.stamp").read_text())
        self.assertEqual(stamp, ["--engine", str(options.build_dir / "engine" / "librdr.so"),
                                 "--ranges", str(self.make_kit() / "native" / "gamecode" / "ranges.json"),
                                 "--out", str(generated)])
        with zipfile.ZipFile(out / "base.apk") as archive:
            names = set(archive.namelist())
            info = json.loads(archive.read(build.BUILD_INFO_ENTRY))
            self.assertEqual(info, {"modVersion": "0.1.1", "buildId": manifest["buildId"], "package": PACKAGE,
                                    "versionCode": "63226194", "engineSha256": manifest["engineSha256"]})
            for name in build.LICENSE_FILES:
                self.assertIn(f"assets/licenses/{name}", names)
            if native_game_mode:
                with zipfile.ZipFile(options.game_package / "base.apk") as source:
                    self.assertEqual(archive.read("classes.dex"), source.read("classes.dex"))
                self.assertEqual(manifest["gameDexCompatibility"]["status"], "native-method-unchanged")
            else:
                with self.assertRaises(dexpatch.DexError):
                    dexpatch.patch_game_manager(archive.read("classes.dex"))
                self.assertEqual(manifest["gameDexCompatibility"]["status"], "null-guard-applied")
            self.assertIn("classes2.dex", names)
            self.assertNotIn("classes3.dex", names)
            loader_dex = archive.read("classes2.dex")
            self.assertTrue(all(not n.startswith("META-INF/") or n.endswith(".version") for n in names))
        config = (options.build_dir / "java-gen" / "org" / "rdrvr" / "bootstrap" / "BuildConfig.java").read_text()
        self.assertEqual(config, build.build_config_source(manifest["buildId"], manifest["engineSha256"], launcher))
        return loader_dex

    def test_native_game_mode_build(self):
        package, signer = self.make_package("native", native_game_mode=True)
        options = self.options(package, "native")
        log = self.run_build(options, self.self_test())
        self.assertIn("Game DEX compatibility: native-method-unchanged", log)
        manifest = json.loads((options.build_dir / "build-manifest.json").read_text())
        self.check_outputs(options, manifest, signer, LAUNCHER, native_game_mode=True)

    def test_reuse_unchanged_data_split(self):
        package, signer = self.make_package("reuse-input")
        original = self.options(package, "reuse-original")
        self.run_build(original, self.self_test())
        cached = original.build_dir / "out" / "split_data0.apk"
        cached_hash = build.sha256_file(cached)
        # A new mod version must rebuild base/engine APKs but may keep identical signed data.
        source = self.make_kit() / "native" / "main.cpp"
        source.write_text(source.read_text() + "\n// next mod version\n")
        updated = self.options(package, "reuse-updated")
        updated.reuse_build = original.build_dir
        log = self.run_build(updated, self.self_test())
        self.assertIn("reusing verified split_data0.apk", log)
        self.assertNotIn("reusing verified base.apk", log)
        self.assertNotIn("reusing verified split_config.arm64_v8a.apk", log)
        self.assertEqual(build.sha256_file(updated.build_dir / "out" / cached.name), cached_hash)
        self.assertEqual(build.sha256_file(cached), cached_hash)
        manifest = json.loads((updated.build_dir / "build-manifest.json").read_text())
        self.check_outputs(updated, manifest, signer, LAUNCHER)
        # Corrupt cache falls back to a fresh signed APK, without damaging the input.
        cached.write_bytes(b"broken cache")
        repaired = self.options(package, "reuse-repaired")
        repaired.reuse_build = original.build_dir
        log = self.run_build(repaired, self.self_test())
        self.assertIn("cached split_data0.apk cannot be reused", log)
        self.check_outputs(repaired, json.loads((repaired.build_dir / "build-manifest.json").read_text()), signer, LAUNCHER)
        # Never let cleanup of a new run erase its cache.
        repaired.reuse_build = repaired.build_dir
        error = self.expect_failure(repaired, self.self_test())
        self.assertIn("must be separate", error.message)
        self.assertTrue((repaired.build_dir / "build-manifest.json").is_file())

    def test_split_folder_build(self):
        package, signer = self.make_package("play")
        options = self.options(package, "play", trade=0)
        log = self.run_build(options, self.self_test())
        self.assertIn("==> checking the game package", log)
        manifest = json.loads((options.build_dir / "build-manifest.json").read_text())
        loader_dex = self.check_outputs(options, manifest, signer, LAUNCHER)
        self.assertEqual([r["file"] for r in manifest["outputs"]],
                         ["base.apk", "split_config.arm64_v8a.apk", "split_data0.apk"])
        self.assertEqual([r["split"] for r in manifest["outputs"]], [None, "config.arm64_v8a", "data0"])
        self.assertEqual(manifest["external"]["root"], str((package / "external").resolve()))
        self.assertEqual([e["path"] for e in manifest["external"]["obb"]],
                         [f"obb/{PACKAGE}/main.63226194.{PACKAGE}.obb"])
        dex = dexpatch.Dex(loader_dex)
        self.assertEqual(dex.class_info("Lorg/rdrvr/bootstrap/RdrVrApplication;")[1], "Landroid/app/Application;")
        out = options.build_dir / "out"
        with zipfile.ZipFile(out / "split_config.arm64_v8a.apk") as archive:
            for name, source in ((build.MOD_LIBRARY, options.build_dir / "native" / build.MOD_LIBRARY),
                                 (build.LAYER_LIBRARY, options.build_dir / "layer" / build.LAYER_LIBRARY),
                                 (build.OPENXR_LIBRARY, options.openxr_loader)):
                self.assertEqual(archive.read(build.LIBRARY_DIR + name), source.read_bytes())
            self.assertEqual(archive.read(build.ENGINE_ENTRY), self.engine_stub())
        PipelineTests.first_signer = manifest["outputSignerSha256"]

        repeat = self.run_build(options, self.self_test())
        self.assertIn("==> writing build-manifest.json", repeat)
        again = json.loads((options.build_dir / "build-manifest.json").read_text())
        self.assertEqual(again["outputs"], manifest["outputs"])

    def test_zz_archive_with_application_class(self):
        package, signer = self.make_package("app", application="com.rockstargames.rdr.GameApp", alias=True)
        archive = self.root / "app.apks"
        with zipfile.ZipFile(archive, "w") as out:
            for path in sorted(package.glob("*.apk")):
                out.write(path, path.name)
        options = self.options(archive, "app", trade=-1)
        self.run_build(options, self.self_test())
        manifest = json.loads((options.build_dir / "build-manifest.json").read_text())
        loader_dex = self.check_outputs(options, manifest, signer, LAUNCHER)
        self.assertIsNone(manifest["external"]["root"])
        dex = dexpatch.Dex(loader_dex)
        self.assertEqual(dex.class_info("Lorg/rdrvr/bootstrap/RdrVrApplication;")[1],
                         "Lcom/rockstargames/rdr/GameApp;")
        self.assertIsNone(dex.class_info("Lcom/rockstargames/rdr/GameApp;"))
        first = getattr(PipelineTests, "first_signer", None)
        if first is not None:
            self.assertEqual(manifest["outputSignerSha256"], first)
        rebuilt = self.root / "vr-output"
        shutil.copytree(options.build_dir / "out", rebuilt)
        error = self.expect_failure(self.options(rebuilt, "vr-output", validate_only=True), self.self_test())
        self.assertIn("already contains the VR loader", error.message)

    def test_validate_only_and_refusals(self):
        package, signer = self.make_package("single", single=True)
        options = self.options(package, "single", validate_only=True)
        self.run_build(options, self.self_test())
        report = json.loads((options.build_dir / "validation.json").read_text())
        self.assertEqual((report["input"], report["engineApk"], report["sourceSignerSha256"], report["loaderDex"]),
                         ("single", "base.apk", signer, "classes2.dex"))
        self.assertFalse((options.build_dir / "engine").exists())
        error = self.expect_failure(options, self.self_test(signer=signer))
        self.assertEqual(error.message, build.THIRD_PARTY_MESSAGE)
        error = self.expect_failure(options, self.self_test(engine_sha256="0" * 64))
        self.assertIn("unsupported game engine", error.message)
        nested = self.options(package, "nested")
        inside = nested.build_dir / "input" / "game.apk"
        inside.parent.mkdir(parents=True)
        shutil.copyfile(package, inside)
        nested.game_package = inside
        error = self.expect_failure(nested, self.self_test())
        self.assertIn("must not be inside", error.message)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", help="Android SDK root with build-tools 35.0.0, android-35, NDK and CMake")
    parser.add_argument("--java-home", help="JDK 21 root")
    parser.add_argument("--work", help="keep toolchain test files in this directory")
    args, rest = parser.parse_known_args()
    if bool(args.sdk) != bool(args.java_home):
        parser.error("--sdk and --java-home must be given together")
    if args.sdk:
        SETTINGS["sdk"] = Path(args.sdk).expanduser().resolve()
        SETTINGS["java_home"] = Path(args.java_home).expanduser().resolve()
    if args.work:
        SETTINGS["work"] = Path(args.work).expanduser().resolve()
    for case in (ToolchainTests, PipelineTests):
        case.__unittest_skip__ = not toolchain_ready()
    program = unittest.main(argv=[sys.argv[0], *rest], exit=False, verbosity=2)
    return 0 if program.result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
