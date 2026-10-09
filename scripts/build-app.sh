#!/usr/bin/env bash
# Builds PS5LM's native app: llama-server with ggml-vulkan on RADV, as a title
# folder (eboot.bin, sce_sys/param.json, sce_module/libc.prx) for
# ShadowMountPlus, in build/app-ps5/title/<TITLE_ID>. The icon and the
# backgrounds come from ps5/app/sce_sys (sources in its source/ folder).
#
# Needs PS5_Vulkan next to its forks (.deps/src: PS5_Vulkan, PS5_Mesa,
# PS5_PayloadSDK), set up with its tools/setup-native-dependencies.sh,
# tools/build-radv.sh release and `make app` (for the host tool and libc.prx).
#
#   scripts/build-app.sh            configure, build and package
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
vk=${PS5_VULKAN_ROOT:-$root/.deps/src/PS5_Vulkan}
sdk="$vk/.deps/native/ps5-payload-sdk"
radv="$vk/.deps/native/radv-release"
archive="$radv/lib/libvulkan_radeon.ps5.a"
tool="$vk/build/host/ps5-native-tool"
llama="$root/third_party/llama.cpp"
out="$root/build/app-ps5"
headers="$root/.deps/ps5-vk-headers"
sce_sys="$root/ps5/app/sce_sys"
param="$sce_sys/param.json"

for f in "$archive" "$tool" "$vk/runtime/libc.prx" "$sdk/bin/prospero-lld"; do
    [ -e "$f" ] || { echo "build-app: missing $f" >&2; exit 2; }
done
export PS5_PAYLOAD_SDK="$sdk" PS5_VULKAN_ROOT="$vk"

# radv-link.sh takes Clang's builtins from <resource dir>/lib/linux/, where
# Debian and Arch put them; Fedora keeps them under the target triple. A clang
# that reports a resource dir with that layout covers both.
builtins=$(clang --print-file-name=libclang_rt.builtins.a)
[ -f "$builtins" ] || builtins="$(clang --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"
shim="$root/.deps/clang-shim"
mkdir -p "$shim/resource/lib/linux"
ln -sf "$builtins" "$shim/resource/lib/linux/libclang_rt.builtins-x86_64.a"
printf '#!/bin/sh\n[ "$1" = --print-resource-dir ] && { echo "%s"; exit 0; }\nexec clang "$@"\n' \
    "$shim/resource" > "$shim/clang"
chmod +x "$shim/clang"

# The Vulkan and SPIR-V headers, copied out of the host's /usr/include so the
# cross build never sees the host's libc headers next to them.
if [ ! -f "$headers/include/vulkan/vulkan.hpp" ]; then
    mkdir -p "$headers/include" "$headers/cmake"
    cp -r /usr/include/vulkan /usr/include/vk_video /usr/include/spirv "$headers/include/"
    cat > "$headers/cmake/SPIRV-HeadersConfig.cmake" <<EOF
add_library(SPIRV-Headers::SPIRV-Headers INTERFACE IMPORTED)
set_target_properties(SPIRV-Headers::SPIRV-Headers PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "$headers/include")
set(SPIRV-Headers_FOUND TRUE)
EOF
fi

# The PS5 changes to llama.cpp (patches/), as scripts/build-llama.sh applies them.
for p in "$root"/patches/*.patch; do
    git -C "$llama" apply --reverse --check "$p" 2>/dev/null || git -C "$llama" apply "$p"
done

flags="-DSUBPROCESS_HAVE_CWD=0 -I$headers/include"
# llama.cpp throws, catches and uses dynamic_cast; the PS5 target turns
# exceptions and RTTI off by default.
cxxflags="$flags -fexceptions -fcxx-exceptions -frtti"
cmake -S "$llama" -B "$out/llama" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/ps5/app/toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$cxxflags" \
    -DBUILD_SHARED_LIBS=OFF -DGGML_BACKEND_DL=OFF -DGGML_NATIVE=OFF -DGGML_CPU_ALL_VARIANTS=OFF \
    -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_BMI2=ON -DGGML_AVX512=OFF \
    -DGGML_OPENMP=OFF -DGGML_CCACHE=OFF -DGGML_VULKAN=ON \
    -DVulkan_INCLUDE_DIR="$headers/include" -DVulkan_LIBRARY="$archive" \
    -DVulkan_GLSLC_EXECUTABLE="$(command -v glslc)" -DSPIRV-Headers_DIR="$headers/cmake" \
    -DMATH_LIBRARY=SceLibcInternal \
    -DLLAMA_OPENSSL=OFF -DLLAMA_BUILD_COMMON=ON -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_BUILD_TOOLS=ON -DLLAMA_BUILD_SERVER=ON > "$out.configure.log" 2>&1 ||
    { tail -30 "$out.configure.log" >&2; exit 1; }
cmake --build "$out/llama" --target llama-server-impl

# The app's own objects, and the title's start code from PS5_Vulkan.
obj="$out/obj"
mkdir -p "$obj" "$out/stubs"
cxx() { "$root/ps5/app/ps5cxx" -std=c++17 -O2 -ffunction-sections -fdata-sections "$@"; }
cxx -fexceptions -fcxx-exceptions -frtti -I "$headers/include" -I "$llama/ggml/include" -c "$root/ps5/app/main.cpp" -o "$obj/main.o"
"$root/ps5/app/ps5cc" -O2 -c "$root/ps5/app/compat_app.c" -o "$obj/compat_app.o"
# In an archive, so --exclude-libs keeps them local: a stub module defines some
# of the same names, and lld would otherwise export ours to interpose them,
# which the title converter refuses.
rm -f "$obj/libps5app_compat.a"
"$sdk/bin/llvm-ar" rcs "$obj/libps5app_compat.a" "$obj/compat_app.o"
cxx -std=c++20 -fno-exceptions -fno-rtti -c "$vk/tooling/native/app_crt.cpp" -o "$obj/app_crt.o"
cxx -std=c++20 -fno-exceptions -fno-rtti -c "$vk/tooling/native/app_cpp_runtime.cpp" -o "$obj/app_cpp_runtime.o"

# AGC comes from system modules; these stubs only name the imports.
for pair in libSceAgc:agc_canary_link_stub.c libSceAgcDriver:agc_driver_canary_link_stub.c; do
    lib=${pair%%:*}
    "$root/ps5/app/ps5cc" -O2 -fPIC -c "$vk/vendor/ps5/sdk/stubs/${pair#*:}" -o "$obj/${lib}_stub.o"
    "$sdk/bin/prospero-lld" --shared -soname "$lib.prx" -o "$out/stubs/$lib.so" "$obj/${lib}_stub.o"
done

# shellcheck source=/dev/null
source "$vk/tools/radv-link.sh"
PS5_CLANG="$shim/clang" radv_link_recipe "$vk" "$sdk" "$archive" || exit 2
# The platform's getaddrinfo always fails; ps5/app/compat_app.c has a numeric one.
flags=()
for f in "${radv_link_flags[@]}"; do
    case $f in --defsym=getaddrinfo=*|--defsym=freeaddrinfo=*) ;; *) flags+=("$f") ;; esac
done
radv_link_flags=("${flags[@]}")
mapfile -t libs < <(find "$out/llama" -name '*.a' | sort)
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --version-script "$vk/tooling/native/app-symbols.map" --exclude-libs=ALL \
    -e _start -o "$out/llvm-pie.elf" \
    "$obj/app_crt.o" "$obj/app_cpp_runtime.o" "$obj/main.o" \
    --start-group "${libs[@]}" "$obj/libps5app_compat.a" --end-group \
    "$out/stubs/libSceAgc.so" "$out/stubs/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk"/target/lib/*.so
"$tool" link --in "$out/llvm-pie.elf" --out "$out/eboot.elf" \
    --stub-dir "$sdk/target/lib" --stub "$out/stubs/libSceAgc.so" \
    --stub "$out/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf

title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
app="$out/title/$title_id"
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$out/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$param" "$app/sce_sys/param.json"
# The home screen backgrounds (3840x2160 BC7 DDS) are made from the SVG in
# ps5/app/sce_sys/source by PS5_Vulkan's asset tools, and only when it changes.
if [ ! -f "$sce_sys/pic0.dds" ] || [ "$sce_sys/source/background.svg" -nt "$sce_sys/pic0.dds" ]; then
    mkdir -p "$out/assets"
    magick "$sce_sys/source/background.svg" "$out/assets/background.png"
    (cd "$vk" && bash tools/setup-asset-dependencies.sh > /dev/null &&
        bash tools/prepare-assets.sh --icon "$sce_sys/icon0.png" --background "$out/assets/background.png" \
            --output-directory "$out/assets" > /dev/null)
    cp "$out/assets/pic0.dds" "$out/assets/pic1.dds" "$sce_sys/"
fi
cp "$sce_sys/icon0.png" "$sce_sys/pic0.dds" "$sce_sys/pic1.dds" "$app/sce_sys/"
cp "$vk/runtime/libc.prx" "$app/sce_module/libc.prx"
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
echo "build-app: $app ($(stat -c %s "$app/eboot.bin") bytes)"
