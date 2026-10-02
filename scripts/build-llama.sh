#!/usr/bin/env bash
# Cross-compiles PS5LM for the PS5: the ps5lm payload and llama.cpp's tools.
#
#   scripts/build-llama.sh                 configure and build into build/llama-ps5
#   scripts/build-llama.sh ps5lm           build one target
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=env.sh
source "$root/scripts/env.sh"

llama="$root/third_party/llama.cpp"
out="$root/build/llama-ps5"
mbedtls="$root/.deps/mbedtls-ps5"
targets=("${@:-ps5lm llama-cli llama-server llama-bench llama-completion}")

if [ ! -f "$llama/CMakeLists.txt" ]; then
    echo "build-llama: run git submodule update --init first" >&2
    exit 1
fi
if [ ! -f "$mbedtls/lib/libmbedtls.a" ]; then
    "$root/scripts/build-mbedtls.sh"
fi

# The PS5 changes to llama.cpp live in patches/ and go onto the submodule's
# working tree; a patch that is already applied is skipped.
for p in "$root"/patches/*.patch; do
    [ -e "$p" ] || continue
    if git -C "$llama" apply --reverse --check "$p" 2>/dev/null; then
        continue
    fi
    git -C "$llama" apply "$p"
    echo "build-llama: applied $(basename "$p")"
done

# The build tree used to be configured from llama.cpp directly; ps5/ is the
# top-level project now, and CMake refuses a tree from another source dir.
if [ -f "$out/CMakeCache.txt" ] && ! grep -q "CMAKE_HOME_DIRECTORY:INTERNAL=$root/ps5$" "$out/CMakeCache.txt"; then
    rm -rf "$out"
fi

# The PS5's CPU is a Zen 2: AVX2, FMA, F16C and BMI2, no AVX-512.
# No OpenMP in the SDK, so ggml's own thread pool. Static everything: a payload
# is one ELF.
# The SDK's libc predates posix_spawn_file_actions_addchdir_np; nothing on the
# console spawns processes with a working directory, so subprocess.h goes without.
# cpp-httplib's HTTPS goes through mbedTLS (the payload downloads models), for
# every file that includes httplib.h, so its classes match across the build.
ps5_flags="-DSUBPROCESS_HAVE_CWD=0 -DCPPHTTPLIB_MBEDTLS_SUPPORT -I$mbedtls/include"
# There is no libm: the math functions are in Sony's libc (libSceLibcInternal).

# ps5/compat fills in the libc functions the console lacks. It and mbedTLS go
# at the very end of every link line (CMAKE_*_STANDARD_LIBRARIES), after the
# objects that need them.
compat_dir="$out/ps5compat"
mkdir -p "$compat_dir"
"$PS5_PAYLOAD_SDK/bin/prospero-clang" -O2 -Wall -Wextra -Werror -c \
    "$root/ps5/compat/compat.c" -o "$compat_dir/compat.o"
rm -f "$compat_dir/libps5compat.a"
"$PS5_PAYLOAD_SDK/bin/prospero-ar" rcs "$compat_dir/libps5compat.a" "$compat_dir/compat.o"
std_libs="$compat_dir/libps5compat.a $mbedtls/lib/libmbedtls.a $mbedtls/lib/libmbedx509.a"
std_libs="$std_libs $mbedtls/lib/libmbedcrypto.a $mbedtls/lib/libeverest.a $mbedtls/lib/libp256m.a"

"$PS5_PAYLOAD_SDK/bin/prospero-cmake" -S "$root/ps5" -B "$out" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$ps5_flags" \
    -DCMAKE_CXX_FLAGS="$ps5_flags" \
    -DMATH_LIBRARY=SceLibcInternal \
    -DCMAKE_C_STANDARD_LIBRARIES="$std_libs" \
    -DCMAKE_CXX_STANDARD_LIBRARIES="$std_libs" \
    -DBUILD_SHARED_LIBS=OFF \
    -DGGML_NATIVE=OFF \
    -DGGML_BACKEND_DL=OFF \
    -DGGML_CPU_ALL_VARIANTS=OFF \
    -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_BMI2=ON \
    -DGGML_AVX512=OFF \
    -DGGML_OPENMP=OFF \
    -DGGML_CCACHE=OFF \
    -DLLAMA_OPENSSL=OFF \
    -DLLAMA_BUILD_COMMON=ON \
    -DLLAMA_BUILD_TESTS=OFF \
    -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_BUILD_TOOLS=ON \
    -DLLAMA_BUILD_SERVER=ON \
    -DCMAKE_VERBOSE_MAKEFILE=OFF

# shellcheck disable=SC2068
cmake --build "$out" --target ${targets[@]}
