#!/usr/bin/env bash
# editor.sh [--scene <scene.yaml>] [--debug] [-- <editor args>]
#
# Opens this project in its editor (built with src/ linked in, so your components load and Play
# runs them). The first open creates assets/. Builds the editor first if it is not built yet.
#
#   --scene <path>  open that scene instead of the last one
#   --debug         run the build-debug/ binary (Vulkan validation on)
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/.toyenv.sh"

args=()
while (( $# )); do
    case "$1" in
        -h|--help) usage ;;
        --debug)   BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build-debug}"; shift ;;
        --)        shift; args+=("$@"); break ;;
        *)         args+=("$1"); shift ;;
    esac
done

bin="${BUILD_DIR:-$PROJECT_DIR/build}/${TOY_TARGET}_editor"
[[ "$bin" = /* ]] || bin="$PROJECT_DIR/$bin"
ensure_binary "$bin"
cd "$PROJECT_DIR"
exec "$bin" "$PROJECT_DIR" ${args[@]+"${args[@]}"}
