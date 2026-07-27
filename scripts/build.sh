#!/usr/bin/env bash
#
# Local ZMK build via docker (no toolchain install needed).
#
#   scripts/build.sh              build left + right
#   scripts/build.sh left         build left half only
#   scripts/build.sh right        build right half only
#   scripts/build.sh reset        build settings-reset firmware (fixes BT pairing issues)
#   scripts/build.sh all          left + right + reset
#   scripts/build.sh update       (re-)run west update (after changing west.yml)
#   scripts/build.sh pristine     wipe build dirs, then build left + right
#
# Output: firmware/left.uf2, firmware/right.uf2, firmware/settings_reset.uf2
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="zmkfirmware/zmk-build-arm:stable"
BOARD="nice_nano_v2"
SHIELD_LEFT="lily58_left"
SHIELD_RIGHT="lily58_right"

run_docker() {
    docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
        -e ZEPHYR_BASE=/work/zephyr -e CMAKE_PREFIX_PATH=/work/zephyr \
        -v "$REPO":/work -w /work "$IMAGE" "$@"
}

ensure_workspace() {
    if [ ! -d "$REPO/.west" ]; then
        echo ">>> First run: initializing west workspace (downloads ~2 GB, one time)"
        run_docker west init -l config
        run_docker west update
    fi
}

west_update() {
    run_docker west update
}

build() {
    local name="$1" shield="$2" pristine="${3:-}" extra=()
    if [ "$name" = "left" ]; then
        extra=(-S studio-rpc-usb-uart)
    fi
    echo ">>> Building $name ($shield)"
    run_docker west build -s zmk/app -d "build/$name" -b "$BOARD" ${pristine:+-p} \
        "${extra[@]}" -- -DSHIELD="$shield" -DZMK_CONFIG=/work/config
    mkdir -p "$REPO/firmware"
    cp "$REPO/build/$name/zephyr/zmk.uf2" "$REPO/firmware/$name.uf2"
    echo ">>> firmware/$name.uf2 ready"
}

build_reset() {
    echo ">>> Building settings_reset"
    run_docker west build -s zmk/app -d build/settings_reset -b "$BOARD" \
        -- -DSHIELD=settings_reset -DZMK_CONFIG=/work/config
    mkdir -p "$REPO/firmware"
    cp "$REPO/build/settings_reset/zephyr/zmk.uf2" "$REPO/firmware/settings_reset.uf2"
    echo ">>> firmware/settings_reset.uf2 ready"
}

cmd="${1:-both}"
case "$cmd" in
    update)   ensure_workspace; west_update ;;
    left)     ensure_workspace; build left  "$SHIELD_LEFT" ;;
    right)    ensure_workspace; build right "$SHIELD_RIGHT" ;;
    reset)    ensure_workspace; build_reset ;;
    all)      ensure_workspace; build left "$SHIELD_LEFT"; build right "$SHIELD_RIGHT"; build_reset ;;
    both)     ensure_workspace; build left "$SHIELD_LEFT"; build right "$SHIELD_RIGHT" ;;
    pristine) ensure_workspace; build left "$SHIELD_LEFT" p; build right "$SHIELD_RIGHT" p ;;
    *) echo "usage: $0 [left|right|reset|all|both|pristine|update]" >&2; exit 1 ;;
esac
