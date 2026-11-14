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

**The binary names are derived, not written down.** A project builds `build/<target>` and
`build/tests/<target>_tests` from the `target:` in its `.toy` file, so a guard that names the
engine's binaries only polices the engine's own tree and waves a project's through. Everything
heavy is matched through `project_targets()` instead -- keep it that way, and do not reintroduce
a literal project name here.
"""

import json
import os
import re
import subprocess
import sys

# --- what counts as heavy -------------------------------------------------------------------

# A project's executables are named after its `.toy` target, not after the engine: one project
# builds `build/<name>` and `build/tests/<name>_tests`, the next builds its own. Naming the
# engine's binaries literally here left every one of those invisible to this guard -- a build
# started on top of a running GPU test was waved straight through, which is the exact pair that
# wedges this box. So the names are DERIVED from the project directory, and the engine does not
# need to know what is built on it.
def project_targets():
    """Executable stems this tree builds. Always includes the engine's own."""
    names = []
    root = os.environ.get("CLAUDE_PROJECT_DIR", "")
    if root:
        try:
            for entry in sorted(os.listdir(root)):
                if not entry.endswith(".toy"):
                    continue
                with open(os.path.join(root, entry), encoding="utf-8", errors="replace") as f:
                    for line in f:
                        found = re.match(r"\s*target:\s*([A-Za-z0-9_.-]+)", line)
                        if found:
                            names.append(found.group(1))
                            break
                break   # one project per directory
        except OSError:
            pass        # cannot tell: fall back to the engine's own names
    if "toyengine" not in names:
        names.append("toyengine")
    return names


def heavy_patterns():
    """(regex, what it is) for each command that can saturate the machine on its own."""
    patterns = [
        (r"\bcmake\s+--build\b", "a build"),
        (r"\bmake\s+-C\s+build\b", "a build"),
        (r"\bninja\b", "a build"),
        (r"\bctest\b", "a test run"),
        # A project's wrapper scripts. Each one builds, or runs the game or the editor, and
        # carries no `-j` on the command line -- so without this row `./build.sh` passed through
        # untouched while choosing its own job count (see jobs_reason).
        (r"(^|[\s;&|(])\.?/?(build|run|editor|package)\.sh\b", "a build or an engine run"),
    ]
    # `build[-\w]*` so a debug tree (`build-debug/`, which build.sh makes) counts too.
    for target in project_targets():
        stem = re.escape(target)
        patterns.append((rf"(^|[\s;&|(])\.?/?build[-\w]*/tests/{stem}\w*\b", "a test run"))
        patterns.append((rf"(^|[\s;&|(])\.?/?build[-\w]*/{stem}(_editor|_hub)?\b",
                         "an engine run"))
    return patterns


def busy_names():
    """(process name, what it is) meaning a heavy job is already in flight.

    Matched against the process NAME (`pgrep -x`), never the command line. `pgrep -f ctest`
    would match the shell about to run the very command being judged -- the self-match trap this
    repo already warns about for wait-loops -- and would therefore block every heavy command
    forever.

    A name is the kernel's `comm`, capped at 15 characters, which is why each entry is truncated:
    `toyengine_editor`, `toyengine_editor_tests` and `toyengine_editor_docs` all report
    `toyengine_edito`, and one entry covers all three. That is what we want -- each is heavy.
    """
    # `cc1plus` rather than `cmake`, because a build's cost is its compilers.
    names = [("cc1plus", "a compile"), ("ctest", "a test run")]
    for target in project_targets():
        names.append((f"{target}_tests"[:15], "a test run"))
        names.append((f"{target}_editor"[:15], "an editor or test run"))
        names.append((target[:15], "an engine run"))
    return names


# Parallelism this box survives. The two caps are NOT comparable, because -j means a different
# thing to each tool.
#
# For a build, -j IS the process count: the heaviest TU peaks at 2.4 GB, so -j6 worst-cases at
# ~14 GB and -j20 demanded ~48 GB and hard-rebooted the machine twice (see .claude/CLAUDE.md).
#
# For ctest, -j is a WEIGHT BUDGET that it divides by each test's PROCESSORS. Every GPU suite
# declares `PROCESSORS 4` (src/tests/CMakeLists.txt), so `ctest -j8` runs 8/4 = *two* GPU tests
# at once, not eight -- which is exactly what `SLOT gpu:2` already bounds machine-wide as the hard
# backstop. A cap of 2 here therefore meant 2/4 = ONE test at a time, silently serialising the
# whole GPU tier.
#
# Measured 2026-11-11 on two editor GPU suites: -j2 took 73.05 s (serial), -j8 took 38.13 s
# (concurrent) -- a 1.92x speedup for ~4% per-test contention, at ~412 MB RSS per test, so ~825 MB
# for the pair against 25 GB free. Memory was never the constraint for ctest; the arithmetic was.
MAX_BUILD_JOBS = 4
MAX_TEST_JOBS = 8


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
    for pattern, what in heavy_patterns():
        if re.search(pattern, code):
            return what
    return ""


def already_running() -> str:
    """What heavy job is already in flight, or empty if the machine is idle."""
    for name, what in busy_names():
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
            if cap == MAX_TEST_JOBS:
                return (f"-j{jobs} is over this machine's ctest cap of {cap}. ctest divides -j by "
                        f"each test's PROCESSORS (4 for every GPU suite), so -j{jobs} would ask for "
                        f"{jobs // 4} concurrent Vulkan devices. Use -j{cap} or fewer.")
            return (f"-j{jobs} is over this machine's build cap of {cap}. The heaviest TU here "
                    f"peaks at 2.4 GB and the swapfile is 2 GB, so a high -j does not OOM-kill, "
                    f"it wedges the desktop. Use -j{cap} or fewer.")
    # A bare `-j` means "unlimited", which is the worst case of all.
    if re.search(r"-j(\s|$)", code) and not re.search(r"-j\s*\d", code):
        return f"a bare -j means one job per core (20 here). Pass an explicit -j{MAX_BUILD_JOBS} or fewer."
    # `./build.sh` carries no -j on the command line, so nothing above sees it -- and left to
    # itself `.toyenv.sh`'s build_jobs() takes one job per core capped at one per 3 GB of RAM,
    # which is 10 on a 31 GB box. Against a 2.4 GB peak TU behind a 2 GB swapfile that is the
    # same arithmetic that hard-rebooted the machine, just reached by a different route.
    if re.search(r"(^|[\s;&|(])\.?/?build\.sh\b", code) \
            and not re.search(r"-j\s*\d|\bJOBS=\s*\d", code):
        return (f"./build.sh chooses its own job count -- one per core, capped at one per 3 GB of "
                f"RAM, which is 10 on this box. Pass -j{MAX_BUILD_JOBS} (or JOBS={MAX_BUILD_JOBS}) "
                f"so the build cannot wedge the desktop.")
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
