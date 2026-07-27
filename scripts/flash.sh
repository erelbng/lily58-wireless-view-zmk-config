#!/usr/bin/env bash
#
# Flash a nice!nano via its UF2 bootloader.
#
#   scripts/flash.sh left
#   scripts/flash.sh right
#   scripts/flash.sh reset        flash settings-reset firmware
#   scripts/flash.sh both         left, then right (prompts in between)
#
# Double-tap the reset button on the half you want to flash; the script
# waits for the NICENANO drive to show up, copies the firmware, done.
# The board reboots into the new firmware by itself.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL="NICENANO"

flash_one() {
    local name="$1"
    local uf2="$REPO/firmware/$name.uf2"
    [ "$name" = "reset" ] && uf2="$REPO/firmware/settings_reset.uf2"

    if [ ! -f "$uf2" ]; then
        echo "!! $uf2 missing — run scripts/build.sh first" >&2
        exit 1
    fi

    echo ">>> Double-tap RESET on the *$name* half now (waiting for $LABEL drive)..."
    local dev=""
    while true; do
        dev="$(readlink -f "/dev/disk/by-label/$LABEL" 2>/dev/null || true)"
        [ -n "$dev" ] && [ -b "$dev" ] && break
        sleep 0.5
    done
    echo ">>> Found bootloader at $dev"

    # Already automounted by the desktop?
    local mnt
    mnt="$(findmnt -rn -o TARGET "$dev" 2>/dev/null || true)"
    if [ -z "$mnt" ]; then
        if command -v udisksctl >/dev/null; then
            udisksctl mount -b "$dev" >/dev/null
            mnt="$(findmnt -rn -o TARGET "$dev")"
        else
            echo "!! No automount and no udisksctl. Mount $dev manually and re-run." >&2
            exit 1
        fi
    fi

    echo ">>> Copying $(basename "$uf2") to $mnt"
    cp "$uf2" "$mnt/"
    sync || true
    echo ">>> Done. Board reboots itself with new firmware."
}

case "${1:-}" in
    left|right|reset) flash_one "$1" ;;
    both)
        flash_one left
        echo
        flash_one right
        ;;
    *) echo "usage: $0 left|right|reset|both" >&2; exit 1 ;;
esac
