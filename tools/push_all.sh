#!/usr/bin/env bash
# push_all.sh -- commit and push toyengine and every submodule with one message.
#
#   tools/push_all.sh "message"          # add, commit, push everything
#   tools/push_all.sh --dryrun "message" # dry run: print what would happen
#
# For each repo -- every submodule, nested ones included, then toyengine itself:
#   git add .  ->  git commit -m "message" (only if something changed)
#           ->  git push (only if the branch is ahead of its upstream, or has none yet)
#
# Order matters: submodules are handled deepest-first and toyengine last, so each
# parent's commit records submodule pointers that have already been pushed. A
# clone can never reference a submodule commit that isn't on the remote.
#
# Before anything is committed, every repo that has work is checked to be on a
# branch. Submodules check out a detached HEAD by default, and a detached HEAD
# can't be pushed -- so the script refuses up front instead of failing halfway.
# Repos with nothing to commit or push (e.g. an untouched third-party nested
# submodule) are skipped, so their detached HEAD doesn't matter.
set -euo pipefail

dry_run=false
if [[ "${1:-}" == "--dryrun" ]]; then
    dry_run=true
    shift
fi
if [[ $# -ne 1 || -z "$1" ]]; then
    echo "usage: $(basename "$0") [--dryrun] \"commit message\"" >&2
    exit 1
fi
message="$1"

root="$(git -C "$(dirname "$0")/.." rev-parse --show-toplevel)"

# Submodules in reverse traversal order (children before parents), then the root.
# (Written for macOS's bash 3.2 too: no mapfile, and no expanding an empty array under set -u.)
repos=()
while IFS= read -r path; do
    [[ -n "$path" ]] && repos=("$root/$path" ${repos[@]+"${repos[@]}"})
done < <(git -C "$root" submodule foreach --recursive --quiet 'echo "$displaypath"')
repos+=("$root")

has_changes() { [[ -n "$(git -C "$1" status --porcelain)" ]]; }

needs_push() {
    local upstream
    # Detached HEAD: there is no branch to push. (If it also has changes, the
    # pre-flight check below reports it.)
    git -C "$1" symbolic-ref -q HEAD >/dev/null || return 1
    upstream="$(git -C "$1" rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null)" || return 0
    [[ "$(git -C "$1" rev-list --count "$upstream..HEAD")" -gt 0 ]]
}

name_of() { [[ "$1" == "$root" ]] && echo "toyengine" || echo "${1#"$root"/}"; }

# --- Pre-flight: every repo with work must be on a branch. ---
blocked=()
for repo in "${repos[@]}"; do
    if has_changes "$repo" || needs_push "$repo"; then
        if ! git -C "$repo" symbolic-ref -q HEAD >/dev/null; then
            blocked+=("$(name_of "$repo")")
        fi
    fi
done
if [[ ${#blocked[@]} -gt 0 ]]; then
    echo "push_all: these repos have work but are on a detached HEAD:" >&2
    printf '  %s\n' "${blocked[@]}" >&2
    echo "Check out a branch in each first (e.g. git -C <path> checkout main)." >&2
    exit 1
fi

run() {
    if $dry_run; then echo "    + $*"; else "$@"; fi
}

# --- Commit and push. ---
for repo in "${repos[@]}"; do
    name="$(name_of "$repo")"
    changed=false; has_changes "$repo" && changed=true

    if ! $changed && ! needs_push "$repo"; then
        echo "-- $name: nothing to do"
        continue
    fi

    branch="$(git -C "$repo" symbolic-ref --short HEAD)"
    echo "== $name ($branch)"

    if $changed; then
        run git -C "$repo" add .
        run git -C "$repo" commit -q -m "$message"
    fi

    # In a dry run the commit didn't happen, so re-check against the real state:
    # anything changed, or already ahead, would be pushed.
    if $changed || needs_push "$repo"; then
        if git -C "$repo" rev-parse --abbrev-ref '@{u}' >/dev/null 2>&1; then
            run git -C "$repo" push
        else
            run git -C "$repo" push -u origin "$branch"
        fi
    fi
done

$dry_run && echo "(dry run -- nothing was committed or pushed)"
echo "done."
