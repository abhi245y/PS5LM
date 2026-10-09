#!/usr/bin/env bash
# Renders the app's dashboard on the PC, off-screen through Mesa (surfaceless
# EGL), to PNG files: the same drawing code as the console, with sample data.
#
#   scripts/preview-app-ui.sh            build/app-preview/*.png
#
# Needs clang++ and the EGL and GL development files (glvnd or Mesa).
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
kit="$root/third_party/ps5-homebrew-ui"
out="$root/build/app-preview"
obj="$out/obj"
mkdir -p "$obj"
cxx=${HOST_CXX:-clang++}
cc=${HOST_CC:-clang}

# The kit's own code that PS5LM uses (no shell, designs or demo), the kit's
# host platform functions, and the app's dashboard.
mapfile -t sources < <(find "$kit/src/gfx" "$kit/src/ui" "$kit/src/core" "$kit/src/audio" "$kit/src/third_party" \
    -type f \( -name '*.cpp' -o -name '*.c' \) | sort)
sources+=("$kit/host/platform_host.cpp" "$root/ps5/app/dashboard.cpp" "$root/ps5/app/stats.cpp"
    "$root/ps5/app/preview_main.cpp")

compile() {
    local src=$1 o
    o="$obj/$(echo "${src#"$root/"}" | tr '/' '_').o"
    if [ ! -f "$o" ] || [ "$src" -nt "$o" ] || [ "$root/ps5/app/dashboard.hpp" -nt "$o" ] || [ "$root/ps5/app/stats.hpp" -nt "$o" ]; then
        if [[ $src == *.c ]]; then
            $cc -std=c11 -O2 -w -I"$kit/src" -c "$src" -o "$o"
        else
            $cxx -std=c++20 -O2 -Wall -Wextra -DGL_GLEXT_PROTOTYPES=1 -I"$kit/src" -I"$kit/host" -I"$kit/third_party" \
                -I"$root/ps5/app" -c "$src" -o "$o"
        fi
    fi
    echo "$o"
}
export -f compile
export root kit obj cxx cc
mapfile -t objects < <(printf '%s\n' "${sources[@]}" | xargs -P "$(nproc)" -I{} bash -c 'compile "$@"' _ {})
$cxx "${objects[@]}" -lEGL -lGL -lm -pthread -o "$out/ps5lm_preview"
"$out/ps5lm_preview" "$kit/assets" "$out"
