#!/usr/bin/env python3
"""Check docs/editor/: broken links and anchors, orphan pages, missing Sources lines, stale pages.

Usage: python3 .claude/skills/editor-user-docs/scripts/check_docs.py [--repo <root>]
Exit code 1 when any link, anchor, image, orphan or Sources problem is found; stale pages and
unused screenshots only warn.
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

LINK_RE = re.compile(r"(?<!!)\[[^\]]*\]\(([^)\s]+)\)")
IMG_RE = re.compile(r"!\[[^\]]*\]\(([^)\s]+)\)")
HEADING_RE = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
# "Sources:" plus any wrapped continuation lines, up to a blank line or the Previous / Next line.
SOURCES_RE = re.compile(r"^Sources:\s*((?:.*\n?)(?:(?!Previous:|Next:)\S.*\n?)*)", re.MULTILINE)


def slugify(heading: str) -> str:
    """GitHub-style anchor slug."""
    s = re.sub(r"`|\*|_(?=\S)|(?<=\S)_", "", heading).strip().lower()
    s = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", s)
    s = re.sub(r"[^\w\- ]", "", s)
    return s.replace(" ", "-")


def anchors(path: Path) -> set:
    out, seen, in_code = set(), {}, False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith("```"):
            in_code = not in_code
            continue
        m = None if in_code else HEADING_RE.match(line)
        if not m:
            continue
        base = slugify(m.group(2))
        n = seen.get(base, 0)
        seen[base] = n + 1
        out.add(base if n == 0 else f"{base}-{n}")
    return out


def strip_code(text: str) -> str:
    text = re.sub(r"```.*?```", "", text, flags=re.DOTALL)
    return re.sub(r"`[^`\n]*`", "", text)


def last_change(repo: Path, rel: str) -> float:
    """Commit time of the last change, or mtime when the file has uncommitted edits."""
    p = repo / rel
    if not p.exists():
        return 0.0
    dirty = subprocess.run(["git", "status", "--porcelain", "--", rel], cwd=repo,
                           capture_output=True, text=True).stdout.strip()
    if dirty:
        return p.stat().st_mtime
    ct = subprocess.run(["git", "log", "-1", "--format=%ct", "--", rel], cwd=repo,
                        capture_output=True, text=True).stdout.strip()
    return float(ct) if ct else p.stat().st_mtime


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=os.getcwd())
    repo = Path(ap.parse_args().repo).resolve()
    docs = repo / "docs" / "editor"
    if not docs.is_dir():
        print(f"no {docs.relative_to(repo)}/ yet -- generate the manual first")
        return 1

    pages = sorted(docs.glob("*.md"))
    errors, warnings = [], []
    anchor_cache = {}
    linked = set()

    for page in pages:
        text = page.read_text(encoding="utf-8")
        body = strip_code(text)
        for target in LINK_RE.findall(body) + IMG_RE.findall(body):
            if re.match(r"^[a-z]+:", target):
                continue  # external
            path_part, _, frag = target.partition("#")
            dest = (page.parent / path_part).resolve() if path_part else page
            if not dest.exists():
                errors.append(f"{page.name}: broken link -> {target}")
                continue
            if dest.parent == docs and dest.suffix == ".md":
                linked.add(dest.name)
            if frag and dest.suffix == ".md":
                if dest not in anchor_cache:
                    anchor_cache[dest] = anchors(dest)
                if frag not in anchor_cache[dest]:
                    errors.append(f"{page.name}: missing anchor -> {target}")

        m = SOURCES_RE.search(text)
        if not m:
            errors.append(f"{page.name}: no 'Sources:' line")
            continue
        sources = re.findall(r"`([^`]+)`", m.group(1))
        missing = [s for s in sources if not (repo / s).exists()]
        for s in missing:
            errors.append(f"{page.name}: Sources path does not exist: {s}")
        page_time = last_change(repo, str(page.relative_to(repo)))
        newer = [s for s in sources if s not in missing and last_change(repo, s) > page_time]
        if newer:
            warnings.append(f"{page.name}: stale -- changed since the page: {', '.join(newer)}")

    shots = repo / "docs" / "images" / "editor"
    used = {Path(m).name for page in pages for m in IMG_RE.findall(page.read_text(encoding="utf-8"))}
    for img in sorted(shots.glob("*.jpg")) if shots.is_dir() else []:
        if img.name not in used:
            warnings.append(f"{img.name}: screenshot not used by any page")

    for page in pages:
        if page.name != "README.md" and page.name not in linked:
            errors.append(f"{page.name}: orphan (nothing in docs/editor/ links to it)")

    for e in errors:
        print("ERROR  " + e)
    for w in warnings:
        print("WARN   " + w)
    print(f"{len(pages)} pages, {len(errors)} errors, {len(warnings)} warnings")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
