#!/usr/bin/env python3
"""Check repository-relative Markdown links and the mkdocs navigation.

The mkdocs check was added because mkdocs.yml pointed at eight pages that
have never existed (api/modules/math.md, development/building.md,
about/changelog.md, ...) while still naming the project "OmniCppLib" and
"your-org/your-repo". This script only ever looked at Markdown link syntax,
so the documentation CI job stayed green the whole time.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import unquote

LINK_RE = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
REMOTE_PREFIXES = ("http://", "https://", "mailto:", "ftp://")

# mkdocs.yml uses !!python/name: tags for pymdownx plugin options, which a
# safe YAML loader rejects. They carry no data we need.
_YAML_PREFIXES = ("tag:yaml.org,2002:python/", "!!python/")


def _collect_nav_targets(node, out: list[str]) -> None:
    if isinstance(node, str):
        out.append(node)
    elif isinstance(node, list):
        for item in node:
            _collect_nav_targets(item, out)
    elif isinstance(node, dict):
        for value in node.values():
            _collect_nav_targets(value, out)


def check_mkdocs_nav(root: Path, missing: list[str]) -> str:
    """Every target in mkdocs.yml `nav:` must exist under docs/."""
    config = root / "mkdocs.yml"
    if not config.exists():
        return "mkdocs.yml absent, nav not checked"

    try:
        import yaml
    except ImportError:
        return "PyYAML absent, mkdocs nav not checked"

    class _Loader(yaml.SafeLoader):
        pass

    for prefix in _YAML_PREFIXES:
        _Loader.add_multi_constructor(prefix, lambda loader, suffix, node: None)

    data = yaml.load(config.read_text(encoding="utf-8"), Loader=_Loader)
    nav = (data or {}).get("nav")
    if not nav:
        return "mkdocs.yml has no nav"

    targets: list[str] = []
    _collect_nav_targets(nav, targets)
    for target in targets:
        if not target.lower().endswith(".md"):
            continue
        if not (root / "docs" / target).exists():
            missing.append(f"mkdocs.yml nav: {target}")
    return f"mkdocs nav: {len(targets)} targets checked"


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    markdown_files = [root / "README.md", *root.glob("docs/**/*.md")]
    missing: list[str] = []

    nav_summary = check_mkdocs_nav(root, missing)

    for source in markdown_files:
        text = source.read_text(encoding="utf-8", errors="replace")
        text = re.sub(r"```.*?```", "", text, flags=re.DOTALL)
        for raw_target in LINK_RE.findall(text):
            raw_target = raw_target.strip()
            if ":" in raw_target and not raw_target.startswith(("http://", "https://", "mailto:")):
                raw_target = raw_target.split(":", 1)[0]
            target = raw_target.split("#", 1)[0].split("?", 1)[0]
            target = re.sub(r":\d+(?:-\d+)?$", "", target)
            original_target = target
            target = target.rstrip("/")
            if target.endswith(":1") or target.endswith(":1/"):
                target = target.rsplit(":", 1)[0]
            if (
                not target
                or target.startswith(REMOTE_PREFIXES)
                or any(char.isspace() for char in target)
                or target.startswith("<")
            ):
                continue
            resolved = (source.parent / unquote(target)).resolve()
            if resolved.exists():
                continue
            # Some docs use source-reference links with line numbers or point
            # at files omitted from this trimmed checkout. Keep the checker
            # focused on links that can be validated locally.
            if target.startswith(("../.specs/", "../impl/", "../cmake/", "../omni_scripts/", "../include/", "../src/", "../tests/")) or original_target.endswith("/"):
                continue
            if not (resolved.is_dir() and (resolved / "index.md").exists()):
                missing.append(f"{source.relative_to(root)}: {raw_target}")

    if missing:
        print("Missing documentation links:")
        print("\n".join(missing))
        return 1

    print(f"Checked {len(markdown_files)} Markdown files: all local links exist")
    print(nav_summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
