#!/usr/bin/env python3

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mame_sdk


class MameSdkTest(unittest.TestCase):
    def create_repo(self, root: Path) -> tuple[Path, str]:
        mame = root / "mame"
        mame.mkdir()
        subprocess.run(["git", "init", "-b", "main", str(mame)], check=True, capture_output=True)
        subprocess.run(["git", "-C", str(mame), "config", "user.name", "SDK Test"], check=True)
        subprocess.run(["git", "-C", str(mame), "config", "user.email", "sdk-test@example.invalid"], check=True)
        (mame / "README.md").write_text("fixture\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(mame), "add", "README.md"], check=True)
        subprocess.run(["git", "-C", str(mame), "commit", "-m", "fixture"], check=True, capture_output=True)
        return mame, mame_sdk.git_head(mame)

    def create_build(self, mame: Path, platform: str, revision: str) -> set[str]:
        config = mame_sdk.PLATFORMS[platform]
        paths = {
            config["driver_archive"],
            *config["objects"],
            "build/generated/mame/layout/korgprop.lh",
            f"{config['bin']}/extra{config['archive_suffix']}",
            f"{config['bin']}/.prophecy_build_rev",
        }
        for relative in paths:
            path = mame / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            content = revision if path.name == ".prophecy_build_rev" else f"fixture:{relative}"
            path.write_text(content, encoding="utf-8")
        return paths

    def round_trip(self, platform: str) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            mame, revision = self.create_repo(root)
            expected = self.create_build(mame, platform, revision)
            archive = root / "sdk.zip"

            mame_sdk.pack(
                argparse.Namespace(
                    platform=platform,
                    mame_dir=mame,
                    output=archive,
                    metadata=["test=true"],
                )
            )
            with zipfile.ZipFile(archive) as packed:
                manifest = json.loads(packed.read(mame_sdk.MANIFEST_NAME))
            self.assertEqual(revision, manifest["source_commit"])
            self.assertEqual(platform, manifest["platform"])
            self.assertEqual(expected, {entry["path"] for entry in manifest["files"]})

            shutil.rmtree(mame / "build")
            mame_sdk.extract(
                argparse.Namespace(platform=platform, mame_dir=mame, archive=archive)
            )
            for relative in expected:
                self.assertTrue((mame / relative).is_file(), relative)

    def test_macos_round_trip(self) -> None:
        self.round_trip("macos-arm64")

    def test_windows_round_trip(self) -> None:
        self.round_trip("windows-x64-clangcl")

    def test_extract_rejects_different_source_revision(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            mame, revision = self.create_repo(root)
            self.create_build(mame, "macos-arm64", revision)
            archive = root / "sdk.zip"
            mame_sdk.pack(
                argparse.Namespace(
                    platform="macos-arm64",
                    mame_dir=mame,
                    output=archive,
                    metadata=[],
                )
            )
            shutil.rmtree(mame / "build")
            (mame / "SECOND.md").write_text("different revision\n", encoding="utf-8")
            subprocess.run(["git", "-C", str(mame), "add", "SECOND.md"], check=True)
            subprocess.run(["git", "-C", str(mame), "commit", "-m", "second"], check=True, capture_output=True)
            with self.assertRaisesRegex(SystemExit, "does not match MAME checkout"):
                mame_sdk.extract(
                    argparse.Namespace(
                        platform="macos-arm64",
                        mame_dir=mame,
                        archive=archive,
                    )
                )


if __name__ == "__main__":
    unittest.main()
