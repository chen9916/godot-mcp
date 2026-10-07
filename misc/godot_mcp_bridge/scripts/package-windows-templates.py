#!/usr/bin/env python3
# Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md).
# SPDX-License-Identifier: MIT
"""Create a deterministic Windows x86_64 TPZ from a verified portable bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import struct
import sys
import zipfile
from pathlib import Path, PurePosixPath

TEMPLATES = {
    "debug": "windows_debug_x86_64.exe",
    "release": "windows_release_x86_64.exe",
    "debug_console": "windows_debug_x86_64_console.exe",
    "release_console": "windows_release_x86_64_console.exe",
}
NOTICES = ("LICENSE.txt", "COPYRIGHT.txt")
RUNTIME_FILES = {
    "d3d12_core": "D3D12Core.x86_64.dll",
    "d3d12_layers": "d3d12SDKLayers.x86_64.dll",
    "license": "D3D12-LICENSE.txt",
}
BUFFER_SIZE = 1024 * 1024


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(BUFFER_SIZE), b""):
            digest.update(block)
    return digest.hexdigest()


def bundle_file(bundle: Path, name: str) -> Path:
    require(isinstance(name, str) and bool(name), "Manifest file paths must be nonempty strings.")
    relative = PurePosixPath(name)
    require(
        not relative.is_absolute()
        and "\\" not in name
        and ":" not in name
        and relative.as_posix() == name
        and all(part not in (".", "..") for part in relative.parts),
        f"Unsafe manifest path: {name}",
    )
    target = bundle.joinpath(*relative.parts)
    require(target.resolve().is_relative_to(bundle), f"Manifest path escapes bundle: {name}")
    require(target.is_file() and not target.is_symlink(), f"Missing regular bundle file: {name}")
    return target


def verify_pe(path: Path) -> None:
    with path.open("rb") as binary:
        header = binary.read(64)
        require(len(header) == 64 and header[:2] == b"MZ", f"Invalid Windows executable: {path}")
        binary.seek(struct.unpack_from("<I", header, 0x3C)[0])
        pe = binary.read(6)
        require(pe == b"PE\0\0\x64\x86", f"Expected Windows x86_64 executable: {path}")


def zip_entry(name: str) -> zipfile.ZipInfo:
    # Fixed metadata reproduces identical inputs with the same Python/zlib runtime.
    entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    entry.create_system = 3
    entry.external_attr = 0o100644 << 16
    entry.compress_type = zipfile.ZIP_DEFLATED
    # Python 3.9-3.12 use this field; the public compress_level arrived in 3.13.
    setattr(entry, "_compresslevel", 6)
    return entry


def build(bundle: Path, output: Path) -> str:
    bundle = bundle.resolve(strict=True)
    require(bundle.is_dir(), "--package must name a validated portable bundle directory.")
    manifest = json.loads((bundle / "RELEASE_MANIFEST.json").read_text(encoding="utf-8"))
    require(isinstance(manifest, dict), "Release manifest must be an object.")
    require(manifest.get("format_version") == 1, "Unsupported release manifest format.")
    require(
        manifest.get("platform") == "windows" and manifest.get("architecture") == "x86_64",
        "Expected Windows x86_64 bundle.",
    )
    require(manifest.get("git_dirty") is False, "Release manifest must record git_dirty=false.")
    revision = manifest.get("git_revision")
    require(
        isinstance(revision, str) and re.fullmatch(r"[0-9a-f]{40}", revision) is not None,
        "Missing source commit identity.",
    )
    metadata = manifest.get("export_templates")
    require(isinstance(metadata, dict), "Bundle must contain export_templates metadata.")
    version = metadata.get("version")
    require(
        isinstance(version, str)
        and re.fullmatch(r"\d+\.\d+(?:\.\d+)?\.[A-Za-z][A-Za-z0-9_-]*(?:\.[A-Za-z0-9][A-Za-z0-9_-]*)*", version)
        is not None,
        "Template version must be a safe Godot configuration such as major.minor.status.",
    )
    require(metadata.get("architecture") == "x86_64", "Template architecture must be x86_64.")
    require(metadata.get("editor_version") == manifest.get("editor_version"), "Template and editor versions differ.")
    editor_version = manifest.get("editor_version")
    build_identity = metadata.get("build")
    require(isinstance(editor_version, str) and isinstance(build_identity, str), "Missing editor build identity.")
    require(
        re.sub(r"\.[0-9a-f]{9}$", "", editor_version) == f"{version}.{build_identity}",
        "Editor version does not match template configuration.",
    )
    directory = f"editor_data/export_templates/{version}"
    require(metadata.get("directory") == directory, "Unexpected portable template directory.")
    expected_files = {key: f"{directory}/{name}" for key, name in TEMPLATES.items()}
    require(
        metadata.get("files") == expected_files, "Manifest must declare exactly the four matching Windows templates."
    )
    hashes = manifest.get("files_sha256")
    require(isinstance(hashes, dict), "Missing release file hashes.")
    inputs: dict[str, Path] = {}
    expected_hashes: dict[str, str] = {}
    selections = [
        *((expected_files[key], f"templates/{name}") for key, name in TEMPLATES.items()),
        *((name, f"templates/{name}") for name in NOTICES),
    ]
    runtime_files = metadata.get("runtime_files")
    if runtime_files is not None:
        expected_runtime = {key: f"{directory}/{name}" for key, name in RUNTIME_FILES.items()}
        require(
            runtime_files == expected_runtime,
            "D3D12 runtime metadata must declare both DLLs and their license together.",
        )
        selections.extend((expected_runtime[key], f"templates/{name}") for key, name in RUNTIME_FILES.items())
    for relative, archive_name in selections:
        source = bundle_file(bundle, relative)
        expected = hashes.get(relative)
        require(
            isinstance(expected, str) and re.fullmatch(r"[0-9a-f]{64}", expected) is not None,
            f"Missing SHA-256: {relative}",
        )
        require(sha256(source) == expected, f"Bundle hash mismatch: {relative}")
        if relative.endswith((".exe", ".dll")):
            verify_pe(source)
        inputs[archive_name] = source
        expected_hashes[archive_name] = expected
    version_file = f"{directory}/version.txt"
    if version_file in hashes:
        source = bundle_file(bundle, version_file)
        require(sha256(source) == hashes[version_file], "Bundle hash mismatch: version.txt")
        require(source.read_text(encoding="utf-8").strip() == version, "Bundled version.txt disagrees with manifest.")
    version_bytes = (version + "\n").encode("utf-8")
    expected_hashes["templates/version.txt"] = hashlib.sha256(version_bytes).hexdigest()

    engine = Path(__file__).resolve().parents[3]
    artifacts = (engine / "artifacts").resolve()
    require(artifacts.is_relative_to(engine), "Artifacts directory must remain inside this checkout.")
    require(not output.is_symlink(), "Refusing a symbolic-link output path.")
    output = output.resolve()
    require(
        output.is_relative_to(artifacts) and output != artifacts,
        "--output must be a new file inside this checkout's artifacts directory.",
    )
    require(output.suffix.lower() == ".tpz", "--output must have the .tpz extension.")
    checksum = output.with_suffix(output.suffix + ".sha256")
    require(not output.exists() and not output.is_symlink(), "Refusing to overwrite TPZ output.")
    require(not checksum.exists() and not checksum.is_symlink(), "Refusing to overwrite checksum output.")
    output.parent.mkdir(parents=True, exist_ok=True)
    created = False
    checksum_created = False
    try:
        with output.open("xb") as destination:
            created = True
            with zipfile.ZipFile(
                destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True
            ) as archive:
                for name in sorted(expected_hashes):
                    if name == "templates/version.txt":
                        archive.writestr(zip_entry(name), version_bytes)
                    else:
                        with (
                            inputs[name].open("rb") as source_stream,
                            archive.open(zip_entry(name), "w", force_zip64=True) as entry,
                        ):
                            shutil.copyfileobj(source_stream, entry, BUFFER_SIZE)
        with zipfile.ZipFile(output) as archive:
            require(archive.testzip() is None, "TPZ CRC verification failed.")
            require(sorted(archive.namelist()) == sorted(expected_hashes), "TPZ inventory verification failed.")
            for name, expected in expected_hashes.items():
                digest = hashlib.sha256()
                with archive.open(name) as entry:
                    for block in iter(lambda: entry.read(BUFFER_SIZE), b""):
                        digest.update(block)
                require(digest.hexdigest() == expected, f"TPZ hash mismatch: {name}")
        archive_digest = sha256(output)
        with checksum.open("x", encoding="utf-8", newline="\n") as destination:
            checksum_created = True
            destination.write(f"{archive_digest}  {output.name}\n")
        return archive_digest
    except BaseException:
        if checksum_created:
            checksum.unlink()
        if created:
            output.unlink()
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--package", type=Path, required=True, help="Validated portable bundle with clean release manifest."
    )
    parser.add_argument(
        "--output", type=Path, required=True, help="New .tpz file inside this checkout's artifacts directory."
    )
    args = parser.parse_args()
    try:
        digest = build(args.package, args.output)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Template packaging failed: {error}\n")
    print(f"Created {args.output.resolve()}\nSHA-256: {digest}\nChecksum: {args.output.resolve()}.sha256")
    return 0


if __name__ == "__main__":
    sys.exit(main())
