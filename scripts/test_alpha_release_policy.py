#!/usr/bin/env python3
"""Ensure the small unnotarized alpha changes distribution policy only."""

from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]


class AlphaReleasePolicyTests(unittest.TestCase):
    def test_alpha_preserves_every_non_notarization_gate(self) -> None:
        broad = json.loads((ROOT / "release/v1_preflight.json").read_text())
        alpha = json.loads((ROOT / "release/v1_alpha_preflight.json").read_text())

        self.assertEqual("public-github-alpha", alpha["channel"])
        self.assertIn("native arm64", alpha["supported_target"])
        self.assertIn("notarization", alpha["distribution_warning"].lower())
        self.assertIn("ad-hoc", alpha["distribution_warning"].lower())
        self.assertIn("gatekeeper", alpha["distribution_warning"].lower())

        metadata = {"channel", "supported_target", "distribution_warning"}
        self.assertEqual(
            broad.keys(),
            alpha.keys() - metadata,
            "alpha may add distribution metadata but may not drop contract sections",
        )
        for key in broad.keys() - {"required_evidence"}:
            self.assertEqual(broad[key], alpha[key], f"alpha changed {key}")

        broad_evidence = set(broad["required_evidence"])
        alpha_evidence = set(alpha["required_evidence"])
        self.assertEqual({"notarization"}, broad_evidence - alpha_evidence)
        self.assertFalse(alpha_evidence - broad_evidence)
        self.assertIn("arm64_dynarec_interpreter_equivalence", alpha_evidence)
        self.assertIn("bundle_signature_dependencies", alpha_evidence)
        self.assertIn("pluginval_au", alpha_evidence)
        self.assertIn("pluginval_vst3", alpha_evidence)
        self.assertIn("auval", alpha_evidence)
        self.assertIn("daw_smoke", alpha_evidence)

    def test_release_workflow_uses_manifest_mame_revision_and_tree(self) -> None:
        alpha = json.loads((ROOT / "release/v1_alpha_preflight.json").read_text())
        workflow = (ROOT / ".github/workflows/alpha-prototype.yml").read_text()
        revision = re.search(r"(?m)^\s*MAME_SHA:\s*([0-9a-f]{40})\s*$", workflow)
        tree = re.search(r"(?m)^\s*MAME_TREE:\s*([0-9a-f]{40})\s*$", workflow)

        self.assertIsNotNone(revision, "release workflow has no pinned 40-hex MAME_SHA")
        self.assertIsNotNone(tree, "release workflow has no pinned 40-hex MAME_TREE")
        self.assertEqual(alpha["dependencies"]["extern/mame"], revision.group(1))
        self.assertEqual(alpha["dependency_trees"]["extern/mame"], tree.group(1))

        gitlink = subprocess.run(
            ["git", "ls-tree", "HEAD", "extern/mame"],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.split()
        self.assertGreaterEqual(len(gitlink), 3, "release commit has no extern/mame gitlink")
        self.assertEqual("160000", gitlink[0], "extern/mame is not a gitlink")
        self.assertEqual(alpha["dependencies"]["extern/mame"], gitlink[2])

    def test_windows_vst3_editor_gates_allow_cold_webview_startup(self) -> None:
        release_workflow = (ROOT / ".github/workflows/alpha-prototype.yml").read_text()
        product_workflow = (ROOT / ".github/workflows/product-shell.yml").read_text()

        # A real-engine release runner can cold-start WebView2 after a large
        # native build. Keep both packaged-VST3 gates explicit and consistent.
        self.assertIn("--editor-timeout 30", release_workflow)
        self.assertIn("--editor-timeout 30", product_workflow)


if __name__ == "__main__":
    unittest.main()
