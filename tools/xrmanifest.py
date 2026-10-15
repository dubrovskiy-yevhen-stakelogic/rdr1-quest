"""VR manifest transform for the game's base APK and its aapt2 cross-check.

transform() edits the binary manifest in place. verify_tree() parses the
`aapt2 dump xmltree` output of the original and of the result, undoes every
expected edit on that independent view and requires both trees to be identical.
"""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass, field

from axml import (
    BOOL_TRUE,
    CHUNK_END_ELEMENT,
    CHUNK_START_ELEMENT,
    NO_INDEX,
    TYPE_BOOLEAN,
    TYPE_INT_DEC,
    TYPE_INT_HEX,
    TYPE_STRING,
    ANDROID_NS,
    AxmlError,
    Document,
    Element,
    qualify,
)

APPLICATION_CLASS = "org.rdrvr.bootstrap.RdrVrApplication"
PREFLIGHT_ACTIVITY = "org.rdrvr.bootstrap.PreflightActivity"
NATIVE_ACTIVITY = "com.rockstargames.rdr.NativeGameActivity"
LIB_NAME_META = "android.app.lib_name"
ORIGINAL_LIBRARY = "rdr"
MOD_LIBRARY = "rdrvr"

ACTION_MAIN = "android.intent.action.MAIN"
CATEGORY_LAUNCHER = "android.intent.category.LAUNCHER"
CATEGORY_IMMERSIVE = "org.khronos.openxr.intent.category.IMMERSIVE_HMD"
CATEGORY_VR = "com.oculus.intent.category.VR"
VR_CATEGORIES = (CATEGORY_IMMERSIVE, CATEGORY_VR)
FOCUS_AWARE_META = "com.oculus.vr.focusaware"
TRADE_META = "com.oculus.trade_cpu_for_gpu_amount"
HEADTRACKING_FEATURE = "android.hardware.vr.headtracking"
HAND_TRACKING_PERMISSION = "com.oculus.permission.HAND_TRACKING"
HAND_TRACKING_FEATURE = "oculus.software.handtracking"
OPENXR_PERMISSION = "org.khronos.openxr.permission.OPENXR_SYSTEM"
OPENXR_SERVICE = "org.khronos.openxr.OpenXRRuntimeService"
OPENXR_AUTHORITIES = "org.khronos.openxr.runtime_broker;org.khronos.openxr.system_runtime_broker"
PREFLIGHT_ORIENTATION = 6

# Values whose presence shows that a manifest was already changed for VR.
MODIFIED_MARKERS = frozenset(
    (
        APPLICATION_CLASS,
        PREFLIGHT_ACTIVITY,
        CATEGORY_IMMERSIVE,
        CATEGORY_VR,
        FOCUS_AWARE_META,
        TRADE_META,
        HEADTRACKING_FEATURE,
        HAND_TRACKING_PERMISSION,
        HAND_TRACKING_FEATURE,
        OPENXR_PERMISSION,
        OPENXR_SERVICE,
    )
)


class ManifestError(ValueError):
    """Manifest that does not have the expected structure."""


@dataclass(frozen=True)
class PackageInfo:
    package: str
    version_code: str
    version_name: str | None
    split: str | None


@dataclass(frozen=True)
class BaseInfo:
    package: PackageInfo
    application_class: str | None
    extract_native_libs: bool | None
    launcher: str
    launcher_component: str
    launcher_tag: str


# Binary manifest helpers.


def _document(data: bytes) -> Document:
    try:
        return Document(data)
    except AxmlError as error:
        raise ManifestError(f"unreadable AndroidManifest.xml: {error}") from error


def _tree(doc: Document) -> Element:
    try:
        return doc.tree()
    except AxmlError as error:
        raise ManifestError(f"unreadable AndroidManifest.xml: {error}") from error


def _plain(element: Element, name: str):
    matches = [a for a in element.attributes if a.namespace is None and a.name == name]
    if len(matches) > 1:
        raise ManifestError(f"duplicate {name} attribute on <{element.tag}>")
    return matches[0].value() if matches else None


def _string(element: Element, name: str) -> str | None:
    try:
        attr = element.attribute(name)
    except AxmlError as error:
        raise ManifestError(str(error)) from error
    if attr is None:
        return None
    if attr.type != TYPE_STRING:
        raise ManifestError(f"android:{name} on <{element.tag}> is not a literal string")
    return attr.string


def read_package_info(data: bytes) -> PackageInfo:
    """Package identity of any APK manifest (base or split)."""
    root = _tree(_document(data))
    package = _plain(root, "package")
    if not isinstance(package, str) or not package:
        raise ManifestError("manifest has no package name")
    version = root.attribute("versionCode")
    if version is None or version.type not in (TYPE_INT_DEC, TYPE_INT_HEX):
        raise ManifestError("manifest has no literal versionCode")
    split = _plain(root, "split")
    if split is not None and not isinstance(split, str):
        raise ManifestError("manifest split name is not a string")
    name = root.attribute("versionName")
    version_name = name.string if name is not None and name.type == TYPE_STRING else None
    return PackageInfo(package, str(version.data), version_name, split or None)


def _application(root: Element) -> Element:
    applications = root.find("application")
    if len(applications) != 1:
        raise ManifestError(f"expected one <application>, found {len(applications)}")
    return applications[0]


def _component(element: Element, package: str) -> str | None:
    name = _string(element, "name")
    return qualify(name, package) if name else None


def _actions(intent_filter: Element) -> list[str | None]:
    return [_string(action, "name") for action in intent_filter.find("action")]


def _categories(intent_filter: Element, name: str) -> list[Element]:
    return [c for c in intent_filter.find("category") if _string(c, "name") == name]


def _single_activity(application: Element, package: str, name: str) -> Element:
    found = [a for a in application.find("activity") if _component(a, package) == name]
    if len(found) != 1:
        raise ManifestError(f"expected one <activity> {name}, found {len(found)}")
    return found[0]


def _main_filters(activity: Element) -> list[Element]:
    return [f for f in activity.find("intent-filter") if ACTION_MAIN in _actions(f)]


def _find_launcher(application: Element, package: str):
    """Return (element, LAUNCHER category element, launcher activity class)."""
    candidates = []
    total = 0
    for element in application.children:
        if element.tag not in ("activity", "activity-alias"):
            continue
        for intent_filter in element.find("intent-filter"):
            categories = _categories(intent_filter, CATEGORY_LAUNCHER)
            total += len(categories)
            if categories and ACTION_MAIN in _actions(intent_filter):
                candidates.append((element, categories))
    if len(candidates) != 1 or total != 1:
        raise ManifestError(
            f"expected exactly one MAIN/LAUNCHER activity, found {len(candidates)} "
            f"with {total} LAUNCHER categories"
        )
    element, categories = candidates[0]
    name = _component(element, package)
    if not name:
        raise ManifestError("the launcher activity has no android:name")
    target = name
    if element.tag == "activity-alias":
        target_name = _string(element, "targetActivity")
        if not target_name:
            raise ManifestError(f"launcher alias {name} has no android:targetActivity")
        target = qualify(target_name, package)
        _single_activity(application, package, target)
    return element, categories[0], target


def inspect_base(data: bytes) -> BaseInfo:
    """Check every precondition of transform() without changing anything."""
    info = read_package_info(data)
    if info.split is not None:
        raise ManifestError("the base manifest declares a split name")
    root = _tree(_document(data))
    application = _application(root)
    for element in root.walk():
        for attr in element.attributes:
            if attr.type == TYPE_STRING and attr.string in MODIFIED_MARKERS:
                raise ManifestError(f"the manifest already contains {attr.string}")
    application_class = _component(application, info.package)
    if application_class and application_class.startswith("org.rdrvr."):
        raise ManifestError("the manifest already uses a VR application class")
    extract = application.attribute("extractNativeLibs")
    if extract is not None and extract.type != TYPE_BOOLEAN:
        raise ManifestError("android:extractNativeLibs is not a literal boolean")
    element, _, launcher = _find_launcher(application, info.package)
    native = _single_activity(application, info.package, NATIVE_ACTIVITY)
    libraries = [m for m in native.find("meta-data") if _string(m, "name") == LIB_NAME_META]
    if len(libraries) != 1:
        raise ManifestError(f"{NATIVE_ACTIVITY} must declare {LIB_NAME_META} once")
    if _string(libraries[0], "value") != ORIGINAL_LIBRARY:
        raise ManifestError(f"{NATIVE_ACTIVITY} does not load lib{ORIGINAL_LIBRARY}.so")
    for name in {launcher, NATIVE_ACTIVITY}:
        activity = _single_activity(application, info.package, name)
        filters = _main_filters(activity)
        if len(filters) > 1:
            raise ManifestError(f"{name} has more than one MAIN intent-filter")
        if not filters and activity.attribute("exported") is None:
            raise ManifestError(f"{name} has no android:exported attribute")
    return BaseInfo(
        package=info,
        application_class=application_class,
        extract_native_libs=None if extract is None else extract.data != 0,
        launcher=launcher,
        launcher_component=_component(element, info.package) or "",
        launcher_tag=element.tag,
    )


def _intent_filter_chunks(doc: Document, categories) -> list[list]:
    start, end = doc.element_chunks("intent-filter")
    nodes = [start, *doc.element_chunks("action", [("name", TYPE_STRING, ACTION_MAIN)])]
    for category in categories:
        nodes += doc.element_chunks("category", [("name", TYPE_STRING, category)])
    return nodes + [end]


def _set_application(doc: Document) -> None:
    application = _application(_tree(doc))
    doc.set_attribute(application, "name", TYPE_STRING, APPLICATION_CLASS)
    application = _application(_tree(doc))
    extract = application.attribute("extractNativeLibs")
    if extract is None or extract.data != BOOL_TRUE:
        doc.set_attribute(application, "extractNativeLibs", TYPE_BOOLEAN, BOOL_TRUE)


def _set_native_library(doc: Document, package: str) -> None:
    native = _single_activity(_application(_tree(doc)), package, NATIVE_ACTIVITY)
    entries = [m for m in native.find("meta-data") if _string(m, "name") == LIB_NAME_META]
    if len(entries) != 1 or _string(entries[0], "value") != ORIGINAL_LIBRARY:
        raise ManifestError(f"unexpected {LIB_NAME_META} on {NATIVE_ACTIVITY}")
    doc.set_attribute(entries[0], "value", TYPE_STRING, MOD_LIBRARY)


def _move_launcher(doc: Document, package: str) -> None:
    element, category, _ = _find_launcher(_application(_tree(doc)), package)
    if category.children:
        raise ManifestError("the LAUNCHER category has child elements")
    # The category lies inside the launcher element, so its end moves back by two chunks.
    launcher_end = element.end - 2
    doc.remove(category)
    start, end = doc.element_chunks(
        "activity",
        [
            ("name", TYPE_STRING, PREFLIGHT_ACTIVITY),
            ("exported", TYPE_BOOLEAN, BOOL_TRUE),
            ("screenOrientation", TYPE_INT_DEC, PREFLIGHT_ORIENTATION),
        ],
    )
    doc.insert(launcher_end + 1, [start, *_intent_filter_chunks(doc, [CATEGORY_LAUNCHER]), end])


def _fragment_nodes(fragment: Document) -> tuple[Element, Element, Element, Element]:
    root = _tree(fragment)
    if [child.tag for child in root.children] != ["uses-permission", "queries", "application"]:
        raise ManifestError("unexpected OpenXR access manifest layout")
    permission, queries, application = root.children
    if application.attributes or [c.tag for c in application.children] != ["meta-data"]:
        raise ManifestError("the access manifest application must hold only the CPU/GPU trade")
    meta = application.children[0]
    if (
        {a.name for a in meta.attributes} != {"name", "value"}
        or _string(meta, "name") != TRADE_META
        or meta.attribute("value") is None
        or meta.attribute("value").type not in (TYPE_INT_DEC, TYPE_INT_HEX)
        or meta.children
    ):
        raise ManifestError("unexpected CPU/GPU trade metadata in the access manifest")
    if {a.name for a in permission.attributes} != {"name"} or _string(permission, "name") != OPENXR_PERMISSION:
        raise ManifestError("unexpected OpenXR permission in the access manifest")
    if [c.tag for c in queries.children] != ["provider", "intent"]:
        raise ManifestError("unexpected OpenXR queries in the access manifest")
    provider, intent = queries.children
    actions = intent.find("action")
    if (
        _string(provider, "authorities") != OPENXR_AUTHORITIES
        or [c.tag for c in intent.children] != ["action"]
        or _string(actions[0], "name") != OPENXR_SERVICE
    ):
        raise ManifestError("unexpected OpenXR runtime query in the access manifest")
    return permission, queries, application, meta


def _copy_chunks(doc: Document, fragment: Document, element: Element) -> list[list]:
    """Copy element's subtree from fragment, remapping string indices into doc."""
    mapping: dict[int, int] = {NO_INDEX: NO_INDEX}

    def remap(index: int) -> int:
        if index not in mapping:
            value = fragment.text(index)
            mapping[index] = doc.string(value, fragment.resource_id(index))
        return mapping[index]

    copied = []
    for kind, chunk in fragment.parts[element.start:element.end + 1]:
        if kind not in (CHUNK_START_ELEMENT, CHUNK_END_ELEMENT):
            raise ManifestError("the access manifest contains unsupported nodes")
        node = bytearray(chunk)
        for offset in (12, 16, 20):
            struct.pack_into("<I", node, offset, remap(struct.unpack_from("<I", node, offset)[0]))
        if kind == CHUNK_START_ELEMENT:
            count = struct.unpack_from("<H", node, 28)[0]
            for j in range(count):
                at = 36 + 20 * j
                for offset in (0, 4, 8):
                    value = struct.unpack_from("<I", node, at + offset)[0]
                    struct.pack_into("<I", node, at + offset, remap(value))
                if node[at + 15] == TYPE_STRING:
                    value = struct.unpack_from("<I", node, at + 16)[0]
                    struct.pack_into("<I", node, at + 16, remap(value))
        copied.append([kind, node])
    return copied


def _merge_access(doc: Document, fragment_data: bytes, trade: int) -> None:
    fragment = _document(fragment_data)
    permission, queries, _, meta = _fragment_nodes(fragment)
    top = _copy_chunks(doc, fragment, permission) + _copy_chunks(doc, fragment, queries)
    inner = _copy_chunks(doc, fragment, meta)
    value = meta.attribute("value")
    struct.pack_into("<IHBBI", inner[0][1], value.offset + 8, NO_INDEX, 8, 0, TYPE_INT_DEC, trade & 0xFFFFFFFF)
    application = _application(_tree(doc))
    doc.insert(application.start + 1, inner)
    doc.insert(application.start, top)


def _add_immersive_chain(doc: Document, package: str, activities: list[str]) -> None:
    application = _application(_tree(doc))
    doc.insert(
        application.start,
        doc.element_chunks(
            "uses-feature",
            [
                ("name", TYPE_STRING, HEADTRACKING_FEATURE),
                ("required", TYPE_BOOLEAN, BOOL_TRUE),
                ("version", TYPE_INT_DEC, 1),
            ],
        ),
    )
    for name in activities:
        activity = _single_activity(_application(_tree(doc)), package, name)
        filters = _main_filters(activity)
        if len(filters) > 1:
            raise ManifestError(f"{name} has more than one MAIN intent-filter")
        focus = doc.element_chunks(
            "meta-data",
            [("name", TYPE_STRING, FOCUS_AWARE_META), ("value", TYPE_BOOLEAN, BOOL_TRUE)],
        )
        if filters:
            categories = []
            for category in VR_CATEGORIES:
                categories += doc.element_chunks("category", [("name", TYPE_STRING, category)])
            doc.insert(filters[0].end, categories)
            activity = _single_activity(_application(_tree(doc)), package, name)
            doc.insert(activity.end, focus)
        else:
            if activity.attribute("exported") is None:
                raise ManifestError(f"{name} has no android:exported attribute")
            doc.insert(activity.end, focus + _intent_filter_chunks(doc, VR_CATEGORIES))


def _add_hand_tracking(doc: Document) -> None:
    nodes = doc.element_chunks("uses-permission", [("name", TYPE_STRING, HAND_TRACKING_PERMISSION)])
    nodes += doc.element_chunks(
        "uses-feature",
        [("name", TYPE_STRING, HAND_TRACKING_FEATURE), ("required", TYPE_BOOLEAN, 0)],
    )
    doc.insert(_application(_tree(doc)).start, nodes)


def transform(data: bytes, fragment: bytes, trade: int) -> tuple[bytes, BaseInfo]:
    """Apply the VR manifest edits to a base APK manifest."""
    if trade not in (-1, 0, 1):
        raise ManifestError("the CPU/GPU trade must be -1, 0 or 1")
    info = inspect_base(data)
    package = info.package.package
    doc = _document(data)
    _set_application(doc)
    _set_native_library(doc, package)
    _move_launcher(doc, package)
    _merge_access(doc, fragment, trade)
    chain = []
    for name in (PREFLIGHT_ACTIVITY, info.launcher, NATIVE_ACTIVITY):
        if name not in chain:
            chain.append(name)
    _add_immersive_chain(doc, package, chain)
    _add_hand_tracking(doc)
    result = doc.to_bytes()
    check = _tree(_document(result))
    if read_package_info(result) != info.package or _component(_application(check), package) != APPLICATION_CLASS:
        raise ManifestError("the transformed manifest lost its identity")
    return result, info


# Independent check on `aapt2 dump xmltree` output.


@dataclass
class TreeNode:
    line: str
    tag: str | None
    attributes: list[str] = field(default_factory=list)
    children: list[TreeNode] = field(default_factory=list)


_ATTRIBUTE = re.compile(r"^(\s*)A: (?:(?P<ns>[^=]*):)?(?P<name>[A-Za-z_][\w.-]*)(?:\(0x[0-9a-f]{8}\))?=(?P<value>.*)$")


def parse_tree(text: str) -> list[TreeNode]:
    roots: list[TreeNode] = []
    stack: list[tuple[int, TreeNode]] = []
    for line in text.splitlines():
        if not line.strip():
            continue
        indent = len(line) - len(line.lstrip(" "))
        content = line.strip()
        while stack and stack[-1][0] >= indent:
            stack.pop()
        if content.startswith("A: "):
            if not stack:
                raise ManifestError("attribute outside an element in aapt2 output")
            stack[-1][1].attributes.append(line)
            continue
        tag = content.split()[1] if content.startswith("E: ") and len(content.split()) > 1 else None
        node = TreeNode(line=line, tag=tag)
        (stack[-1][1].children if stack else roots).append(node)
        stack.append((indent, node))
    return roots


def render_tree(nodes: list[TreeNode]) -> list[str]:
    lines = []
    for node in nodes:
        lines.append(node.line)
        lines.extend(node.attributes)
        lines.extend(render_tree(node.children))
    return lines


def _attribute_parts(line: str) -> tuple[str | None, str, str]:
    match = _ATTRIBUTE.match(line)
    if not match:
        raise ManifestError(f"cannot parse aapt2 attribute: {line.strip()}")
    return match.group("ns"), match.group("name"), match.group("value")


def _value(text: str) -> str:
    match = re.fullmatch(r'"(.*)" \(Raw: "(.*)"\)', text)
    if match and match.group(1) == match.group(2):
        return '"' + match.group(1) + '"'
    return text


def _android_attributes(node: TreeNode) -> dict[str, str]:
    result = {}
    for line in node.attributes:
        namespace, name, value = _attribute_parts(line)
        if namespace != ANDROID_NS:
            continue
        if name in result:
            raise ManifestError(f"duplicate android:{name} in aapt2 output")
        result[name] = _value(value)
    return result


def _quoted(value: str) -> str:
    return '"' + value + '"'


def _matches(node: TreeNode, tag: str, attributes: dict[str, str]) -> bool:
    if node.tag != tag:
        return False
    if any(_attribute_parts(line)[0] != ANDROID_NS for line in node.attributes):
        return False
    return _android_attributes(node) == attributes


def _take(parent: TreeNode, tag: str, attributes: dict[str, str], *, leaf: bool = True) -> TreeNode:
    found = [child for child in parent.children if _matches(child, tag, attributes)]
    if len(found) != 1:
        raise ManifestError(f"expected one added <{tag}> {attributes}, found {len(found)}")
    if leaf and found[0].children:
        raise ManifestError(f"added <{tag}> has unexpected children")
    parent.children.remove(found[0])
    return found[0]


def _single(parent: TreeNode, tag: str) -> TreeNode:
    found = [child for child in parent.children if child.tag == tag]
    if len(found) != 1:
        raise ManifestError(f"expected one <{tag}> in aapt2 output, found {len(found)}")
    return found[0]


def _tree_component(node: TreeNode, package: str) -> str | None:
    name = _android_attributes(node).get("name")
    if not name or not name.startswith('"'):
        return None
    return qualify(name[1:-1], package)


def _tree_names(nodes, tag: str) -> list[str]:
    return [_android_attributes(n).get("name", "") for n in nodes if n.tag == tag]


def _tree_activity(application: TreeNode, package: str, name: str) -> TreeNode:
    found = [c for c in application.children if c.tag == "activity" and _tree_component(c, package) == name]
    if len(found) != 1:
        raise ManifestError(f"expected one activity {name} in aapt2 output, found {len(found)}")
    return found[0]


def _tree_main_filters(activity: TreeNode) -> list[TreeNode]:
    return [
        f
        for f in activity.children
        if f.tag == "intent-filter" and _quoted(ACTION_MAIN) in _tree_names(f.children, "action")
    ]


def _manifest(roots: list[TreeNode]) -> TreeNode:
    found = [n for n in roots if n.tag == "manifest"]
    found += [c for n in roots for c in n.children if c.tag == "manifest"]
    if len(found) != 1:
        raise ManifestError("aapt2 output has no single <manifest> element")
    return found[0]


def _same_element(left: TreeNode, right: TreeNode) -> bool:
    return left.tag == right.tag and left.line == right.line and left.attributes == right.attributes


def _tree_launcher(application: TreeNode, package: str) -> tuple[TreeNode, TreeNode, str]:
    candidates = []
    total = 0
    for element in application.children:
        if element.tag not in ("activity", "activity-alias"):
            continue
        for intent_filter in element.children:
            if intent_filter.tag != "intent-filter":
                continue
            count = _tree_names(intent_filter.children, "category").count(_quoted(CATEGORY_LAUNCHER))
            total += count
            if count and _quoted(ACTION_MAIN) in _tree_names(intent_filter.children, "action"):
                candidates.append((element, intent_filter))
    if len(candidates) != 1 or total != 1:
        raise ManifestError("aapt2 output of the original manifest has no single launcher")
    element, intent_filter = candidates[0]
    name = _tree_component(element, package)
    if element.tag == "activity-alias":
        target = _android_attributes(element).get("targetActivity", "")
        name = qualify(target.strip('"'), package) if target.startswith('"') else None
    if not name:
        raise ManifestError("aapt2 output of the launcher has no class name")
    return element, intent_filter, name


def _shape(node: TreeNode) -> list:
    return [(child.tag, _android_attributes(child), _shape(child)) for child in node.children]


def _check_preflight(application: TreeNode, launcher_node: TreeNode, package: str) -> None:
    found = [
        i
        for i, child in enumerate(application.children)
        if child.tag == "activity" and _tree_component(child, package) == PREFLIGHT_ACTIVITY
    ]
    if len(found) != 1:
        raise ManifestError(f"expected one {PREFLIGHT_ACTIVITY}, found {len(found)}")
    index = found[0]
    if index == 0 or not _same_element(application.children[index - 1], launcher_node):
        raise ManifestError(f"{PREFLIGHT_ACTIVITY} does not follow the original launcher")
    node = application.children[index]
    expected_attributes = {
        "name": _quoted(PREFLIGHT_ACTIVITY),
        "exported": "true",
        "screenOrientation": str(PREFLIGHT_ORIENTATION),
    }
    expected_shape = [
        (
            "intent-filter",
            {},
            [
                ("action", {"name": _quoted(ACTION_MAIN)}, []),
                ("category", {"name": _quoted(CATEGORY_LAUNCHER)}, []),
                ("category", {"name": _quoted(CATEGORY_IMMERSIVE)}, []),
                ("category", {"name": _quoted(CATEGORY_VR)}, []),
            ],
        ),
        ("meta-data", {"name": _quoted(FOCUS_AWARE_META), "value": "true"}, []),
    ]
    if not _matches(node, "activity", expected_attributes) or _shape(node) != expected_shape:
        raise ManifestError(f"unexpected {PREFLIGHT_ACTIVITY} declaration")
    del application.children[index]


def _take_queries(manifest: TreeNode) -> None:
    expected = [
        ("provider", {"authorities": _quoted(OPENXR_AUTHORITIES)}, []),
        ("intent", {}, [("action", {"name": _quoted(OPENXR_SERVICE)}, [])]),
    ]
    found = [c for c in manifest.children if c.tag == "queries" and _shape(c) == expected and not c.attributes]
    if len(found) != 1:
        raise ManifestError(f"expected one added OpenXR <queries>, found {len(found)}")
    manifest.children.remove(found[0])


def _attribute_lines(node: TreeNode, name: str) -> list[str]:
    result = []
    for line in node.attributes:
        namespace, key, _ = _attribute_parts(line)
        if namespace == ANDROID_NS and key == name:
            result.append(line)
    return result


def _restore_attribute(result: TreeNode, original: TreeNode, name: str, expected: str) -> None:
    lines = _attribute_lines(result, name)
    if len(lines) != 1 or _value(_attribute_parts(lines[0])[2]) != expected:
        raise ManifestError(f"android:{name} is not {expected} in the result")
    before = _attribute_lines(original, name)
    index = result.attributes.index(lines[0])
    if before:
        result.attributes[index] = before[0]
    else:
        del result.attributes[index]


def verify_tree(original_text: str, result_text: str, trade: int) -> None:
    """Raise ManifestError unless result differs from original only by the VR edits."""
    original = parse_tree(original_text)
    result = parse_tree(result_text)
    om = _manifest(original)
    rm = _manifest(result)
    packages = [line for line in om.attributes if _attribute_parts(line)[1] == "package"]
    if len(packages) != 1:
        raise ManifestError("aapt2 output has no package attribute")
    package = _value(_attribute_parts(packages[0])[2]).strip('"')

    _take(rm, "uses-permission", {"name": _quoted(OPENXR_PERMISSION)})
    _take_queries(rm)
    _take(rm, "uses-feature", {"name": _quoted(HEADTRACKING_FEATURE), "version": "1", "required": "true"})
    _take(rm, "uses-permission", {"name": _quoted(HAND_TRACKING_PERMISSION)})
    _take(rm, "uses-feature", {"name": _quoted(HAND_TRACKING_FEATURE), "required": "false"})

    oa = _single(om, "application")
    ra = _single(rm, "application")
    _restore_attribute(ra, oa, "name", _quoted(APPLICATION_CLASS))
    _restore_attribute(ra, oa, "extractNativeLibs", "true")
    _take(ra, "meta-data", {"name": _quoted(TRADE_META), "value": str(trade)})

    launcher_node, launcher_filter, launcher = _tree_launcher(oa, package)
    launchers = [
        c for c in launcher_filter.children if _matches(c, "category", {"name": _quoted(CATEGORY_LAUNCHER)})
    ]
    if len(launchers) != 1 or launchers[0].children:
        raise ManifestError("unexpected LAUNCHER category in the original manifest")
    launcher_filter.children.remove(launchers[0])
    _check_preflight(ra, launcher_node, package)

    for name in dict.fromkeys((launcher, NATIVE_ACTIVITY)):
        before = _tree_activity(oa, package, name)
        after = _tree_activity(ra, package, name)
        _take(after, "meta-data", {"name": _quoted(FOCUS_AWARE_META), "value": "true"})
        filters = _tree_main_filters(after)
        if len(filters) != 1:
            raise ManifestError(f"{name} must have one MAIN intent-filter in the result")
        for category in VR_CATEGORIES:
            _take(filters[0], "category", {"name": _quoted(category)})
        if not _tree_main_filters(before):
            if _shape(filters[0]) != [("action", {"name": _quoted(ACTION_MAIN)}, [])] or filters[0].attributes:
                raise ManifestError(f"unexpected created intent-filter on {name}")
            after.children.remove(filters[0])

    native_after = _tree_activity(ra, package, NATIVE_ACTIVITY)
    native_before = _tree_activity(oa, package, NATIVE_ACTIVITY)
    after_meta = [
        c for c in native_after.children
        if c.tag == "meta-data" and _android_attributes(c).get("name") == _quoted(LIB_NAME_META)
    ]
    before_meta = [
        c for c in native_before.children
        if c.tag == "meta-data" and _android_attributes(c).get("name") == _quoted(LIB_NAME_META)
    ]
    if len(after_meta) != 1 or len(before_meta) != 1:
        raise ManifestError(f"{LIB_NAME_META} is missing in aapt2 output")
    if _android_attributes(before_meta[0]).get("value") != _quoted(ORIGINAL_LIBRARY):
        raise ManifestError(f"the original {LIB_NAME_META} is not {ORIGINAL_LIBRARY}")
    _restore_attribute(after_meta[0], before_meta[0], "value", _quoted(MOD_LIBRARY))

    before_lines = render_tree(original)
    after_lines = render_tree(result)
    if before_lines != after_lines:
        for left, right in zip(before_lines + ["<end>"], after_lines + ["<end>"]):
            if left != right:
                raise ManifestError(
                    "the manifest differs beyond the expected VR edits: "
                    f"expected {left.strip()!r}, found {right.strip()!r}"
                )
