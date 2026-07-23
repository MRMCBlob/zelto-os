#!/usr/bin/env bash
# Build and run meta/kbd-measure.c — the numbers behind the language-model
# decisions in words_en.h and lm_trie.c. See the note at the head of that file.
#
# It compiles the model sources directly rather than linking the keyboard: the
# model has no SDK dependency by design (predict.h says so), and that is exactly
# what makes it measurable without a compositor.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
OUT="${TMPDIR:-/tmp}/zelto-kbd-measure"

# -D_GNU_SOURCE is what the meson build passes, and lm_trie.c needs it for
# clock_gettime under -std=c17. Same flags, same code paths.
cc -O2 -std=c17 -D_GNU_SOURCE -o "$OUT" "$HERE/kbd-measure.c" -lm
"$OUT" "${1:-200}"
