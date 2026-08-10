#!/usr/bin/env python3
"""Turn an Apple ld map into an object/source/license evidence ledger.

This is an evidence tool, not legal advice.  It inventories objects that the
linker loaded and records mechanically discoverable per-source or package
license evidence.  Review flags are deliberately conservative.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any


OBJECT_RE = re.compile(r"^\[\s*(\d+)\]\s+(.+)$")
ARCHIVE_RE = re.compile(r"^(.*?)\[(\d+)\]\((.+)\)$")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm", ".s", ".asm"}
SYNTHETIC_INPUTS = {
    "linker synthesized", "objc-stubs-file", "dedup-file", "objc-file",
    "tlv-file", "inits-file",
}
HEADER_LICENSE_RE = re.compile(r"(?:SPDX-License-Identifier:|license:)\s*([^\s*]+)")
PROJECT_LICENSE = "AGPL-3.0-only"
JUCE_SELECTED_LICENSE = "AGPL-3.0-only"
JUCE_DUAL_LICENSE = "AGPL-3.0-only OR LicenseRef-JUCE-Commercial"
MAME_WHOLE_WORK_LICENSE = "GPL-2.0-or-later"

THIRDPARTY_LICENSES: dict[str, tuple[str, str]] = {
    "asmjit": ("Zlib", "3rdparty/asmjit/LICENSE.md"),
    "bgfx": ("BSD-2-Clause", "3rdparty/bgfx/LICENSE"),
    "bimg": ("BSD-2-Clause", "3rdparty/bimg/LICENSE"),
    "bx": ("BSD-2-Clause", "3rdparty/bx/LICENSE"),
    "expat": ("MIT", "3rdparty/expat/COPYING"),
    "libjpeg": ("IJG", "3rdparty/libjpeg/README"),
    "linenoise": ("BSD-2-Clause", "3rdparty/linenoise/linenoise.c"),
    "lsqlite3": ("MIT", "3rdparty/lsqlite3/lsqlite3.c"),
    "lua": ("MIT", "3rdparty/lua/doc/readme.html"),
    "lua-linenoise": ("BSD-2-Clause", "3rdparty/lua-linenoise/linenoise.c"),
    "lua-zlib": ("MIT", "3rdparty/lua-zlib/README"),
    "luafilesystem": ("MIT", "3rdparty/luafilesystem/LICENSE"),
    "lzma": ("LicenseRef-LZMA-SDK-Public-Domain", "3rdparty/lzma/C/7zAlloc.c"),
    "softfloat3": ("BSD-3-Clause", "3rdparty/softfloat3/COPYING.txt"),
    "sqlite3": ("blessing", "3rdparty/sqlite3/sqlite3.c"),
    "utf8proc": ("MIT", "3rdparty/utf8proc/LICENSE.md"),
    "zlib": ("Zlib", "3rdparty/zlib/LICENSE"),
    "zstd": ("BSD-3-Clause", "3rdparty/zstd/LICENSE"),
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def normalize_license(value: str) -> str:
    aliases = {
        "GPL-2.0+": "GPL-2.0-or-later",
        "GPL2+": "GPL-2.0-or-later",
        "LGPL-2.1+": "LGPL-2.1-or-later",
        "BSD-3-clause": "BSD-3-Clause",
        "AGPLv3/Commercial": "AGPL-3.0-only OR LicenseRef-JUCE-Commercial",
    }
    return aliases.get(value, value)


def review_flags(expression: str) -> list[str]:
    flags: list[str] = []
    if expression == "UNKNOWN":
        flags.append("unknown-license")
    if "GPL-2.0-only" in expression:
        flags.append("gpl-2.0-only")
    if "LGPL" in expression:
        flags.append("lgpl")
    if "Proprietary" in expression or "Commercial" in expression:
        flags.append("proprietary-alternative")
    return flags


def parse_link_map(path: Path) -> tuple[str, list[tuple[int, str]], list[tuple[int, str]]]:
    arch = ""
    objects: list[tuple[int, str]] = []
    synthetic: list[tuple[int, str]] = []
    in_objects = False
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("# Arch:"):
            arch = line.split(":", 1)[1].strip()
        elif line == "# Object files:":
            in_objects = True
        elif line == "# Sections:":
            break
        elif in_objects:
            match = OBJECT_RE.match(line)
            if not match:
                continue
            item = (int(match.group(1)), match.group(2))
            (synthetic if item[1] in SYNTHETIC_INPUTS else objects).append(item)
    if not arch or not objects:
        raise ValueError("link map lacks an architecture or real object-file table")
    return arch, objects, synthetic


def collapse_dependency_file(path: Path) -> tuple[str, list[str]]:
    text = path.read_text(encoding="utf-8", errors="replace").replace("\\\n", " ")
    target, separator, dependencies = text.partition(":")
    if not separator:
        return "", []
    return target.strip(), dependencies.split()


def source_from_dependency(path: Path, relative_base: Path) -> Path | None:
    _, dependencies = collapse_dependency_file(path)
    for value in dependencies:
        candidate = Path(value)
        if candidate.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        return candidate.resolve() if candidate.is_absolute() else (relative_base / candidate).resolve()
    return None


def cmake_dependency_index(build_root: Path) -> dict[Path, Path]:
    result: dict[Path, Path] = {}
    for dependency in build_root.rglob("*.o.d"):
        source = source_from_dependency(dependency, build_root)
        if source is not None:
            result[Path(str(dependency)[:-2]).resolve()] = source
    return result


def parse_release_archive_objects(makefile: Path, mame_root: Path) -> tuple[str, list[Path]]:
    lines = makefile.read_text(encoding="utf-8", errors="replace").splitlines()
    make_root = makefile.parent
    start = next(
        index for index, line in enumerate(lines)
        if "OBJDIR" in line and "osx_clang/obj/x64/Release" in line
    )
    objdir = (make_root / lines[start].split("=", 1)[1].strip()).resolve()
    target = next(
        line.rsplit("/", 1)[-1].strip()
        for line in lines[start:]
        if "override TARGET" in line and "=" in line
    )
    objects: list[Path] = []
    in_objects = False
    for line in lines[start:]:
        if line.startswith("  OBJECTS :="):
            in_objects = True
            continue
        if not in_objects:
            continue
        if not line.startswith("\t"):
            break
        value = line.strip().rstrip("\\").strip()
        if value:
            objects.append(Path(value.replace("$(OBJDIR)", str(objdir))).resolve())
    if not objects:
        raise ValueError(f"no release objects parsed from {makefile}")
    if not objdir.is_relative_to(mame_root.resolve()):
        raise ValueError(f"unexpected object directory outside MAME root: {objdir}")
    return target, objects


def mame_archive_index(mame_root: Path) -> dict[str, list[Path]]:
    make_root = mame_root / "build/projects/sdl/mamepropmin/gmake-osx-clang"
    result: dict[str, list[Path]] = {}
    for makefile in sorted(make_root.glob("*.make")):
        target, objects = parse_release_archive_objects(makefile, mame_root)
        result[target] = objects
    return result


def sanitize(path: Path, roots: list[tuple[str, Path]]) -> str:
    resolved = path.resolve()
    for label, root in roots:
        try:
            return f"{label}/{resolved.relative_to(root.resolve())}"
        except ValueError:
            pass
    return str(resolved)


def header_license(source: Path) -> str | None:
    if not source.is_file():
        return None
    with source.open("r", encoding="utf-8", errors="replace") as handle:
        head = "".join(handle.readline() for _ in range(80))
    match = HEADER_LICENSE_RE.search(head)
    return normalize_license(match.group(1)) if match else None


def license_evidence(
    source: Path | None,
    plugin_root: Path,
    juce_root: Path,
    mame_root: Path,
    sdl_license: Path,
) -> tuple[str, Path | None, str]:
    if source is None:
        return "UNKNOWN", None, "no-source-resolution"
    if source.suffix == ".tbd" and "SDKs" in source.parts:
        return (
            "LicenseRef-Apple-SDK-System-Library",
            source,
            "Apple-SDK-dynamic-link-input-not-embedded-object",
        )
    if source.name == "NotoSans-Bold.ttf":
        return (
            "OFL-1.1",
            source.parent / "NotoSans-OFL-1.1.txt",
            "font-package-license",
        )
    try:
        source.relative_to(juce_root)
        is_juce_source = True
    except ValueError:
        is_juce_source = False
    tagged = header_license(source)
    if tagged:
        if is_juce_source and tagged == JUCE_DUAL_LICENSE:
            return JUCE_SELECTED_LICENSE, source, "source-header-AGPL-alternative-selected"
        return tagged, source, "source-header"
    if source.suffix.lower() in {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}:
        for suffix in (".h", ".hpp"):
            sibling = source.with_suffix(suffix)
            sibling_tag = header_license(sibling)
            if sibling_tag:
                if is_juce_source and sibling_tag == JUCE_DUAL_LICENSE:
                    return JUCE_SELECTED_LICENSE, sibling, "sibling-header-AGPL-alternative-selected"
                return sibling_tag, sibling, "sibling-header"
    if is_juce_source:
        return (
            JUCE_SELECTED_LICENSE,
            juce_root / "LICENSE.md",
            "JUCE-package-license-AGPL-alternative-selected",
        )
    try:
        relative_plugin = source.relative_to(plugin_root)
    except ValueError:
        relative_plugin = None
    if relative_plugin is not None:
        if relative_plugin.parts[:2] == ("extern", "JUCE"):
            return (
                JUCE_SELECTED_LICENSE,
                plugin_root / "extern/JUCE/LICENSE.md",
                "JUCE-package-license-AGPL-alternative-selected",
            )
        if "juce_binarydata" in source.parts:
            return "UNKNOWN", None, "generated-binary-data-composite"
        return PROJECT_LICENSE, plugin_root / "LICENSE", "project-fallback"
    try:
        relative_mame = source.relative_to(mame_root)
    except ValueError:
        relative_mame = None
    if relative_mame is not None:
        if relative_mame.parts and relative_mame.parts[0] == "3rdparty" and len(relative_mame.parts) > 1:
            package = relative_mame.parts[1]
            known = THIRDPARTY_LICENSES.get(package)
            if known:
                expression, evidence = known
                return expression, mame_root / evidence, "third-party-package-license"
            return "UNKNOWN", None, f"unclassified-third-party-package:{package}"
        return (
            MAME_WHOLE_WORK_LICENSE,
            mame_root / "README.md",
            "MAME-whole-work-GPL-2.0-or-later-fallback",
        )
    if source == sdl_license:
        return "Zlib", sdl_license, "SDL-package-license"
    return "UNKNOWN", None, "source-outside-classified-roots"


def resolve_archive_member(
    archive_name: str,
    member_ordinal: int,
    member_name: str,
    build_index: dict[Path, Path],
    mame_archives: dict[str, list[Path]],
    mame_make_root: Path,
) -> tuple[list[Path], str]:
    if archive_name in mame_archives:
        index = member_ordinal - 2  # Darwin ar slot 1 is the symbol table.
        objects = mame_archives[archive_name]
        if index < 0 or index >= len(objects):
            return [], "MAME-archive-ordinal-out-of-range"
        object_path = objects[index]
        if object_path.name != member_name:
            return [], f"MAME-archive-member-mismatch:{object_path.name}"
        dependency = object_path.with_suffix(".d")
        source = source_from_dependency(
            dependency,
            mame_make_root,
        ) if dependency.is_file() else None
        return ([source] if source else []), "MAME-generated-makefile-ordinal"

    candidates = [
        source for object_path, source in build_index.items()
        if object_path.name == member_name
    ]
    if archive_name == "libProfligacy_SharedCode.a":
        candidates = [source for object_path, source in build_index.items()
                      if object_path.name == member_name and "ProphecyPlugin.dir" in str(object_path)]
    elif archive_name == "libprophecy_engine.a":
        candidates = [source for object_path, source in build_index.items()
                      if object_path.name == member_name and "prophecy_engine.dir" in str(object_path)]
    elif archive_name == "libProphecyData.a":
        candidates = [source for object_path, source in build_index.items()
                      if object_path.name == member_name and "ProphecyData.dir" in str(object_path)]
    unique = sorted(set(candidates))
    return unique, "cmake-dependency" if len(unique) == 1 else "cmake-dependency-ambiguous"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--link-map", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--plugin-root", type=Path, required=True)
    parser.add_argument("--mame-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--sdl-archive", type=Path, required=True)
    parser.add_argument("--sdl-license", type=Path, required=True)
    parser.add_argument("--plugin-revision", required=True)
    parser.add_argument("--mame-revision", required=True)
    parser.add_argument("--juce-revision", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fail-on-review-flags", action="store_true")
    args = parser.parse_args()

    paths = (args.link_map, args.binary, args.plugin_root, args.mame_root,
             args.build_root, args.sdl_archive, args.sdl_license)
    if any(not path.exists() for path in paths):
        parser.error("every input path must exist")
    arch, inputs, synthetic = parse_link_map(args.link_map)
    if arch != "arm64":
        parser.error(f"expected arm64 link map, got {arch!r}")

    plugin_root = args.plugin_root.resolve()
    juce_root = (plugin_root / "extern/JUCE").resolve()
    mame_root = args.mame_root.resolve()
    build_root = args.build_root.resolve()
    sdl_archive = args.sdl_archive.resolve()
    sdl_license = args.sdl_license.resolve()
    roots = [("plugin", plugin_root), ("juce", juce_root), ("mame", mame_root), ("build", build_root),
             ("sdl", sdl_archive.parent)]
    build_index = cmake_dependency_index(build_root)
    mame_archives = mame_archive_index(mame_root)
    mame_make_root = mame_root / "build/projects/sdl/mamepropmin/gmake-osx-clang"
    archive_hashes: dict[str, str] = {}
    records: list[dict[str, Any]] = []

    for object_id, link_input in inputs:
        archive_match = ARCHIVE_RE.match(link_input)
        source_paths: list[Path] = []
        resolution = "direct-object-dependency"
        archive_path: Path | None = None
        archive_ordinal: int | None = None
        member: str | None = None
        if archive_match:
            raw_archive, raw_ordinal, member = archive_match.groups()
            archive_ordinal = int(raw_ordinal)
            archive_path = Path(raw_archive)
            if not archive_path.is_absolute():
                archive_path = build_root / archive_path
            archive_path = archive_path.resolve()
            if archive_path == sdl_archive:
                source_paths = [sdl_license]
                resolution = "SDL-package-source-unavailable"
            elif archive_path.name == "libProphecyData.a":
                binary_data_sources = [
                    plugin_root / "src/editor/index.html",
                    plugin_root / "src/editor/deep_editor_manifest.js",
                    plugin_root / "src/editor/assets/NotoSans-Bold.ttf",
                ]
                source_index = archive_ordinal - 2
                expected_member = f"BinaryData{source_index + 1}.cpp.o"
                if 0 <= source_index < len(binary_data_sources) and member == expected_member:
                    source_paths = [binary_data_sources[source_index]]
                    resolution = "CMake-binary-data-source-order"
                else:
                    resolution = "CMake-binary-data-member-mismatch"
            else:
                source_paths, resolution = resolve_archive_member(
                    archive_path.name, archive_ordinal, member, build_index,
                    mame_archives, mame_make_root,
                )
            archive_key = sanitize(archive_path, roots)
            archive_hashes.setdefault(archive_key, sha256(archive_path))
        else:
            object_path = Path(link_input)
            if not object_path.is_absolute():
                object_path = (build_root / object_path).resolve()
            object_path = object_path.resolve()
            source = build_index.get(object_path)
            if object_path.suffix == ".tbd" and "SDKs" in object_path.parts:
                source = object_path
                resolution = "Apple-SDK-dynamic-link-input"
            if source is None and object_path.suffix == ".o":
                dependency = object_path.with_suffix(".d")
                if dependency.is_file():
                    source = source_from_dependency(
                        dependency,
                        mame_root / "build/projects/sdl/mamepropmin/gmake-osx-clang",
                    )
            source_paths = [source] if source else []
            if not source_paths:
                resolution = "direct-object-source-unresolved"

        evidences = [license_evidence(source, plugin_root, juce_root, mame_root, sdl_license)
                     for source in source_paths]
        expressions = sorted({evidence[0] for evidence in evidences}) or ["UNKNOWN"]
        expression = " AND ".join(expressions)
        flags = review_flags(expression)
        records.append({
            "object_id": object_id,
            "link_input": link_input,
            "archive": sanitize(archive_path, roots) if archive_path else None,
            "archive_member_ordinal": archive_ordinal,
            "member": member,
            "source_resolution": resolution,
            "sources": [
                {
                    "path": sanitize(source, roots),
                    "sha256": sha256(source) if source.is_file() else None,
                }
                for source in source_paths
            ],
            "license_expression": expression,
            "license_evidence": [
                {
                    "expression": license_expression,
                    "method": method,
                    "path": sanitize(evidence_path, roots) if evidence_path else None,
                    "sha256": sha256(evidence_path) if evidence_path and evidence_path.is_file() else None,
                }
                for license_expression, evidence_path, method in evidences
            ],
            "review_flags": flags,
        })

    flag_counts = Counter(flag for record in records for flag in record["review_flags"])
    license_counts = Counter(record["license_expression"] for record in records)
    resolution_counts = Counter(record["source_resolution"] for record in records)
    receipt = {
        "schema": 1,
        "tool": {
            "name": "generate_au_link_license_ledger.py",
            "sha256": sha256(Path(__file__)),
        },
        "scope": "exact Apple ld object table for arm64 Profligacy AU",
        "legal_conclusion": None,
        "source_revisions": {
            "plugin_base": args.plugin_revision,
            "mame_build_input": args.mame_revision,
            "juce": args.juce_revision,
        },
        "arch": arch,
        "binary": {"path": "AU/Profligacy.component/Contents/MacOS/Profligacy", "sha256": sha256(args.binary)},
        "link_map": {"sha256": sha256(args.link_map), "bytes": args.link_map.stat().st_size},
        "object_counts": {"real": len(records), "synthetic_excluded": len(synthetic)},
        "synthetic_excluded": [{"object_id": number, "name": name} for number, name in synthetic],
        "archives": archive_hashes,
        "summary": {
            "licenses": dict(sorted(license_counts.items())),
            "source_resolution": dict(sorted(resolution_counts.items())),
            "review_flags": dict(sorted(flag_counts.items())),
            "records_with_review_flags": sum(bool(record["review_flags"]) for record in records),
        },
        "objects": records,
        "passed_review_policy": not flag_counts,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        f"objects={len(records)} synthetic={len(synthetic)} "
        f"flagged={receipt['summary']['records_with_review_flags']} "
        f"receipt={args.output}"
    )
    for flag, count in sorted(flag_counts.items()):
        print(f"FLAG {flag}: {count}")
    return 1 if args.fail_on_review_flags and flag_counts else 0


if __name__ == "__main__":
    sys.dont_write_bytecode = True
    raise SystemExit(main())
