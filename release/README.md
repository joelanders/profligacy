# Profligacy release preflight

Two fail-closed profiles are tracked:

- `v1_preflight.json` is the eventual broadly distributed release. It requires
  Developer ID signing and notarization.
- `v1_alpha_preflight.json` is the native-Apple-Silicon public GitHub alpha. It keeps
  every source, correctness, performance, host, archive, license, SBOM, and
  ad-hoc bundle-signature gate, but explicitly does not claim Developer ID
  identity or Apple notarization. Its release page and package instructions must
  disclose the resulting Gatekeeper friction rather than implying a normal signed
  macOS installation.

The alpha profile is not a shortcut around product testing. Its unit test
requires it to differ from the broad-release profile only by the notarization
receipt and the declared distribution channel.

Run the fail-closed integration check with an initialized, exact dependency
checkout and a configured test build:

```sh
python3 scripts/release_preflight.py --build-dir /path/to/configured-build
```

Use `--manifest release/v1_alpha_preflight.json` for the unnotarized alpha.

Before running an expensive correctness gate, inspect the resumable checkpoint:

```sh
python3 scripts/correctness_checkpoint.py
```

It reports each correctness receipt as `PASS`, `MISSING`, `STALE`, or `FAIL`
against the current committed tree and exact dependency checkouts. A dirty
tracked worktree or mismatched submodule blocks the checkpoint. Run only gates
that are not already `PASS`; a later session can resume from the same list.
`scripts/run_gates.sh` remains a convenient development smoke battery, but its
non-resumable aggregate result is not the final release-evidence workflow.

The named ARM64 interpreter/dynarec receipt comes from the public MAME gate,
not from a prose assertion:

```sh
python3 extern/mame/scripts/korgprophecy_pf4_equivalence.py \
  --require-arm64 \
  --binary extern/mame/propmin \
  --rompath /path/to/user-supplied/roms \
  --output .release-evidence/arm64_dynarec_interpreter_equivalence/raw
python3 scripts/promote_arm64_dynarec_receipt.py \
  --raw .release-evidence/arm64_dynarec_interpreter_equivalence/raw/receipt.json
```

Promotion requires all five scenario families, every byte comparison, explicit
native compile/run evidence, interpreter non-execution of pooled code, the
forced-midframe hit, exact retained-artifact hashing, and a clean current source
binding. A partial or fallback-only run cannot become a release PASS.

The command passes only when the worktree is clean, the accepted development
branch has no missing shipping patch, dependency gitlinks and checkouts match
the release manifest, mandatory CTest tests are present, forbidden/private
assets are absent, and every required gate has a current `PASS` receipt.

Receipts live in `.release-evidence/<gate>.json` and are deliberately ignored
by Git.  Every receipt has this form:

```json
{
  "schema": 1,
  "gate": "gate_name_from_v1_preflight.json",
  "status": "PASS",
  "binding": {
    "plugin_tree": "40-hex tree",
    "dependencies": {
      "extern/JUCE": "40-hex commit",
      "extern/mame": "40-hex commit"
    }
  },
  "artifacts": [
    {"path": "gate_name/raw-result.json", "sha256": "64-hex digest"}
  ]
}
```

Artifact paths are relative to `.release-evidence/`; artifacts must exist and
match their recorded SHA-256. `FAIL`, `INCONCLUSIVE`, a missing receipt, a
missing mandatory test, a changed source tree, or a changed dependency all
fail the preflight.  A human assertion without a retained artifact is not a
passing receipt.

The binding deliberately uses the Git tree rather than the commit ID. This
allows the final history-squashed public root to reuse evidence only when its
tracked contents are byte-for-byte identical; a source or metadata-file change
still produces a different tree and invalidates every receipt.

Consequently, finish all tracked source, test-policy, version, and release-text
changes before the final correctness run. Packaging must consume the frozen
tree without editing it. This makes a completed correctness receipt final
instead of forcing an avoidable rerun for last-minute prose.

Use `--phase fresh-root` only for the final sanitized public repository, where
the private integration ref is intentionally unavailable.  It retains every
other check.  The source archive/provenance receipt must contain the tracked
tree comparison for that fresh-root transition.

The fresh-root phase additionally requires exactly one commit authored and
committed by `joe@joelanders.net`, no refs or unreachable objects outside that
root, and no tracked internal `HANDOVER.md` or `notes/` tree.  This is a
physical-history check: deleting a private file or proprietary firmware dump in
a later commit is not sufficient because the old blob would remain publishable.
The reviewed HD44780 datasheet reconstruction compiled into the public source is
intentional and is guarded by `editor_asset_provenance`.

## Updating an existing public repository

A clean integration worktree is a patch source, not a publication checkout.
Create fresh clones from the public GitHub repositories and never add a research
repository as a remote there.

Before transferring anything, enumerate the complete integration delta from the
actual public base with `git diff --name-status PUBLIC_BASE..INTEGRATION_TARGET`.
Every path must be explicitly classified as public or excluded; an unclassified
path fails the release. Do not replace this complete inventory with a hand-made
source-file list. Apply the full reviewed MAME delta, then require the resulting
public MAME root tree from `git rev-parse HEAD^{tree}` to equal the staged
integration MAME tree from `git write-tree`. For the product repository, compare
the Git object IDs of every declared shipping path and separately confirm that
`notes/` and `HANDOVER.md` are absent.

The release workflow pins both the public MAME commit and its complete root tree.
Merge the MAME publication PR without squashing away the pinned commit, and
verify that the pin is an ancestor of the public MAME branch before merging,
tagging, or packaging the product. Only then may the product PR and release
artifact workflow proceed.
