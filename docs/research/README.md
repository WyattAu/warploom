# Research notes

One file per technique actually adopted. A file here means the technique is in
the tree; an idea that was evaluated and rejected belongs in the commit that
rejected it, not here.

The point of these notes is not credit, it is falsifiability. A future reader
should be able to check the claim, check whether it is novel, and check whether
the numbers came from this codebase or from a paper.

## Required sections

Each note has five parts, in this order:

1. **Source** — full citation: authors, title, venue, year, and a stable
   identifier (DOI, ACM DL, IEEE Xplore, USENIX). Vendor documentation is
   acceptable for API behaviour and must be labelled as such.
2. **The claim** — the specific assertion being borrowed, phrased so it can be
   checked rather than paraphrased into vagueness.
3. **Novelty assessment** — see below.
4. **Fit and cost** — why it suits this engine, what it costs in dependencies,
   determinism consequences, and platform assumptions.
5. **What was measured here** — numbers from this codebase. A paper's speedup
   is not this engine's speedup, and the difference is usually the interesting
   part.

## On novelty

The honest default is that a technique is **not** novel. Say plainly which of
these applies:

- **Standard.** Published, in production use elsewhere. Say where.
- **A combination.** Two or more published techniques applied together in a way
  that does not appear in the literature. A combination is a combination; do not
  upgrade it to "novel".
- **Actually new.** Requires a specific argument for why the result is new,
  plus a description of what would falsify it. Be suspicious of this category.

"Does not appear in the literature" means a search was run and is described.
Not searching and finding nothing is not evidence of absence.

## Source preference

Prefer, in order: ACM Digital Library and SIGGRAPH proceedings; IEEE journals
and conferences; ACM TOMS and TOG; USENIX ATC and OSDI; peer-reviewed
dissertations; NVIDIA and AMD developer documentation (for API behaviour only);
arXiv last.

arXiv is neither peer-reviewed nor archival. A preprint is cited as a preprint
and never as the settled position on a question.

## Index

| Technique | Source | Status |
|---|---|---|
| _(none yet)_ | | |

This index is kept empty rather than aspirational. It fills in when a technique
lands, not before.
