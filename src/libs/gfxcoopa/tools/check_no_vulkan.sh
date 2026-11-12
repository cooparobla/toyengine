#!/usr/bin/env bash
# gfxcoopa/tools/check_no_vulkan.sh — the "gfxcoopa is the sole Vulkan/GLFW
# owner" leak gate. Run over a CONSUMER repo's source tree (blendy, uicoopa,
# pixengine, toyengine) -- never over gfxcoopa's own sources, which are
# expected and allowed to name these symbols freely.
#
# Fails (exit 1) if any Vk*/VK_*/vk*/Vma*/vma*/GLFW*/glfw* symbol, or the
# internal coopa::gfx::detail namespace, appears in *.h/*.hpp/*.cpp/*.cc
# under the given root (excluding build/, includes/, .venv/, .git/).
# Comments are stripped before matching -- a doc comment mentioning
# GLFW_KEY_ESCAPE isn't a leak. A line may opt out of a genuinely
# unavoidable reference with a trailing `// gfx-allow-vulkan` marker; keep
# that budget near zero (see gfxcoopa/README.md's layering rule).
#
# Usage: check_no_vulkan.sh <repo-root>

set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <repo-root>" >&2
    exit 2
fi

root="$1"
if [ ! -d "$root" ]; then
    echo "check_no_vulkan.sh: not a directory: $root" >&2
    exit 2
fi

python3 - "$root" <<'PYEOF'
import re
import sys
import pathlib

root = pathlib.Path(sys.argv[1]).resolve()
EXCLUDE_DIRS = {"build", "includes", ".venv", ".git", ".pytest_cache", "__pycache__"}
SUFFIXES = {".h", ".hpp", ".cpp", ".cc"}

PATTERN = re.compile(
    r"\b("
    r"Vk[A-Z][A-Za-z0-9_]*"
    r"|VK_[A-Z0-9_]+"
    r"|vk[A-Z][A-Za-z0-9_]*"
    r"|Vma[A-Z][A-Za-z0-9_]*"
    r"|vma[A-Z][A-Za-z0-9_]*"
    r"|GLFW[a-zA-Z]*"
    r"|GLFW_[A-Z0-9_]+"
    r"|glfw[A-Z][A-Za-z0-9_]*"
    r"|coopa::gfx::detail"
    r")\b"
)

ESCAPE = "gfx-allow-vulkan"

BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT = re.compile(r"//.*")


def strip_comments(text: str) -> str:
    # Block comments first, replacing each with an equal number of newlines
    # so line numbers stay aligned for the per-line pass below.
    def repl_block(m):
        return "\n" * m.group(0).count("\n")
    text = BLOCK_COMMENT.sub(repl_block, text)
    lines = text.split("\n")
    return "\n".join(LINE_COMMENT.sub("", line) for line in lines)


def iter_files():
    for path in root.rglob("*"):
        if path.suffix not in SUFFIXES or not path.is_file():
            continue
        if any(part in EXCLUDE_DIRS for part in path.relative_to(root).parts):
            continue
        yield path


files = list(iter_files())
violations = []
for path in files:
    try:
        original = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        continue
    stripped = strip_comments(original)
    orig_lines = original.split("\n")
    for lineno, line in enumerate(stripped.split("\n"), start=1):
        if not PATTERN.search(line):
            continue
        source_line = orig_lines[lineno - 1] if lineno - 1 < len(orig_lines) else ""
        if ESCAPE in source_line:
            continue
        violations.append((path.relative_to(root), lineno, source_line.strip()))

if violations:
    print(f"check_no_vulkan.sh: {len(violations)} Vulkan/GLFW leak(s) found under {root}:\n")
    for rel_path, lineno, content in violations:
        print(f"  {rel_path}:{lineno}: {content}")
    print(f"\nSee gfxcoopa/README.md's layering rule -- consumers must interact with "
          f"gfxcoopa's sealed API only. Use `// {ESCAPE}` sparingly for a genuinely "
          f"unavoidable reference.")
    sys.exit(1)

print(f"check_no_vulkan.sh: clean ({len(files)} files scanned under {root})")
PYEOF
