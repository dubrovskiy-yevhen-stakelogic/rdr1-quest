"""Pinned Quest startup compatibility for the tested alternate Android launcher."""
import hashlib
import struct
import zlib
import dexpatch
import xrmanifest
from axml import Document, TYPE_BOOLEAN, BOOL_TRUE


def prepare_manifest(data):
    info = xrmanifest.inspect_base(data)
    # The tested Netflix frontend requires services absent on Quest. Keep its
    # Application, providers and resources; enter the native game activity.
    if info.package.package != xrmanifest.NETFLIX_PACKAGE or info.launcher == xrmanifest.NATIVE_ACTIVITY:
        return data
    doc = Document(data)
    _, category, _ = xrmanifest._find_launcher(xrmanifest._application(doc.tree()), info.package.package)
    doc.remove(category)
    native = xrmanifest._single_activity(xrmanifest._application(doc.tree()), info.package.package, xrmanifest.NATIVE_ACTIVITY)
    doc.set_attribute(native, "exported", TYPE_BOOLEAN, BOOL_TRUE)
    native = xrmanifest._single_activity(xrmanifest._application(doc.tree()), info.package.package, xrmanifest.NATIVE_ACTIVITY)
    doc.insert(native.end, xrmanifest._intent_filter_chunks(doc, [xrmanifest.CATEGORY_LAUNCHER]))
    return doc.to_bytes()


def patch_provider(data):
    dex = dexpatch.Dex(data)
    owner = "Lcom/pairip/licensecheck/LicenseContentProvider;"
    if dex.class_def(owner) is None:
        return data
    _, _, code = dex.code(owner, "onCreate", ("Z", ()))
    units = dex.units(code)
    expected = [34, 63, 4206, 175, 2, 268, 8304, 143, 16, 4206, 151, 0, 4114, 15]
    if units != expected or dex.method(151) != (
        "Lcom/pairip/licensecheck/LicenseClient;", "initializeLicenseCheck", ("V", ())
    ):
        raise dexpatch.DexError("this startup provider variant is not supported; report it to the author")
    result = bytearray(data)
    offset = code + 16 + 9 * 2
    result[offset:offset + 6] = bytes(6)
    result[12:32] = hashlib.sha1(result[32:]).digest()
    struct.pack_into("<I", result, 8, zlib.adler32(result[12:]) & 0xffffffff)
    return bytes(result)
