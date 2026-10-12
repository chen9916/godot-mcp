#!/usr/bin/env python3
# Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md).
# SPDX-License-Identifier: MIT
"""Create a checked, deterministic portable ZIP only after the release gate."""

import argparse
import hashlib
import json
import shutil
import zipfile
from pathlib import Path, PurePosixPath


def sha256(filename):
    digest = hashlib.sha256()
    with filename.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def build(bundle, output):
    bundle = bundle.resolve(strict=True)
    artifacts = Path(__file__).resolve().parents[3] / "artifacts"
    output = output.resolve()
    if not bundle.is_relative_to(artifacts) or not output.is_relative_to(artifacts):
        raise ValueError("Bundle and new ZIP must be inside this checkout's artifacts directory.")
    if output.suffix.lower() != ".zip":
        raise ValueError("Output must have the .zip extension.")
    checksum = output.with_suffix(".zip.sha256")
    if output.exists() or checksum.exists():
        raise ValueError("Refusing to overwrite an existing archive or checksum.")
    manifest_file = bundle / "RELEASE_MANIFEST.json"
    manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
    report = json.loads(Path(str(bundle) + ".verification.json").read_text(encoding="utf-8"))
    if manifest.get("git_dirty") is not False or report.get("status") != "verified":
        raise ValueError("A clean manifest and passing release verification report are required.")
    required_checks = {
        "bridge-default",
        "native-units",
        "package-smoke-source",
        "windows-export",
        "animation-workflow",
        "native-protocol",
        "release-workflow",
        "setup-workflow",
        "package-final-inventory",
    }
    checks = report.get("checks", [])
    if not required_checks.issubset({check.get("name") for check in checks}) or not all(
        check.get("pass") is True for check in checks
    ):
        raise ValueError("The complete release gate must pass before archiving.")
    if report.get("git_revision") != manifest.get("git_revision") or report.get("manifest_sha256") != sha256(
        manifest_file
    ):
        raise ValueError("Release report does not certify this manifest.")
    expected = dict(manifest["files_sha256"])
    expected["RELEASE_MANIFEST.json"] = sha256(manifest_file)
    actual = []
    for entry in bundle.rglob("*"):
        if entry.is_symlink():
            raise ValueError("Symbolic links are not allowed in the bundle.")
        if entry.is_file():
            actual.append(entry.relative_to(bundle).as_posix())
    if sorted(actual) != sorted(expected):
        raise ValueError("Bundle inventory changed after verification.")
    for name, digest in expected.items():
        relative = PurePosixPath(name)
        if relative.is_absolute() or "\\" in name or ":" in name or any(part in (".", "..") for part in relative.parts):
            raise ValueError("Unsafe manifest path: " + name)
        if sha256(bundle / name) != digest:
            raise ValueError("Bundle hash mismatch: " + name)
    created = False
    try:
        with output.open("xb") as destination:
            created = True
            with zipfile.ZipFile(
                destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True
            ) as archive:
                for name in sorted(expected):
                    entry = zipfile.ZipInfo(f"{bundle.name}/{name}", date_time=(1980, 1, 1, 0, 0, 0))
                    entry.create_system = 3
                    entry.external_attr = 0o100644 << 16
                    entry.compress_type = zipfile.ZIP_DEFLATED
                    entry._compresslevel = 6
                    with (bundle / name).open("rb") as source, archive.open(entry, "w", force_zip64=True) as target:
                        shutil.copyfileobj(source, target, 1024 * 1024)
        with zipfile.ZipFile(output) as archive:
            if archive.testzip() is not None:
                raise ValueError("ZIP CRC verification failed.")
            if sorted(archive.namelist()) != sorted(f"{bundle.name}/{name}" for name in expected):
                raise ValueError("ZIP inventory verification failed.")
            for name, digest in expected.items():
                hashed = hashlib.sha256()
                with archive.open(f"{bundle.name}/{name}") as source:
                    for block in iter(lambda: source.read(1024 * 1024), b""):
                        hashed.update(block)
                if hashed.hexdigest() != digest:
                    raise ValueError("ZIP content mismatch: " + name)
        digest = sha256(output)
        with checksum.open("x", encoding="utf-8", newline="\n") as target:
            target.write(f"{digest}  {output.name}\n")
        print(
            json.dumps(
                {
                    "archive": str(output),
                    "bytes": output.stat().st_size,
                    "sha256": digest,
                    "files": len(expected),
                    "crc_and_inventory_verified": True,
                },
                indent=2,
            )
        )
    except BaseException:
        if created:
            output.unlink()
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    build(args.package, args.output)
