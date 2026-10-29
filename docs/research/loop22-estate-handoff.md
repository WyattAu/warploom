# Loop 22 — estate handoff of cpp-kit (plumbing, no new technique)

Loop 21 extracted warploom's CI into a local `workflow_call` file and left
the handoff pending. This loop executes it. Per AGENTS.md this is plumbing,
so there is no research section beyond the mechanics.

What was done, in order:

1. Cloned `engineering-standards`, confirmed the kit shape from the estate's
   own files: SHA-pinned actions with version comments
   (`checkout@3d3c42e5… # v7.0.1`, `cache@55cc8345… # v6.1.0`,
   `upload-artifact@cf430e03… # v7.0.2`), `workflow_call` inputs per kit,
   repo-specific thresholds stay in the caller.
2. Wrote `.github/workflows/cpp-kit.yml` (tier input `fast|full`, default
   `full`; sanitizer/validation/live-proof/examples legs gated on `full`),
   all action refs SHA-pinned to the SHAs already in use by sibling kits —
   no new SHA was introduced, so nothing unresolvable shipped.
3. First contract design (inline heredoc) was un-runnable: YAML block
   scalars cannot hold column-0 Python, and bash heredocs preserve the
   indent, so both the file and its real CI execution would have failed
   identically. Replaced with an estate checkout at `estate/` plus
   `scripts/check_cpp_contract.py`, which sees both sides. No helper file
   needed in any caller.
4. Warploom's `test.yml` now calls
   `WyattAu/engineering-standards/.github/workflows/cpp-kit.yml@main`;
   local `cpp-gates.yml` and `scripts/check_ci_contract.py` deleted.

Verification: estate YAML parses (12 jobs); the check passes against
warploom's caller (`tier=full, 12 gates`); sabotage A/B — `tier: turbo`
fails naming the value, dropping `presets` fails naming the gate; both
reverted. No source file changed, so the compiled baseline is unaffected;
the real execution proof is warploom CI itself running the estate file on
push. Roadmap E7 updated: handoff done, next is the dogfooding consumer.
