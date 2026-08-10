#!/usr/bin/env python3
"""Package and verify the exact MAME link inputs consumed by Profligacy."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import zipfile


MANIFEST_NAME = ".profligacy-sdk/manifest.json"
PLATFORMS = {
    "macos-arm64": {
        "bin": "build/osx_clang/bin/x64/Release",
        "archive_suffix": ".a",
        "driver_archive": "build/osx_clang/bin/x64/Release/mame_propmin/libmame_propmin.a",
        "objects": (
            "build/osx_clang/obj/x64/Release/src/mame/mame.o",
            "build/osx_clang/obj/x64/Release/generated/mame/propmin/drivlist.o",
            "build/osx_clang/obj/x64/Release/generated/version.o",
        ),
    },
    "windows-x64-clangcl": {
        # The ClangCL project directory is named vs2022-clang, but MAME keeps
        # the conventional vs2022 output directory for its libraries/objects.
        "bin": "build/vs2022/bin/x64/Release",
        "archive_suffix": ".lib",
        "driver_archive": "build/vs2022/bin/x64/Release/mame_propmin/mame_propmin.lib",
        "objects": (
            "build/vs2022/obj/x64/Release/propmin/mame.obj",
            "build/vs2022/obj/x64/Release/propmin/drivlist.obj",
            "build/vs2022/obj/x64/Release/propmin/version.obj",
        ),
    },
}


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"error: {message}")


def git_head(mame_dir: Path) -> str:
    result = subprocess.run(
        ["git", "-C", str(mame_dir), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_target(root: Path, relative: str) -> Path:
    rel = Path(relative)
    if rel.is_absolute() or ".." in rel.parts:
        fail(f"unsafe SDK path: {relative}")
    target = (root / rel).resolve()
    if os.path.commonpath((str(root.resolve()), str(target))) != str(root.resolve()):
        fail(f"SDK path escapes MAME tree: {relative}")
    return target


def collect_files(mame_dir: Path, platform: str) -> list[Path]:
    config = PLATFORMS[platform]
    bin_dir = mame_dir / config["bin"]
    if not bin_dir.is_dir():
        fail(f"MAME release library directory is missing: {bin_dir}")

    files = sorted(path for path in bin_dir.rglob(f"*{config['archive_suffix']}") if path.is_file())
    driver = mame_dir / config["driver_archive"]
    if driver not in files:
        fail(f"focused Prophecy driver archive is missing: {driver}")

    for relative in config["objects"]:
        path = mame_dir / relative
        if not path.is_file():
            fail(f"required generated object is missing: {path}")
        files.append(path)

    layout_dir = mame_dir / "build/generated/mame/layout"
    layouts = sorted(path for path in layout_dir.rglob("*") if path.is_file())
    if not layouts:
        fail(f"generated MAME layouts are missing: {layout_dir}")
    files.extend(layouts)

    stamp = bin_dir / ".prophecy_build_rev"
    if not stamp.is_file():
        fail(f"MAME revision stamp is missing: {stamp}")
    expected = git_head(mame_dir)
    if stamp.read_text(encoding="utf-8").strip() != expected:
        fail(f"MAME revision stamp does not match checkout {expected}: {stamp}")
    files.append(stamp)

    return sorted(set(files), key=lambda path: path.relative_to(mame_dir).as_posix())


def pack(args: argparse.Namespace) -> None:
    mame_dir = args.mame_dir.resolve()
    output = args.output.resolve()
    if output.exists():
        fail(f"refusing to overwrite SDK archive: {output}")

    files = collect_files(mame_dir, args.platform)
    entries = []
    for path in files:
        relative = path.relative_to(mame_dir).as_posix()
        entries.append({"path": relative, "size": path.stat().st_size, "sha256": sha256_file(path)})

    metadata = {}
    for item in args.metadata:
        if "=" not in item:
            fail(f"metadata must use key=value form: {item}")
        key, value = item.split("=", 1)
        if not key or key in metadata:
            fail(f"invalid or duplicate metadata key: {key}")
        metadata[key] = value

    manifest = {
        "schema": 1,
        "kind": "profligacy-mame-sdk",
        "platform": args.platform,
        "source_commit": git_head(mame_dir),
        "metadata": metadata,
        "files": entries,
    }
    manifest_bytes = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")

    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path in files:
            archive.write(path, path.relative_to(mame_dir).as_posix())
        archive.writestr(MANIFEST_NAME, manifest_bytes)

    print(
        json.dumps(
            {
                "archive": str(output),
                "archive_sha256": sha256_file(output),
                "files": len(files),
                "platform": args.platform,
                "source_commit": manifest["source_commit"],
            },
            sort_keys=True,
        )
    )


def extract(args: argparse.Namespace) -> None:
    mame_dir = args.mame_dir.resolve()
    archive_path = args.archive.resolve()
    with zipfile.ZipFile(archive_path, "r") as archive:
        try:
            manifest_bytes = archive.read(MANIFEST_NAME)
        except KeyError:
            fail(f"SDK archive has no {MANIFEST_NAME}")
        manifest = json.loads(manifest_bytes)

        if manifest.get("schema") != 1 or manifest.get("kind") != "profligacy-mame-sdk":
            fail("unsupported MAME SDK manifest")
        if manifest.get("platform") != args.platform:
            fail(f"SDK platform {manifest.get('platform')} does not match requested {args.platform}")
        head = git_head(mame_dir)
        if manifest.get("source_commit") != head:
            fail(f"SDK source {manifest.get('source_commit')} does not match MAME checkout {head}")

        declared = {entry["path"]: entry for entry in manifest.get("files", [])}
        actual = set(archive.namelist()) - {MANIFEST_NAME}
        if actual != set(declared):
            fail("SDK archive contents do not exactly match its manifest")

        for relative in sorted(declared):
            entry = declared[relative]
            data = archive.read(relative)
            if len(data) != entry["size"] or sha256_bytes(data) != entry["sha256"]:
                fail(f"SDK file failed size/hash verification: {relative}")
            target = safe_target(mame_dir, relative)
            if target.exists():
                fail(f"refusing to overwrite existing SDK file: {target}")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)

    print(
        json.dumps(
            {
                "archive": str(archive_path),
                "files": len(declared),
                "platform": args.platform,
                "source_commit": manifest["source_commit"],
                "verified": True,
            },
            sort_keys=True,
        )
    )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)

    pack_parser = commands.add_parser("pack", help="create a verified SDK ZIP")
    pack_parser.add_argument("--platform", choices=sorted(PLATFORMS), required=True)
    pack_parser.add_argument("--mame-dir", type=Path, required=True)
    pack_parser.add_argument("--output", type=Path, required=True)
    pack_parser.add_argument("--metadata", action="append", default=[])
    pack_parser.set_defaults(func=pack)

    extract_parser = commands.add_parser("extract", help="verify and restore an SDK ZIP")
    extract_parser.add_argument("--platform", choices=sorted(PLATFORMS), required=True)
    extract_parser.add_argument("--mame-dir", type=Path, required=True)
    extract_parser.add_argument("--archive", type=Path, required=True)
    extract_parser.set_defaults(func=extract)
    return result


def main() -> int:
    args = parser().parse_args()
    args.func(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
