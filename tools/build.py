#!/usr/bin/env python3
"""Build the Red Dead Redemption VR APK set from the original game package.

Validate the game package, build the native libraries and the Java loader, edit the
manifest and the game DEX, then assemble, align, sign and check every APK."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import traceback
import zipfile
import zlib
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

sys.dont_write_bytecode = True
TOOLS = Path(__file__).resolve().parent
# The embeddable Python does not add the script directory to sys.path.
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import apkzip  # noqa: E402
import dexpatch  # noqa: E402
import xrmanifest  # noqa: E402

KIT_ROOT = TOOLS.parent

MOD_VERSION = "0.1.0"
PACKAGE_NAME = "com.rockstargames.rdr"
VERSION_CODE = "63226194"
VERSION_NAME = "1.58.63226194"
ENGINE_ENTRY = "lib/arm64-v8a/librdr.so"
ENGINE_SIZE = 1_323_108_640
ENGINE_SHA256 = "3f80845edec32a663c42a8570e3131446813e695076c4bc96e2142c190bc22f3"
OPENXR_LOADER_SHA256 = "713e3bb8d955254c670acc1c4899a65cb8c930e97dd9958bf37ea922d72b7a06"
MOD_LIBRARY = "librdrvr.so"
LAYER_LIBRARY = "libVkLayer_RDR2_raw_worker28.so"
OPENXR_LIBRARY = "libopenxr_loader.so"
LIBRARY_DIR = "lib/arm64-v8a/"
DATA_FILES = tuple(
    f"assets/data/{name}_android.rpf"
    for name in (
        "act", "animationres", "camera", "flash", "fonts", "fragments", "gringores", "mapres",
        "naturalmotion", "navres", "shaders", "strings", "terrainboundres", "terrainlodres",
        "textures", "treeres", "tune", "vfx", "vgd",
    )
)
LICENSE_FILES = ("OpenXR-LICENSE.txt", "MiamiVR-Quest-LICENSE.txt", "UltimateXR-LICENSE.txt",
                 "NVIDIA-FXAA-3.11.txt")
BUILD_INFO_ENTRY = "assets/rdrvr/build.json"
BOOTSTRAP_PACKAGE = "org/rdrvr/bootstrap"
BOOTSTRAP_DESCRIPTOR = "Lorg/rdrvr/"

# A package re-signed or extended by a third party is refused.
THIRD_PARTY_SIGNER_SHA256 = "358020b0ed9d910c3eb4e03e261eb340604833f8bdc75e236d965e80e63a56de"
THIRD_PARTY_TYPE_PREFIXES = ("Lcom/an1/", "Lm/i/n/")
THIRD_PARTY_LIBRARIES = ("lib/arm64-v8a/liba.so", "lib/arm64-v8a/libNotlib.so")
THIRD_PARTY_MESSAGE = (
    "the package was modified by a third party; "
    "export the game from the phone with EXPORT_RDR_APKS.bat"
)

KEY_ALIAS = "rdrvr"
# The personal key stays on the build machine; the password only guards the file format.
KEY_PASSWORD = "rdrvrrdrvr"
KEY_SUBJECT = "CN=Red Dead Redemption VR Personal Build"
MIN_SDK = "29"
BUILD_TOOLS_VERSION = "35.0.0"
PLATFORM_VERSION = "android-35"
NDK_VERSION = "27.2.12479018"
CMAKE_VERSION = "3.22.1"

GIB = 1024 ** 3
# Subdirectories of the build directory that build.py clears and rewrites.
MANAGED_DIRS = (
    "input", "engine", "generated", "layer", "native", "java-gen", "java-src", "java-stub",
    "classes", "dex", "manifest", "apk", "out",
)
MAX_ARCHIVE_APKS = 64
MAX_APK_BYTES = apkzip.ZIP32_LIMIT
BLOCK = 4 * 1024 * 1024
ACC_PUBLIC = 0x0001
ACC_PROTECTED = 0x0004
ACC_FINAL = 0x0010
ACC_INTERFACE = 0x0200
ACC_ABSTRACT = 0x0400


class KitError(RuntimeError):
    """Expected failure with a plain-language message."""

    def __init__(self, message: str, details: str = ""):
        super().__init__(message)
        self.message = message
        self.details = details


@dataclass
class SelfTest:
    """Self-test hooks. main() never sets them.

    tools/test-apk-tools.py uses them to run the whole pipeline on a synthetic game
    package with a stub engine, a stub OpenXR loader and a synthetic source tree.
    """

    engine_sha256: str
    engine_size: int
    openxr_loader_sha256: str
    kit_root: Path
    third_party_signer_sha256: str = THIRD_PARTY_SIGNER_SHA256


@dataclass
class Options:
    game_package: Path
    build_dir: Path
    sdk: Path
    java_home: Path
    openxr_loader: Path | None = None
    vulkan_headers: Path | None = None
    keystore: Path | None = None
    trade: int = 0
    validate_only: bool = False
    jobs: int = 0
    reuse_build: Path | None = None


@dataclass
class Expected:
    engine_sha256: str = ENGINE_SHA256
    engine_size: int = ENGINE_SIZE
    openxr_loader_sha256: str = OPENXR_LOADER_SHA256
    third_party_signer_sha256: str = THIRD_PARTY_SIGNER_SHA256
    kit_root: Path = KIT_ROOT


@dataclass(frozen=True)
class Toolchain:
    sdk: Path
    java_home: Path
    android_jar: Path
    aapt2: Path
    zipalign: Path
    apksigner: Path
    d8: Path
    java: Path
    javac: Path
    keytool: Path
    cmake: Path
    ninja: Path
    toolchain_file: Path


@dataclass
class InputApk:
    path: Path
    name: str
    size: int
    info: xrmanifest.PackageInfo
    entries: dict[str, zipfile.ZipInfo]
    signer: str = ""


@dataclass
class GamePackage:
    kind: str
    apks: list[InputApk]
    base: InputApk
    engine: InputApk
    base_info: xrmanifest.BaseInfo
    game_dex: str
    loader_dex: str
    external_root: Path | None
    signer: str = ""
    engine_sha256: str = ""
    external: dict = field(default_factory=dict)
    game_dex_compatibility: dict = field(default_factory=dict)


def say(message: str) -> None:
    print(message, flush=True)


def step(message: str) -> None:
    say(f"==> {message}")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(BLOCK), b""):
            digest.update(block)
    return digest.hexdigest()


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(text)


def write_json(path: Path, value) -> None:
    temporary = path.with_name(path.name + ".tmp")
    write_text(temporary, json.dumps(value, indent=2) + "\n")
    os.replace(temporary, path)


def is_within(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def clean_directory(path: Path, build: Path) -> Path:
    if not is_within(path, build) or path.resolve() == build.resolve():
        raise KitError(f"refusing to clear a path outside the build directory: {path}")
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        raise KitError(f"build path is not a plain directory: {path}")
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)
    return path


def remove_file(path: Path) -> None:
    try:
        path.unlink()
    except FileNotFoundError:
        pass


def require_free_space(path: Path, needed: int, purpose: str) -> None:
    probe = path
    while not probe.exists():
        probe = probe.parent
    free = shutil.disk_usage(probe).free
    if free < needed:
        raise KitError(
            f"not enough free disk space for {purpose}: {free / GIB:.1f} GB free, "
            f"about {needed / GIB:.1f} GB needed on the drive of {path}"
        )


class Runner:
    """Run tools with their output in DIR/logs/<name>.log."""

    def __init__(self, logs: Path, env: dict[str, str]):
        self.logs = logs
        self.env = env

    def log_path(self, name: str) -> Path:
        return self.logs / (re.sub(r"[^A-Za-z0-9_.-]+", "-", name).strip("-") + ".log")

    def run(self, name: str, command, failure: str, *, cwd: Path | None = None, capture: bool = False) -> str:
        args = [str(item) for item in command]
        log = self.log_path(name)
        header = "$ " + subprocess.list2cmdline(args) + "\n"
        try:
            if capture:
                result = subprocess.run(
                    args, cwd=cwd, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    stdin=subprocess.DEVNULL,
                )
                stdout = result.stdout.decode("utf-8", errors="replace")
                stderr = result.stderr.decode("utf-8", errors="replace")
                write_text(log, header + stdout + ("\n" + stderr if stderr else ""))
            else:
                with open(log, "w", encoding="utf-8", newline="\n") as stream:
                    stream.write(header)
                    stream.flush()
                    result = subprocess.run(
                        args, cwd=cwd, env=self.env, stdout=stream, stderr=subprocess.STDOUT,
                        stdin=subprocess.DEVNULL,
                    )
                stdout = ""
        except OSError as error:
            raise KitError(f"{failure}: cannot run {args[0]} ({error})") from error
        if result.returncode != 0:
            lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
            tail = "\n".join(lines[-30:])
            raise KitError(f"{failure} (exit code {result.returncode}); log: {log}", tail)
        return stdout


# Toolchain.


def resolve_toolchain(sdk: Path, java_home: Path, *, native: bool) -> Toolchain:
    exe = ".exe" if os.name == "nt" else ""
    build_tools = sdk / "build-tools" / BUILD_TOOLS_VERSION
    ndk = sdk / "ndk" / NDK_VERSION
    cmake_bin = sdk / "cmake" / CMAKE_VERSION / "bin"
    tools = Toolchain(
        sdk=sdk,
        java_home=java_home,
        android_jar=sdk / "platforms" / PLATFORM_VERSION / "android.jar",
        aapt2=build_tools / f"aapt2{exe}",
        zipalign=build_tools / f"zipalign{exe}",
        apksigner=build_tools / "lib" / "apksigner.jar",
        d8=build_tools / "lib" / "d8.jar",
        java=java_home / "bin" / f"java{exe}",
        javac=java_home / "bin" / f"javac{exe}",
        keytool=java_home / "bin" / f"keytool{exe}",
        cmake=cmake_bin / f"cmake{exe}",
        ninja=cmake_bin / f"ninja{exe}",
        toolchain_file=ndk / "build" / "cmake" / "android.toolchain.cmake",
    )
    required = [tools.android_jar, tools.aapt2, tools.apksigner, tools.java]
    if native:
        required += [tools.zipalign, tools.d8, tools.javac, tools.keytool, tools.cmake, tools.ninja,
                     tools.toolchain_file]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise KitError("required build tools are missing", "\n".join(missing))
    return tools


def tool_environment(tools: Toolchain) -> dict[str, str]:
    env = dict(os.environ)
    env["JAVA_HOME"] = str(tools.java_home)
    env["PATH"] = str(tools.java_home / "bin") + os.pathsep + env.get("PATH", "")
    env["ANDROID_HOME"] = str(tools.sdk)
    env["ANDROID_SDK_ROOT"] = str(tools.sdk)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    return env


# Input selection and validation.


def safe_member(name: str) -> PurePosixPath:
    normalized = name.replace("\\", "/")
    member = PurePosixPath(normalized)
    if (
        not member.parts
        or member.is_absolute()
        or ".." in member.parts
        or any(":" in part for part in member.parts)
        or any(ch in normalized for ch in "\0\r\n")
    ):
        raise KitError(f"unsafe archive member: {name!r}")
    return member


def is_split_name(name: str) -> bool:
    return name.startswith("split_") and name.endswith(".apk") and len(name) > len("split_.apk")


def extract_archive(source: Path, destination: Path) -> list[tuple[Path, str]]:
    try:
        archive = zipfile.ZipFile(source)
    except (OSError, zipfile.BadZipFile) as error:
        raise KitError(f"cannot open the archive {source.name}: {error}") from error
    with archive:
        selected = []
        for info in archive.infolist():
            if info.is_dir():
                continue
            member = safe_member(info.filename)
            if member.name == "base.apk" or is_split_name(member.name):
                selected.append((info, member.name))
        names = [name for _, name in selected]
        if "base.apk" not in names:
            raise KitError(f"{source.name} does not contain base.apk")
        if len(names) != len({name.lower() for name in names}):
            raise KitError(f"{source.name} contains the same APK name more than once")
        if len(selected) > MAX_ARCHIVE_APKS:
            raise KitError(f"{source.name} contains too many APK files ({len(selected)})")
        if any(info.file_size >= MAX_APK_BYTES for info, _ in selected):
            raise KitError(f"{source.name} contains an APK larger than 4 GiB")
        total = sum(info.file_size for info, _ in selected)
        require_free_space(destination, total + GIB, "extracting the APK archive")
        result = []
        for info, name in selected:
            target = destination / name
            partial = target.with_name(target.name + ".partial")
            written = 0
            try:
                with archive.open(info) as stream, open(partial, "wb") as output:
                    for block in iter(lambda: stream.read(BLOCK), b""):
                        written += len(block)
                        if written > info.file_size:
                            raise KitError(f"{name} in {source.name} is larger than its recorded size")
                        output.write(block)
                if written != info.file_size:
                    raise KitError(f"{name} in {source.name} was truncated while extracting")
                os.replace(partial, target)
            except (zipfile.BadZipFile, EOFError, zlib.error) as error:
                raise KitError(
                    f"the archive {source.name} is damaged or incomplete: {name} failed its integrity check. "
                    "Obtain a fresh copy of the archive or export the game again with EXPORT_RDR_APKS.bat, "
                    "then select the new archive or export folder. The damaged APK cannot be used.",
                    str(error),
                ) from error
            except OSError as error:
                raise KitError(f"cannot extract {name} from {source.name}: {error}") from error
            finally:
                remove_file(partial)
            result.append((target, name))
    return result


def select_inputs(source: Path, build: Path) -> tuple[str, list[tuple[Path, str]], Path | None]:
    """Return (kind, [(file, output name)], external folder or None)."""
    if source.is_dir():
        base = source / "base.apk"
        if not base.is_file():
            raise KitError(
                f"{source} has no base.apk; select the folder created by EXPORT_RDR_APKS.bat"
            )
        files = [(base, "base.apk")]
        for path in sorted(source.iterdir()):
            if path.is_file() and is_split_name(path.name):
                files.append((path, path.name))
            elif path.is_file() and path.suffix.lower() == ".apk" and path.name != "base.apk":
                say(f"    ignoring {path.name}")
        external = source / "external"
        return "folder", files, external if external.is_dir() else None
    if not source.is_file():
        raise KitError(f"the game package does not exist: {source}")
    suffix = source.suffix.lower()
    if suffix == ".apk":
        if source.name == "base.apk":
            files = [(source, "base.apk")]
            files += [
                (p, p.name) for p in sorted(source.parent.iterdir()) if p.is_file() and is_split_name(p.name)
            ]
            external = source.parent / "external"
            return "base", files, external if external.is_dir() else None
        if is_split_name(source.name):
            raise KitError("select base.apk or the export folder, not a split APK")
        return "single", [(source, "base.apk")], None
    if suffix in (".apks", ".zip"):
        destination = clean_directory(build / "input", build)
        return "archive", extract_archive(source, destination), None
    raise KitError("the game package must be an export folder, base.apk, a single .apk, or an .apks/.zip archive")


def open_apk(path: Path, name: str) -> InputApk:
    try:
        size = path.stat().st_size
        if size >= MAX_APK_BYTES:
            raise KitError(f"{name} is larger than 4 GiB")
        with zipfile.ZipFile(path) as archive:
            entries = apkzip.inventory(archive)
            if "AndroidManifest.xml" not in entries:
                raise KitError(f"{name} has no AndroidManifest.xml")
            manifest = archive.read("AndroidManifest.xml")
    except (OSError, zipfile.BadZipFile, apkzip.ApkZipError) as error:
        raise KitError(f"{name} is not a readable APK: {error}") from error
    try:
        info = xrmanifest.read_package_info(manifest)
    except xrmanifest.ManifestError as error:
        raise KitError(f"{name}: {error}") from error
    return InputApk(path=path, name=name, size=size, info=info, entries=entries)


def dex_names(entries) -> list[str]:
    """classes.dex, classes2.dex, ... in order; gaps are refused."""
    indices = {}
    for name in entries:
        match = re.fullmatch(r"classes(\d*)\.dex", name)
        if match:
            number = int(match.group(1)) if match.group(1) else 1
            if match.group(1) in ("0", "1") or match.group(1).startswith("0"):
                raise KitError(f"unexpected DEX entry name: {name}")
            indices[number] = name
    if not indices:
        return []
    if sorted(indices) != list(range(1, len(indices) + 1)):
        raise KitError(f"the DEX numbering has gaps: {sorted(indices.values())}")
    return [indices[i] for i in range(1, len(indices) + 1)]


def read_entry(path: Path, name: str) -> bytes:
    with zipfile.ZipFile(path) as archive:
        return archive.read(name)


def check_third_party_markers(apks: list[InputApk]) -> None:
    for apk in apks:
        if any(name in apk.entries for name in THIRD_PARTY_LIBRARIES):
            raise KitError(THIRD_PARTY_MESSAGE)
    for apk in apks:
        for name in dex_names(apk.entries):
            try:
                types = dexpatch.Dex(read_entry(apk.path, name)).types()
            except dexpatch.DexError as error:
                raise KitError(f"{apk.name}:{name} is not a readable DEX file: {error}") from error
            if any(t.startswith(THIRD_PARTY_TYPE_PREFIXES) for t in types):
                raise KitError(THIRD_PARTY_MESSAGE)
            if any(t.startswith(BOOTSTRAP_DESCRIPTOR) for t in types):
                raise KitError(
                    "the package already contains the VR loader; export the original game "
                    "from the phone with EXPORT_RDR_APKS.bat"
                )


def java_class_name(name: str) -> str:
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*)+", name):
        raise KitError(f"unsupported application class name in the manifest: {name}")
    return name


def check_application_class(base: InputApk, names: list[str], class_name: str) -> None:
    """The original Application class must be a usable superclass for RdrVrApplication."""
    java_class_name(class_name)
    dexes = [dexpatch.Dex(read_entry(base.path, name)) for name in names]

    def lookup(descriptor: str):
        for dex in dexes:
            found = dex.class_info(descriptor)
            if found is not None:
                return dex, found
        return None, None

    descriptor = "L" + class_name.replace(".", "/") + ";"
    dex, found = lookup(descriptor)
    if found is None:
        raise KitError(f"the application class {class_name} is not in the base APK")
    flags, superclass = found
    if flags & (ACC_FINAL | ACC_INTERFACE | ACC_ABSTRACT):
        raise KitError(f"the application class {class_name} cannot be extended")
    constructors = [
        access
        for index, access, _ in dex.methods(descriptor)
        if dex.method(index) == (descriptor, "<init>", ("V", ()))
    ]
    if len(constructors) != 1 or not constructors[0] & (ACC_PUBLIC | ACC_PROTECTED):
        raise KitError(f"the application class {class_name} has no accessible no-argument constructor")
    for _ in range(32):
        if superclass == "Landroid/app/Application;":
            return
        if superclass is None:
            break
        _, parent = lookup(superclass)
        if parent is None:
            break
        superclass = parent[1]
    raise KitError(f"the application class {class_name} does not extend android.app.Application")


def apk_signer(runner: Runner, tools: Toolchain, path: Path, label: str) -> str:
    output = runner.run(
        f"apksigner-verify-{label}",
        [tools.java, "-jar", tools.apksigner, "verify", "--min-sdk-version", MIN_SDK, "--print-certs", path],
        f"the APK signature of {label} is not valid",
        capture=True,
    )
    match = re.search(r"Signer #1 certificate SHA-256 digest:\s*([0-9a-fA-F]{64})", output)
    if not match:
        raise KitError(f"cannot read the signer certificate of {label}")
    return match.group(1).lower()


def badging(runner: Runner, tools: Toolchain, path: Path, label: str) -> dict[str, str]:
    output = runner.run(
        f"aapt2-badging-{label}",
        [tools.aapt2, "dump", "badging", path],
        f"aapt2 cannot read {label}",
        capture=True,
    )
    line = next((item for item in output.splitlines() if item.startswith("package:")), "")
    values = dict(re.findall(r"\b(\w+)='([^']*)'", line))
    launchable = re.search(r"^launchable-activity: name='([^']+)'", output, re.MULTILINE)
    if launchable:
        values["launchable-activity"] = launchable.group(1)
    return values


def list_external(root: Path | None) -> dict:
    result = {"root": None, "obb": [], "assetpacks": []}
    if root is None or not root.is_dir():
        return result
    result["root"] = str(root.resolve())
    for category in ("obb", "assetpacks"):
        folder = root / category
        if not folder.is_dir() or folder.is_symlink():
            continue
        for directory, subdirectories, files in os.walk(folder):
            subdirectories[:] = sorted(d for d in subdirectories if not (Path(directory) / d).is_symlink())
            for name in sorted(files):
                path = Path(directory) / name
                if path.is_symlink() or not path.is_file():
                    continue
                result[category].append(
                    {
                        "path": path.relative_to(root).as_posix(),
                        "size": path.stat().st_size,
                        "sha256": sha256_file(path),
                    }
                )
    return result


NETFLIX_MESSAGE = (
    "this is the Netflix Games edition (com.netflix.NGP.Kamo). Its game engine is a different build from the "
    "Google Play edition (same version number, different code), and the mod hooks into the Google Play engine. "
    "Export the Google Play version of Red Dead Redemption instead."
)


def validate_package(
    options: Options, build: Path, tools: Toolchain, runner: Runner, expected: Expected
) -> GamePackage:
    kind, files, external = select_inputs(options.game_package, build)
    apks = [open_apk(path, name) for path, name in files]
    for apk in apks:
        if apk.info.package == xrmanifest.NETFLIX_PACKAGE:
            raise KitError(f"{apk.name}: {NETFLIX_MESSAGE}")
        if apk.info.package != PACKAGE_NAME:
            raise KitError(f"{apk.name} belongs to {apk.info.package}, not {PACKAGE_NAME}")
        if apk.info.version_code != VERSION_CODE:
            raise KitError(
                f"{apk.name} is version code {apk.info.version_code}; "
                f"this kit supports {VERSION_NAME} ({VERSION_CODE})"
            )
        if apk.info.version_name not in (None, VERSION_NAME):
            raise KitError(f"{apk.name} is version {apk.info.version_name}; this kit supports {VERSION_NAME}")
    bases = [apk for apk in apks if apk.info.split is None]
    if len(bases) != 1 or bases[0].name != "base.apk":
        raise KitError(f"expected one base APK named base.apk, found {len(bases)} base APKs")
    base = bases[0]
    if base.info.version_name != VERSION_NAME:
        raise KitError(f"base.apk is not version {VERSION_NAME}")
    splits = [apk.info.split for apk in apks if apk.info.split is not None]
    if len(splits) != len(set(splits)):
        raise KitError("the package contains the same split more than once")
    apks = [base] + sorted((apk for apk in apks if apk is not base), key=lambda apk: apk.name)

    check_third_party_markers(apks)

    engines = [apk for apk in apks if ENGINE_ENTRY in apk.entries]
    if len(engines) != 1:
        raise KitError(
            f"expected exactly one APK with {ENGINE_ENTRY}, found {len(engines)}; the export must "
            "include split_config.arm64_v8a.apk from a 64-bit ARM phone"
        )
    engine = engines[0]
    if engine.entries[ENGINE_ENTRY].file_size != expected.engine_size:
        raise KitError(f"unsupported game engine: {ENGINE_ENTRY} has an unexpected size")
    present = set().union(*(apk.entries for apk in apks))
    missing = [name for name in DATA_FILES if name not in present]
    if missing:
        raise KitError(
            "the game data is incomplete; launch the game on the phone once, let it finish "
            "downloading, then export it again",
            "\n".join(missing),
        )

    try:
        base_info = xrmanifest.inspect_base(read_entry(base.path, "AndroidManifest.xml"))
    except xrmanifest.ManifestError as error:
        raise KitError(f"the base APK manifest is not supported: {error}") from error
    names = dex_names(base.entries)
    owners = []
    for name in names:
        try:
            if dexpatch.defines_owner(read_entry(base.path, name)):
                owners.append(name)
        except dexpatch.DexError as error:
            raise KitError(f"base.apk:{name} is not a readable DEX file: {error}") from error
    if len(owners) != 1:
        raise KitError(f"expected one DEX defining {dexpatch.OWNER}, found {len(owners)}")
    try:
        _, dex_compatibility = dexpatch.patch_game_manager(read_entry(base.path, owners[0]))
    except dexpatch.DexError as error:
        raise KitError(f"the game DEX is not supported: {error}") from error
    if base_info.application_class:
        check_application_class(base, names, base_info.application_class)

    values = badging(runner, tools, base.path, "base.apk")
    if (values.get("name"), values.get("versionCode"), values.get("versionName")) != (
        PACKAGE_NAME, VERSION_CODE, VERSION_NAME,
    ) or values.get("split"):
        raise KitError("aapt2 reports a different identity for base.apk")

    step("checking the APK signatures")
    for apk in apks:
        apk.signer = apk_signer(runner, tools, apk.path, apk.name)
        if apk.signer == expected.third_party_signer_sha256:
            raise KitError(THIRD_PARTY_MESSAGE)
    signers = {apk.signer for apk in apks}
    if len(signers) != 1:
        raise KitError("the APKs are not signed by one certificate; export them again from one phone")

    package = GamePackage(
        kind=kind,
        apks=apks,
        base=base,
        engine=engine,
        base_info=base_info,
        game_dex=owners[0],
        loader_dex=f"classes{len(names) + 1}.dex",
        external_root=external,
        signer=base.signer,
        game_dex_compatibility=dex_compatibility,
    )
    return package


def hash_engine(package: GamePackage, destination: Path | None, expected: Expected) -> None:
    try:
        size, digest = apkzip.entry_digest(package.engine.path, ENGINE_ENTRY, destination)
    except (OSError, zipfile.BadZipFile, apkzip.ApkZipError) as error:
        raise KitError(f"cannot read {ENGINE_ENTRY}: {error}") from error
    if size != expected.engine_size or digest != expected.engine_sha256:
        if destination is not None:
            remove_file(destination)
        raise KitError(
            f"unsupported game engine: {ENGINE_ENTRY} sha256 {digest}; "
            f"this kit supports {VERSION_NAME} ({expected.engine_sha256})"
        )
    package.engine_sha256 = digest


# Sources and native build.


def source_build_id(kit: Path) -> str:
    lines = []
    for top in ("native", "layer", "loader", "assets"):
        folder = kit / top
        if not folder.is_dir():
            raise KitError(f"the source kit is incomplete: {top}/ is missing")
        for directory, subdirectories, files in os.walk(folder):
            subdirectories.sort()
            for name in files:
                path = Path(directory) / name
                if path.is_file():
                    lines.append(f"{path.relative_to(kit).as_posix()}\t{sha256_file(path)}\n")
    lines.sort()
    return hashlib.sha256("".join(lines).encode("utf-8")).hexdigest()


def elf_header(path: Path) -> bytes:
    """ELF header of a little-endian ELF64 AArch64 shared object."""
    with open(path, "rb") as stream:
        header = stream.read(64)
    if len(header) < 64 or header[:4] != b"\x7fELF" or header[4] != 2 or header[5] != 1:
        raise KitError(f"{path.name} is not a 64-bit little-endian ELF file")
    e_type, e_machine = struct.unpack_from("<HH", header, 16)
    if e_type != 3 or e_machine != 183:
        raise KitError(f"{path.name} is not an AArch64 shared library")
    return header


def elf_build_id(path: Path) -> bytes:
    """GNU build-id note (NT_GNU_BUILD_ID) of an AArch64 shared object."""
    header = elf_header(path)
    with open(path, "rb") as stream:
        phoff = struct.unpack_from("<Q", header, 32)[0]
        shoff = struct.unpack_from("<Q", header, 40)[0]
        phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", header, 54)
        regions = []
        for index in range(phnum):
            stream.seek(phoff + index * phentsize)
            entry = stream.read(56)
            if len(entry) == 56 and struct.unpack_from("<I", entry, 0)[0] == 4:
                regions.append((struct.unpack_from("<Q", entry, 8)[0], struct.unpack_from("<Q", entry, 32)[0]))
        if not regions:
            for index in range(shnum):
                stream.seek(shoff + index * shentsize)
                entry = stream.read(64)
                if len(entry) == 64 and struct.unpack_from("<I", entry, 4)[0] == 7:
                    regions.append((struct.unpack_from("<Q", entry, 24)[0], struct.unpack_from("<Q", entry, 32)[0]))
        for offset, size in regions:
            stream.seek(offset)
            notes = stream.read(size)
            pos = 0
            while pos + 12 <= len(notes):
                namesz, descsz, kind = struct.unpack_from("<III", notes, pos)
                name_start = pos + 12
                desc_start = name_start + ((namesz + 3) & ~3)
                name = notes[name_start:name_start + namesz]
                desc = notes[desc_start:desc_start + descsz]
                if kind == 3 and name == b"GNU\0" and len(desc) == descsz:
                    if descsz != 20:
                        raise KitError(f"{path.name} has a {descsz}-byte build-id; 20 bytes are required")
                    return desc
                pos = desc_start + ((descsz + 3) & ~3)
    raise KitError(f"{path.name} has no GNU build-id note")


def build_id_header(build_id: bytes) -> str:
    values = ",".join(f"0x{byte:02x}" for byte in build_id)
    return (
        "#pragma once\n"
        "namespace rdr2_game_worker {\n"
        f"inline constexpr unsigned char ExpectedLayerBuildId[]={{{values}}};\n"
        "}\n"
    )


def cmake_toolchain_args(tools: Toolchain) -> list[str]:
    return [
        "-G", "Ninja",
        f"-DCMAKE_MAKE_PROGRAM={tools.ninja.as_posix()}",
        f"-DCMAKE_TOOLCHAIN_FILE={tools.toolchain_file.as_posix()}",
        "-DANDROID_ABI=arm64-v8a",
        "-DANDROID_PLATFORM=android-29",
        "-DANDROID_STL=c++_static",
    ]


def run_generators(kit: Path, build: Path, runner: Runner) -> Path:
    generated = build / "generated"
    engine = build / "engine" / "librdr.so"
    runner.run(
        "gamecode",
        [sys.executable, kit / "tools" / "gamecode.py", "--engine", engine,
         "--ranges", kit / "native" / "gamecode" / "ranges.json", "--out", generated],
        "game code generation failed",
        cwd=kit,
    )
    runner.run(
        "hands-assets",
        [sys.executable, kit / "tools" / "hands_assets.py", "--assets", kit / "assets" / "vrhands",
         "--out", generated],
        "VR hand asset generation failed",
        cwd=kit,
    )
    return generated


def build_native(
    kit: Path, build: Path, tools: Toolchain, runner: Runner, options: Options, build_id: str
) -> tuple[Path, Path]:
    generated = build / "generated"
    layer_dir = clean_directory(build / "layer", build)
    step("building the Vulkan layer")
    runner.run(
        "layer-configure",
        [tools.cmake, "-S", kit / "layer", "-B", layer_dir, *cmake_toolchain_args(tools),
         "-DCMAKE_BUILD_TYPE=Release",
         f"-DVULKAN_HEADERS_DIR={options.vulkan_headers.as_posix()}",
         f"-DRDRVR_GENERATED_DIR={generated.as_posix()}"],
        "configuring the Vulkan layer failed",
    )
    runner.run("layer-build", [tools.cmake, "--build", layer_dir], "building the Vulkan layer failed")
    layer = layer_dir / LAYER_LIBRARY
    if not layer.is_file():
        raise KitError(f"the layer build did not produce {LAYER_LIBRARY}")
    write_text(generated / "WorkerLayerBuildId.h", build_id_header(elf_build_id(layer)))

    native_dir = clean_directory(build / "native", build)
    step(f"building {MOD_LIBRARY}")
    runner.run(
        "native-configure",
        [tools.cmake, "-S", kit / "native", "-B", native_dir, *cmake_toolchain_args(tools),
         "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
         f"-DRDRVR_BUILD_ID={build_id}",
         f"-DOPENXR_LOADER_SO={options.openxr_loader.as_posix()}",
         f"-DRDRVR_GENERATED_DIR={generated.as_posix()}"],
        f"configuring {MOD_LIBRARY} failed",
    )
    jobs = options.jobs if options.jobs > 0 else (os.cpu_count() or 4)
    runner.run(
        "native-build",
        [tools.cmake, "--build", native_dir, "-j", str(jobs)],
        f"building {MOD_LIBRARY} failed",
    )
    library = native_dir / MOD_LIBRARY
    if not library.is_file():
        raise KitError(f"the native build did not produce {MOD_LIBRARY}")
    elf_header(library)
    return layer, library


# Java loader.


def build_config_source(build_id: str, engine_sha256: str, launcher: str) -> str:
    return (
        "package org.rdrvr.bootstrap;\n"
        "final class BuildConfig {\n"
        "static final boolean PROBE=false;\n"
        "static final boolean BENCH_CONTROL=true;\n"
        "static final boolean TRACE=true;\n"
        f'static final String BUILD_ID="{build_id}";\n'
        f'static final String ENGINE_SHA="{engine_sha256}";\n'
        f'static final String LAUNCHER="{launcher}";\n'
        f'static final String MOD_VERSION="{MOD_VERSION}";\n'
        "}\n"
    )


def rewrite_superclass(source: str, class_name: str) -> str:
    pattern = re.compile(r"\bextends\s+Application\b")
    if len(pattern.findall(source)) != 1:
        raise KitError("RdrVrApplication.java must contain exactly one 'extends Application'")
    return pattern.sub(f"extends {class_name}", source)


def build_loader(
    kit: Path, build: Path, tools: Toolchain, runner: Runner, package: GamePackage, build_id: str
) -> bytes:
    step("compiling the Java loader")
    loader_root = kit / "loader" / "src"
    sources = sorted(loader_root.rglob("*.java"))
    application_source = loader_root / BOOTSTRAP_PACKAGE / "RdrVrApplication.java"
    if application_source not in sources:
        raise KitError("the source kit is incomplete: RdrVrApplication.java is missing")
    config = clean_directory(build / "java-gen", build) / BOOTSTRAP_PACKAGE / "BuildConfig.java"
    write_text(config, build_config_source(build_id, package.engine_sha256, package.base_info.launcher))
    classes = clean_directory(build / "classes", build)
    dex_dir = clean_directory(build / "dex", build)
    stub_root = clean_directory(build / "java-stub", build)
    copy_root = clean_directory(build / "java-src", build)
    javac = [tools.javac, "-encoding", "UTF-8", "-source", "8", "-target", "8",
             "-bootclasspath", tools.android_jar]
    d8_extra: list[object] = []
    superclass = package.base_info.application_class
    if superclass:
        java_class_name(superclass)
        copy = copy_root / BOOTSTRAP_PACKAGE / "RdrVrApplication.java"
        write_text(copy, rewrite_superclass(application_source.read_text(encoding="utf-8"), superclass))
        sources = [copy if path == application_source else path for path in sources]
        package_name, simple = superclass.rsplit(".", 1)
        stub_source = stub_root / "src" / Path(*package_name.split(".")) / f"{simple}.java"
        write_text(
            stub_source,
            f"package {package_name};\n\n"
            f"public class {simple} extends android.app.Application {{\n"
            f"    public {simple}() {{\n    }}\n}}\n",
        )
        stub_classes = stub_root / "classes"
        stub_classes.mkdir()
        runner.run("javac-application-stub", [*javac, "-d", stub_classes, stub_source],
                   "compiling the application class stub failed")
        stub_jar = stub_root / "stub.jar"
        with zipfile.ZipFile(stub_jar, "w") as jar:
            for path in sorted(stub_classes.rglob("*.class")):
                jar.write(path, path.relative_to(stub_classes).as_posix())
        javac += ["-classpath", stub_classes]
        d8_extra = ["--classpath", stub_jar]
    runner.run("javac", [*javac, "-d", classes, *sources, config], "compiling the Java loader failed")
    class_files = sorted(classes.rglob("*.class"))
    if not class_files:
        raise KitError("the Java compiler produced no class files")
    runner.run(
        "d8",
        [tools.java, "-cp", tools.d8, "com.android.tools.r8.D8", "--release", "--min-api", MIN_SDK,
         "--lib", tools.android_jar, *d8_extra, "--output", dex_dir, *class_files],
        "converting the Java loader to DEX failed",
    )
    produced = sorted(p.name for p in dex_dir.iterdir())
    if produced != ["classes.dex"]:
        raise KitError(f"D8 produced unexpected files: {produced}")
    data = (dex_dir / "classes.dex").read_bytes()
    dex = dexpatch.Dex(data)
    defined = [dex.type(dex.u32(dex.class_offset + 32 * i)) for i in range(dex.class_count)]
    outside = [name for name in defined if not name.startswith(BOOTSTRAP_DESCRIPTOR)]
    if outside:
        raise KitError(f"the loader DEX defines classes outside org.rdrvr: {outside[:5]}")
    application = dex.class_info(f"L{BOOTSTRAP_PACKAGE}/RdrVrApplication;")
    if application is None or dex.class_info(f"L{BOOTSTRAP_PACKAGE}/PreflightActivity;") is None:
        raise KitError("the loader DEX lacks RdrVrApplication or PreflightActivity")
    wanted = "L" + superclass.replace(".", "/") + ";" if superclass else "Landroid/app/Application;"
    if application[1] != wanted:
        raise KitError(f"RdrVrApplication extends {application[1]}, expected {wanted}")
    return data


# Keystore and signing.


def keystore_command(tools: Toolchain, keystore: Path, *extra) -> list:
    return [tools.keytool, *extra, "-keystore", keystore, "-storepass", KEY_PASSWORD, "-alias", KEY_ALIAS]


def ensure_keystore(tools: Toolchain, runner: Runner, keystore: Path) -> None:
    if keystore.is_file():
        runner.run(
            "keytool-check",
            keystore_command(tools, keystore, "-list"),
            f"the signing key {keystore} cannot be opened or has no '{KEY_ALIAS}' entry",
            capture=True,
        )
        return
    if keystore.exists():
        raise KitError(f"the signing key path is not a file: {keystore}")
    step("creating the personal signing key")
    keystore.parent.mkdir(parents=True, exist_ok=True)
    temporary = keystore.with_name(f"{keystore.name}.{os.getpid()}.tmp")
    remove_file(temporary)
    try:
        runner.run(
            "keytool-create",
            [tools.keytool, "-genkeypair", "-noprompt", "-keystore", temporary, "-storetype", "PKCS12",
             "-alias", KEY_ALIAS, "-keyalg", "RSA", "-keysize", "3072", "-validity", "10000",
             "-storepass", KEY_PASSWORD, "-keypass", KEY_PASSWORD, "-dname", KEY_SUBJECT],
            "creating the personal signing key failed",
        )
        if keystore.exists():
            raise KitError(f"the signing key appeared while it was being created: {keystore}")
        os.replace(temporary, keystore)
    finally:
        remove_file(temporary)


def sign_apk(tools: Toolchain, runner: Runner, keystore: Path, aligned: Path, signed: Path) -> None:
    runner.run(
        f"apksigner-sign-{signed.name}",
        [tools.java, "-jar", tools.apksigner, "sign", "--ks", keystore, "--ks-key-alias", KEY_ALIAS,
         "--ks-pass", f"pass:{KEY_PASSWORD}", "--key-pass", f"pass:{KEY_PASSWORD}",
         "--min-sdk-version", MIN_SDK, "--v1-signing-enabled", "false", "--v2-signing-enabled", "true",
         "--v3-signing-enabled", "true", "--v4-signing-enabled", "false",
         "--alignment-preserved", "true", "--out", signed, aligned],
        f"signing {signed.name} failed",
    )


# Assembly.


def compression_of(apk: InputApk, name: str, default: int) -> int:
    entry = apk.entries.get(name)
    return entry.compress_type if entry is not None else default


def plan_changes(
    package: GamePackage,
    apk: InputApk,
    manifest: bytes,
    game_dex: bytes,
    loader_dex: bytes,
    libraries: dict[str, bytes],
    build_info: bytes,
    licenses: dict[str, bytes],
) -> dict[str, tuple[bytes, int]]:
    changes: dict[str, tuple[bytes, int]] = {}
    added: list[str] = []
    if apk is package.base:
        dex_method = compression_of(apk, package.game_dex, zipfile.ZIP_DEFLATED)
        changes["AndroidManifest.xml"] = (manifest, compression_of(apk, "AndroidManifest.xml", zipfile.ZIP_DEFLATED))
        changes[package.game_dex] = (game_dex, dex_method)
        changes[package.loader_dex] = (loader_dex, dex_method)
        added.append(package.loader_dex)
        for name, data in licenses.items():
            changes[f"assets/licenses/{name}"] = (data, zipfile.ZIP_DEFLATED)
            added.append(f"assets/licenses/{name}")
        changes[BUILD_INFO_ENTRY] = (build_info, zipfile.ZIP_DEFLATED)
        added.append(BUILD_INFO_ENTRY)
    if apk is package.engine:
        for name, data in libraries.items():
            changes[LIBRARY_DIR + name] = (data, zipfile.ZIP_DEFLATED)
            added.append(LIBRARY_DIR + name)
    collisions = sorted(name for name in added if name in apk.entries)
    if collisions:
        raise KitError(f"{apk.name} already contains {', '.join(collisions)}")
    return changes


def assemble(
    package: GamePackage,
    build: Path,
    tools: Toolchain,
    runner: Runner,
    keystore: Path,
    contents: dict,
    reuse_build: Path | None = None,
) -> tuple[list[dict], str]:
    work = clean_directory(build / "apk", build)
    out = clean_directory(build / "out", build)
    records = []
    signers = set()
    for apk in package.apks:
        changes = plan_changes(package, apk, **contents)
        unsigned = work / f"{apk.name}.unsigned"
        aligned = work / f"{apk.name}.aligned"
        signed = out / apk.name
        reused = False
        if reuse_build is not None and not changes and len(signers) == 1:
            cached = reuse_build / "out" / apk.name
            try:
                # Match the actual payload and current key, not merely the old manifest or filename.
                certificate = apkzip.signer_certificate(cached)
                if certificate in signers:
                    apkzip.verify_payload(apk.path, cached, {})
                    if apk_signer(runner, tools, cached, f"cache-{apk.name}") != hashlib.sha256(certificate).hexdigest():
                        raise KitError("cached APK signing block differs from apksigner")
                    runner.run(f"cache-align-{apk.name}", [tools.zipalign, "-c", "-P", "16", "4", cached],
                               "cached APK alignment failed")
                    shutil.copyfile(cached, signed)
                    reused = True
                    step(f"reusing verified {apk.name}; no repackaging or signing needed")
            except (OSError, ValueError, zipfile.BadZipFile, apkzip.ApkZipError, KitError):
                remove_file(signed)
                say(f"    cached {apk.name} cannot be reused; rebuilding it")
        if not reused:
            step(f"assembling {apk.name}")
            try:
                apkzip.write_apk(apk.path, unsigned, changes)
            except (OSError, apkzip.ApkZipError) as error:
                raise KitError(f"cannot assemble {apk.name}: {error}") from error
            runner.run(f"zipalign-{apk.name}", [tools.zipalign, "-P", "16", "-f", "4", unsigned, aligned],
                       f"aligning {apk.name} failed")
            remove_file(unsigned)
            step(f"signing {apk.name}")
            sign_apk(tools, runner, keystore, aligned, signed)
            remove_file(aligned)
        signer = apk_signer(runner, tools, signed, f"output-{apk.name}")
        runner.run(f"zipalign-check-{apk.name}", [tools.zipalign, "-c", "-P", "16", "4", signed],
                   f"{apk.name} is not aligned")
        try:
            apkzip.verify_payload(apk.path, signed, {name: data for name, (data, _) in changes.items()})
            certificate = apkzip.signer_certificate(signed)
        except (OSError, apkzip.ApkZipError) as error:
            raise KitError(f"the signed {apk.name} does not match its inputs: {error}") from error
        if hashlib.sha256(certificate).hexdigest() != signer:
            raise KitError(f"the signing block of {apk.name} does not match apksigner")
        signers.add(certificate)
        records.append(
            {
                "file": apk.name,
                "split": apk.info.split,
                "size": signed.stat().st_size,
                "sha256": sha256_file(signed),
            }
        )
    if len(signers) != 1:
        raise KitError("the output APKs are not signed by one certificate")
    shutil.rmtree(work, ignore_errors=True)
    return records, signers.pop()


def check_output_manifest(
    package: GamePackage, build: Path, tools: Toolchain, runner: Runner, trade: int
) -> None:
    step("checking the output manifest")
    output = build / "out" / "base.apk"
    values = badging(runner, tools, output, "output-base.apk")
    if (values.get("name"), values.get("versionCode"), values.get("versionName")) != (
        PACKAGE_NAME, VERSION_CODE, VERSION_NAME,
    ):
        raise KitError("aapt2 reports a different identity for the output base.apk")
    if values.get("launchable-activity") != xrmanifest.PREFLIGHT_ACTIVITY:
        raise KitError("the output base.apk does not launch PreflightActivity")
    command = [tools.aapt2, "dump", "xmltree", "--file", "AndroidManifest.xml"]
    original = runner.run("manifest-original", [*command, package.base.path],
                          "aapt2 cannot read the original manifest", capture=True)
    result = runner.run("manifest-result", [*command, output], "aapt2 cannot read the output manifest",
                        capture=True)
    try:
        xrmanifest.verify_tree(original, result, trade)
    except xrmanifest.ManifestError as error:
        raise KitError(f"the output manifest check failed: {error}") from error


def kit_files(kit: Path) -> dict[str, Path]:
    files = {
        "layer project": kit / "layer" / "CMakeLists.txt",
        "native project": kit / "native" / "CMakeLists.txt",
        "game code ranges": kit / "native" / "gamecode" / "ranges.json",
        "game code generator": kit / "tools" / "gamecode.py",
        "hand asset generator": kit / "tools" / "hands_assets.py",
        "OpenXR access manifest": kit / "loader" / "openxr-access" / "AndroidManifest.xml",
    }
    for name in LICENSE_FILES:
        files[name] = kit / "assets" / "licenses" / name
    missing = [f"{label}: {path}" for label, path in files.items() if not path.is_file()]
    if not (kit / "assets" / "vrhands").is_dir():
        missing.append(f"VR hand assets: {kit / 'assets' / 'vrhands'}")
    if missing:
        raise KitError("the source kit is incomplete", "\n".join(missing))
    return files


def check_protected(label: str, path: Path, build: Path) -> None:
    if is_within(build, path):
        raise KitError(f"the build directory must not be inside the {label}: {build}")
    for name in (*MANAGED_DIRS, "logs"):
        if is_within(path, build / name):
            raise KitError(f"the {label} must not be inside {build / name}: {path}")


def check_options(options: Options, build: Path, kit: Path, expected: Expected) -> None:
    if build == Path(build.anchor) or is_within(kit, build):
        raise KitError(f"the build directory must not be the source kit or one of its parents: {build}")
    for top in ("native", "layer", "loader", "assets", "tools"):
        if is_within(build, kit / top):
            raise KitError(f"the build directory must not be inside the source folder {kit / top}")
    for name in MANAGED_DIRS:
        if is_within(kit, build / name):
            raise KitError(f"the source kit must not be inside {build / name}")
    check_protected("game package", options.game_package, build)
    check_protected("Android SDK", options.sdk, build)
    check_protected("JDK", options.java_home, build)
    if options.reuse_build is not None:
        if is_within(options.reuse_build, build) or is_within(build, options.reuse_build):
            raise KitError("--reuse-build must be separate from the new build directory")
    if options.trade not in (-1, 0, 1):
        raise KitError("--trade must be -1, 0 or 1")
    if options.validate_only:
        return
    for label, value in (("--openxr-loader", options.openxr_loader), ("--vulkan-headers", options.vulkan_headers),
                         ("--keystore", options.keystore)):
        if value is None:
            raise KitError(f"{label} is required to build")
    if not options.openxr_loader.is_file():
        raise KitError(f"the OpenXR loader does not exist: {options.openxr_loader}")
    if sha256_file(options.openxr_loader) != expected.openxr_loader_sha256:
        raise KitError("the OpenXR loader does not match the pinned Khronos 1.1.43 arm64 build")
    if not (options.vulkan_headers / "include" / "vulkan" / "vulkan_core.h").is_file():
        raise KitError(f"--vulkan-headers has no include/vulkan/vulkan_core.h: {options.vulkan_headers}")
    for label, value in (("OpenXR loader", options.openxr_loader), ("Vulkan headers", options.vulkan_headers),
                         ("signing key", options.keystore)):
        check_protected(label, value, build)


def run(options: Options, self_test: SelfTest | None = None) -> int:
    expected = Expected()
    if self_test is not None:
        expected = Expected(
            engine_sha256=self_test.engine_sha256,
            engine_size=self_test.engine_size,
            openxr_loader_sha256=self_test.openxr_loader_sha256,
            third_party_signer_sha256=self_test.third_party_signer_sha256,
            kit_root=self_test.kit_root,
        )
    kit = expected.kit_root.resolve()
    build = options.build_dir
    check_options(options, build, kit, expected)
    tools = resolve_toolchain(options.sdk, options.java_home, native=not options.validate_only)
    files = {} if options.validate_only else kit_files(kit)
    build.mkdir(parents=True, exist_ok=True)
    for stale in ("build-manifest.json", "validation.json"):
        remove_file(build / stale)
    for stale in ("out", "apk"):
        if (build / stale).exists():
            clean_directory(build / stale, build)
    logs = build / "logs"
    if logs.is_symlink() or (logs.exists() and not logs.is_dir()):
        raise KitError(f"build path is not a plain directory: {logs}")
    logs.mkdir(exist_ok=True)
    runner = Runner(logs, tool_environment(tools))

    step("checking the game package")
    package = validate_package(options, build, tools, runner, expected)
    say(f"Game DEX compatibility: {package.game_dex_compatibility['status']}")
    package.external = list_external(package.external_root)
    if options.validate_only:
        step("hashing the game engine")
        hash_engine(package, None, expected)
        write_json(build / "validation.json", validation_record(package))
        say(f"Validation passed: {len(package.apks)} APK(s), engine {package.engine_sha256}")
        say(f"Report: {build / 'validation.json'}")
        return 0

    total = sum(apk.size for apk in package.apks)
    largest = max(apk.size for apk in package.apks)
    require_free_space(build, total + largest + expected.engine_size + 2 * GIB, "the build")
    ensure_keystore(tools, runner, options.keystore)

    step("extracting the game engine")
    engine_dir = clean_directory(build / "engine", build)
    engine_path = engine_dir / "librdr.so"
    hash_engine(package, engine_path, expected)

    step("preparing the manifest and the game DEX")
    manifest_dir = clean_directory(build / "manifest", build)
    fragment_apk = manifest_dir / "openxr-access.apk"
    runner.run(
        "openxr-access",
        [tools.aapt2, "link", "--manifest", files["OpenXR access manifest"], "-I", tools.android_jar,
         "-o", fragment_apk],
        "compiling the OpenXR access manifest failed",
    )
    try:
        manifest, _ = xrmanifest.transform(
            read_entry(package.base.path, "AndroidManifest.xml"),
            read_entry(fragment_apk, "AndroidManifest.xml"),
            options.trade,
        )
        game_dex, _ = dexpatch.patch_game_manager(read_entry(package.base.path, package.game_dex))
    except (xrmanifest.ManifestError, dexpatch.DexError) as error:
        raise KitError(f"cannot prepare the base APK: {error}") from error
    (manifest_dir / "AndroidManifest.xml").write_bytes(manifest)

    step("generating sources")
    clean_directory(build / "generated", build)
    run_generators(kit, build, runner)
    build_id = source_build_id(kit)
    layer, library = build_native(kit, build, tools, runner, options, build_id)
    loader_dex = build_loader(kit, build, tools, runner, package, build_id)

    build_info = json.dumps(
        {
            "modVersion": MOD_VERSION,
            "buildId": build_id,
            "package": PACKAGE_NAME,
            "versionCode": VERSION_CODE,
            "engineSha256": package.engine_sha256,
        },
        separators=(",", ":"),
    ).encode("utf-8") + b"\n"
    contents = {
        "manifest": manifest,
        "game_dex": game_dex,
        "loader_dex": loader_dex,
        "libraries": {
            MOD_LIBRARY: library.read_bytes(),
            LAYER_LIBRARY: layer.read_bytes(),
            OPENXR_LIBRARY: options.openxr_loader.read_bytes(),
        },
        "build_info": build_info,
        "licenses": {name: files[name].read_bytes() for name in LICENSE_FILES},
    }
    outputs, certificate = assemble(package, build, tools, runner, options.keystore, contents, options.reuse_build)
    check_output_manifest(package, build, tools, runner, options.trade)

    step("writing build-manifest.json")
    write_json(
        build / "build-manifest.json",
        {
            "formatVersion": 1,
            "modVersion": MOD_VERSION,
            "buildId": build_id,
            "package": PACKAGE_NAME,
            "versionCode": VERSION_CODE,
            "versionName": VERSION_NAME,
            "engineSha256": package.engine_sha256,
            "trade": options.trade,
            "gameDexCompatibility": package.game_dex_compatibility,
            "launcher": package.base_info.launcher,
            "sourceSignerSha256": package.signer,
            "outputSignerSha256": hashlib.sha256(certificate).hexdigest(),
            "outputSignerHashCode": apkzip.java_hash_code(certificate),
            "outputs": outputs,
            "external": package.external,
        },
    )
    remove_file(engine_path)
    say(f"APK output: {build / 'out'}")
    say(f"Build manifest: {build / 'build-manifest.json'}")
    return 0


def validation_record(package: GamePackage) -> dict:
    return {
        "formatVersion": 1,
        "modVersion": MOD_VERSION,
        "package": PACKAGE_NAME,
        "versionCode": VERSION_CODE,
        "versionName": VERSION_NAME,
        "input": package.kind,
        "apks": [{"file": apk.name, "split": apk.info.split, "size": apk.size} for apk in package.apks],
        "engineApk": package.engine.name,
        "engineSha256": package.engine_sha256,
        "sourceSignerSha256": package.signer,
        "launcher": package.base_info.launcher,
        "applicationClass": package.base_info.application_class,
        "gameDex": package.game_dex,
        "gameDexCompatibility": package.game_dex_compatibility,
        "loaderDex": package.loader_dex,
        "external": package.external,
    }


def parse_args(argv=None) -> Options:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-package", required=True, help="export folder, base.apk, .apk, .apks or .zip")
    parser.add_argument("--build-dir", required=True, help="disposable build directory")
    parser.add_argument("--sdk", required=True, help="Android SDK root")
    parser.add_argument("--java-home", required=True, help="JDK 21 root")
    parser.add_argument("--openxr-loader", help="Khronos OpenXR loader 1.1.43 arm64 libopenxr_loader.so")
    parser.add_argument("--vulkan-headers", help="Vulkan-Headers vulkan-sdk-1.4.357.0 root")
    parser.add_argument("--keystore", help="personal signing key; created when missing")
    parser.add_argument("--reuse-build", help="previous build whose unchanged signed data APKs may be reused after verification")
    parser.add_argument("--trade", type=int, choices=(-1, 0, 1), default=0,
                        help="com.oculus.trade_cpu_for_gpu_amount (default 0)")
    parser.add_argument("--validate-only", action="store_true", help="check the game package and stop")
    parser.add_argument("--jobs", type=int, default=0, help="parallel native compile jobs")
    args = parser.parse_args(argv)

    def path(value):
        return Path(value).expanduser().resolve() if value else None

    return Options(
        game_package=path(args.game_package),
        build_dir=path(args.build_dir),
        sdk=path(args.sdk),
        java_home=path(args.java_home),
        openxr_loader=path(args.openxr_loader),
        vulkan_headers=path(args.vulkan_headers),
        keystore=path(args.keystore),
        trade=args.trade,
        validate_only=args.validate_only,
        jobs=args.jobs if args.jobs > 0 else (os.cpu_count() or 4),
        reuse_build=path(args.reuse_build),
    )


def report_error(message: str, details: str = "") -> None:
    print(f"ERROR: {message}", file=sys.stderr, flush=True)
    for line in details.splitlines():
        print(f"  {line}", file=sys.stderr, flush=True)


def main(argv=None) -> int:
    options = parse_args(argv)
    try:
        return run(options)
    except KitError as error:
        report_error(error.message, error.details)
        return 1
    except KeyboardInterrupt:
        report_error("the build was interrupted")
        return 130
    except Exception as error:  # noqa: BLE001
        report_error(f"unexpected failure: {type(error).__name__}: {error}", traceback.format_exc())
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
