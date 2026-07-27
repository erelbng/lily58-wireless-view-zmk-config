#!/usr/bin/env bash
#
# Render config/lily58.keymap to a pretty SVG (keymap-drawer via uvx).
#
#   scripts/draw-keymap.sh          parse keymap + draw keymap-drawer/lily58.svg
#
# The intermediate keymap-drawer/lily58.yaml is kept: you can hand-edit labels
# there (e.g. rename keys, add combos) and re-run only the draw step with:
#   scripts/draw-keymap.sh draw
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="$REPO/keymap-drawer"
KD=(uvx --from keymap-drawer keymap)

if [ "${1:-}" != "draw" ]; then
    "${KD[@]}" -c "$DIR/config.yaml" parse -z "$REPO/config/lily58.keymap" \
        > "$DIR/lily58.yaml"
    echo ">>> parsed to keymap-drawer/lily58.yaml"
fi

"${KD[@]}" -c "$DIR/config.yaml" draw "$DIR/lily58.yaml" > "$DIR/lily58.svg"
echo ">>> drawn to keymap-drawer/lily58.svg"
