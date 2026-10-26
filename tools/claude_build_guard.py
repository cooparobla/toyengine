#!/usr/bin/env python3
"""Refuses the two Bash commands that have hard-locked this machine.

Wired as a `PreToolUse` hook on `Bash` in `.claude/settings.json`, so it sees every shell
command Claude is about to run and can deny it with a reason. It exists because the rules in
`.claude/CLAUDE.md` were already there, in writing, and were not enough: the failure mode is a
frozen desktop and a hard reboot, which is too expensive to leave to good intentions.

**What actually went wrong.** The heavy things in this repo are each a whole machine's worth of
work: one compiler TU peaks at 2.4 GB, and one GPU test constructs a full Vulkan device plus a
20-thread JobEngine. The box has 31 GB of RAM behind a 2 GB swapfile, so there is no graceful
OOM kill -- it wedges. Two mistakes stacked:

1. Sweeps were launched with `nohup ... & disown`. A detached process outlives the session, so
   the harness could neither track nor stop it, and successive sessions left four concurrent
   `ctest` runs alive at once.
2. Builds and manual engine runs were started *while* a sweep was running.

So this blocks exactly those: detaching, and starting a second heavy job while one is live.
Everything else passes straight through.

Deliberately NOT an advisory warning. A warning is what the CLAUDE.md rules already were.
"""

import json
import os
import re
import subprocess
import sys

# --- what counts as heavy -------------------------------------------------------------------

# Each of these can saturate the machine on its own. Matched against the command text.
HEAVY_PATTERNS = [
    (r"\bcmake\s+--build\b", "a build"),
    (r"\bmake\s+-C\s+build\b", "a build"),
    (r"\bninja\b", "a build"),
    (r"\bctest\b", "a test run"),
    (r"(^|[\s;&|(])\.?/?build/toyengine(_editor|_hub)?\b", "an engine run"),
    (r"(^|[\s;&|(])\.?/?build/tests/toyengine\w*\b", "a test run"),
]

# Processes that mean one is already in flight. `cc1plus` rather than `cmake`, because a build's
# cost is its compilers; `ctest` and the test binaries, because a GPU test is a Vulkan device.
#
# Matched against the process NAME (`pgrep -x`), never the command line. `pgrep -f ctest` would
# match the shell about to run the very command being judged -- the self-match trap this repo
# already warns about for wait-loops -- and would therefore block every heavy command forever.
#
# A name is the kernel's `comm`, capped at 15 characters, so `toyengine_editor`,
# `toyengine_editor_tests` and `toyengine_editor_docs` all report `toyengine_edito`. One entry
# covers all three, which is what we want: each is heavy.
BUSY_NAMES = [
    ("cc1plus", "a compile"),
    ("ctest", "a test run"),
    ("toyengine_tests", "a test run"),
    ("toyengine_edito", "an editor or test run"),
    ("toyengine", "an engine run"),
]

# Parallelism this box survives. Measured: the heaviest TU peaks at 2.4 GB, so -j6 worst-cases
# at ~14 GB; -j20 demanded ~48 GB and hard-rebooted the machine twice (see .claude/CLAUDE.md).
MAX_BUILD_JOBS = 4
MAX_TEST_JOBS = 2


def deny(reason: str) -> None:
    """Refuses the command, telling the model what to do instead."""
    print(json.dumps({
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": reason,
        }
    }))
    sys.exit(0)


def strip_heredocs(command: str) -> str:
    """The command with every here-document BODY removed.

    Writing a file through `cat > x <<'EOF' ... EOF` puts its whole contents in the command
    text. Without this, documenting these very rules -- or writing any code that mentions
    `ctest` or `-j4` -- reads as an attempt to run them. (This guard blocked its own
    documentation being written, which is how the case was found.)
    """
    out, lines = [], command.split("\n")
    i = 0
    while i < len(lines):
        out.append(lines[i])
        starts = re.findall(r"<<-?\s*['\"]?([A-Za-z_][A-Za-z0-9_]*)['\"]?", lines[i])
        i += 1
        for delimiter in starts:
            while i < len(lines) and lines[i].strip() != delimiter:
                i += 1          # body: dropped
            if i < len(lines):
                i += 1          # the closing delimiter line
    return "\n".join(out)


def strip_quoted(command: str) -> str:
    """The command with quoted runs blanked, so a word inside a string is not read as syntax.

    `grep -n "nohup" file` must not look like a detach, and `echo "build &"` must not look like
    a backgrounded job.
    """
    return re.sub(r"'[^']*'|\"[^\"]*\"", "''", command)


def detach_reason(code: str) -> str:
    """Why this command would outlive the session, or empty if it would not."""
    for word in ("nohup", "disown", "setsid"):
        if re.search(rf"(^|[\s;&|(]){word}\b", code):
            return word
    # A trailing `&` backgrounds; `&&` and `&>` do not. Check each segment's tail.
    for segment in re.split(r"[;\n]|\|\||&&", code):
        if re.search(r"(?<![&>])&\s*$", segment.strip()):
            return "a trailing &"
    return ""


def heavy_reason(code: str) -> str:
    """What kind of heavy job this command starts, or empty if it is not one."""
    for pattern, what in HEAVY_PATTERNS:
        if re.search(pattern, code):
            return what
    return ""


def already_running() -> str:
    """What heavy job is already in flight, or empty if the machine is idle."""
    for name, what in BUSY_NAMES:
        try:
            found = subprocess.run(["pgrep", "-x", name], capture_output=True, timeout=5)
        except (OSError, subprocess.SubprocessError):
            return ""   # cannot tell: do not block on a broken probe
        if found.returncode == 0 and found.stdout.strip():
            return what
    return ""


def jobs_reason(code: str) -> str:
    """Why this command's parallelism is too high for this box, or empty if it is fine."""
    for match in re.finditer(r"-j\s*(\d+)", code):
        jobs = int(match.group(1))
        cap = MAX_TEST_JOBS if re.search(r"\bctest\b", code) else MAX_BUILD_JOBS
        if jobs > cap:
            return (f"-j{jobs} is over this machine's cap of {cap}. The heaviest TU here peaks "
                    f"at 2.4 GB and the swapfile is 2 GB, so a high -j does not OOM-kill, it "
                    f"wedges the desktop. Use -j{cap} or fewer.")
    # A bare `-j` means "unlimited", which is the worst case of all.
    if re.search(r"-j(\s|$)", code) and not re.search(r"-j\s*\d", code):
        return "a bare -j means one job per core (20 here). Pass an explicit -j4 or fewer."
    return ""


def main() -> None:
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return   # not something we can judge; let it through
    if payload.get("tool_name") != "Bash":
        return
    command = payload.get("tool_input", {}).get("command", "")
    if not isinstance(command, str) or not command.strip():
        return

    code = strip_quoted(strip_heredocs(command))

    detach = detach_reason(code)
    if detach:
        deny(
            f"Blocked: `{detach}` detaches the process from this session.\n\n"
            "A detached job outlives the session, so neither you nor the harness can stop it. "
            "Four orphaned `ctest` sweeps stacked up this way and hard-locked the machine.\n\n"
            "Use the Bash tool's own `run_in_background: true` instead -- the harness tracks it, "
            "notifies you when it exits, and tears it down with the session."
        )

    heavy = heavy_reason(code)
    if not heavy:
        return   # ordinary command: nothing to police

    jobs = jobs_reason(code)
    if jobs:
        deny(f"Blocked: {jobs}")

    busy = already_running()
    if busy:
        deny(
            f"Blocked: this starts {heavy}, but {busy} is already running.\n\n"
            "One heavy job at a time on this box. A build and a GPU test run together exceed "
            "31 GB behind a 2 GB swapfile, which wedges the desktop rather than OOM-killing.\n\n"
            "Wait for the running one to finish. `pgrep -fa 'cc1plus|ctest|toyengine'` shows "
            "what it is; if it is an orphan from an earlier session, kill it first."
        )


if __name__ == "__main__":
    main()
