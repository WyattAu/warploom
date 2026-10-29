# Loop 21 — shared C++ CI gates (estate handoff, first of the "all" series)

## Sources

- Vendor documentation: GitHub Actions, "Reusing workflows"
  (`docs.github.com/en/actions/how-tos/reuse-automations/reuse-workflows`) —
  labelled as vendor docs, not evidence. The claim borrowed: a workflow with
  `on: workflow_call` is callable from another repo via
  `uses: OWNER/REPO/.github/workflows/FILE@REF`, and from the same repo via
  `uses: ./.github/workflows/FILE`; caller and callee share no namespace
  except declared `inputs`/`secrets`; top-level `on: [push, pull_request]`
  and `concurrency` stay with the caller.
- Estate sources (read raw, 2026-10-29): `engineering-standards` gate-matrix
  page (rust-kit.yml matrix: build/test/clippy/fmt/vet/semver/criterion/loom/
  miri/wasm tiers A–C; "every kit repository runs the shared matrix via one
  reusable-workflow call") and
  `go-kit.yml` (1769 bytes: `quality` job, `workflow_call` with 4 inputs,
  actions pinned by SHA, `persist-credentials: false`, repo-specific
  thresholds deliberately left in repo CI).
- In-repo: `.github/workflows/test.yml` (392 lines, 10 jobs — the gate
  matrix warploom already enforces on every push).

## The claim being borrowed

A language kit in this estate is a thin reusable workflow: the shared file
holds the gate *sequence*, each repo passes *parameters*, anything with a
repo-specific threshold stays in repo CI. go-kit proves the shape at 1.7 KB;
rust-kit proves it scales to a 24 KB tiered matrix.

## Novelty assessment

Nothing here is novel. Extracting warploom's 10 jobs into a `workflow_call`
file is a mechanical move; the estate already runs Go, Haskell, Node and
Python kits this way. A C++ kit is a new instance of the established
combination, not an invention.

## Why it fits here, and what it costs

Fit: warploom's CI is the most elaborate in the estate (sanitizer presets,
lavapipe validation, socket live-proofs, shader-count assertions) and the
only C++ repo — every future C++/CUDA repo (a dogfooding game, a modeler)
would otherwise re-derive it. The reusable file is developed and executed
*here first* (warploom CI runs it on every push), then copied verbatim into
`engineering-standards`; the only change at handoff is the caller's `uses:`.
Cost: one extra YAML file, one contract script, zero new dependencies
(pyyaml is stdlib-adjacent test tooling already used by `check_docs_links`).
What was deliberately NOT done: pinning actions by SHA (the estate style).
The current file uses tags; re-pinning in the same move would be an
unverifiable behaviour change (a wrong SHA breaks CI with no local
instrument to catch it). SHA-pinning rides with the estate handoff, where
it can be resolved against the registry and reviewed there.

## What was measured

- `scripts/check_ci_contract.py` parses caller + callee and asserts the
  interface: every input the caller passes is declared by the callee, every
  gate in the warploom matrix exists in the callee, checkouts set
  `persist-credentials: false` (estate convention). Sabotage A/B by
  temporary edit: deleting a gate from the callee fails the check naming
  the gate; passing an undeclared input from the caller fails it naming
  the input. Both reverted.
- Behaviour preservation is structural, not statistical: the callee holds
  byte-identical job bodies (moved, not rewritten); the caller holds only
  `name`/`on`/top-level `concurrency` plus one `uses:` job. `git diff`
  of the extracted steps is empty by construction — verified by diffing
  the step lists before/after.
- Full baseline at the loop boundary (user-selected pace): default ctest
  6/6, asan-ubsan 6/6, tsan 6/6, vulkan-validation 6/6, headless live
  proofs, viewport e1, `lake build` clean. No source file changed, so all
  builds were incremental no-ops; the runs pin the tree green at the
  boundary rather than proving anything about YAML.
