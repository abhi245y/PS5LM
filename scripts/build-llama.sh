#!/usr/bin/env bash
# Cross-compiles llama.cpp's CPU backend and tools for the PS5 as payload ELFs.
#
#   scripts/build-llama.sh                 configure and build into build/llama-ps5
#   scripts/build-llama.sh llama-cli       build one target
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=env.sh
source "$root/scripts/env.sh"

src="$root/third_party/llama.cpp"
out="$root/build/llama-ps5"
targets=("${@:-llama-cli llama-server llama-bench}")

if [ ! -f "$src/CMakeLists.txt" ]; then
    echo "build-llama: run git submodule update --init first" >&2
    exit 1
fi

# The PS5's CPU is a Zen 2: AVX2, FMA, F16C and BMI2, no AVX-512.
# No OpenMP in the SDK, so ggml's own thread pool. Static everything: a payload
# is one ELF. No HTTPS client (models are copied to the console, not downloaded).
# The SDK's libc predates posix_spawn_file_actions_addchdir_np; nothing on the
# console spawns processes with a working directory, so subprocess.h goes without.
ps5_flags="-DSUBPROCESS_HAVE_CWD=0"
# There is no libm: the math functions are in Sony's libc (libSceLibcInternal).
"$PS5_PAYLOAD_SDK/bin/prospero-cmake" -S "$src" -B "$out" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$ps5_flags" \
    -DCMAKE_CXX_FLAGS="$ps5_flags" \
    -DMATH_LIBRARY=SceLibcInternal \
    -DBUILD_SHARED_LIBS=OFF \
    -DGGML_NATIVE=OFF \
    -DGGML_BACKEND_DL=OFF \
    -DGGML_CPU_ALL_VARIANTS=OFF \
    -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_BMI2=ON \
    -DGGML_AVX512=OFF \
    -DGGML_OPENMP=OFF \
    -DGGML_CCACHE=OFF \
    -DLLAMA_OPENSSL=OFF \
    -DLLAMA_BUILD_TESTS=OFF \
    -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_BUILD_TOOLS=ON \
    -DLLAMA_BUILD_SERVER=ON \
    -DCMAKE_VERBOSE_MAKEFILE=OFF

# shellcheck disable=SC2068
cmake --build "$out" --target ${targets[@]}
