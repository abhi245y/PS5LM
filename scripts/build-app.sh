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
glsdk=${PS5_OPENGL_SDK:-$root/.deps/ps5-opengl/ps5-opengl-sdk-1.0.0/sdk}
sce_sys="$root/ps5/app/sce_sys"
param="$sce_sys/param.json"

for f in "$archive" "$tool" "$vk/runtime/libc.prx" "$sdk/bin/prospero-lld" "$glsdk/lib/libPS5OpenGL.a"; do
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
cxx -std=c++20 -fexceptions -fcxx-exceptions -frtti -I "$headers/include" -I "$llama/ggml/include" -I "$root/third_party/ps5-homebrew-ui/src" \
    -I "${PS5_OPENGL_SDK:-$root/.deps/ps5-opengl/ps5-opengl-sdk-1.0.0/sdk}/include" -DGL_GLEXT_PROTOTYPES=1 \
    -c "$root/ps5/app/main.cpp" -o "$obj/main.o"
cxx -fexceptions -fcxx-exceptions -c "$root/ps5/app/model_plan.cpp" -o "$obj/model_plan.o"
cxx -std=c++20 -fexceptions -fcxx-exceptions -I "$llama/vendor" -c "$root/ps5/app/settings.cpp" -o "$obj/settings.o"
cxx -std=c++20 -fexceptions -fcxx-exceptions -c "$root/ps5/app/fetch.cpp" -o "$obj/fetch.o"
cxx -std=c++20 -fexceptions -fcxx-exceptions -I "$llama/vendor" -c "$root/ps5/app/market.cpp" -o "$obj/market.o"
"$root/ps5/app/ps5cc" -O2 -I "$llama/vendor/hash" -c "$llama/vendor/hash/sha256/sha256.c" -o "$obj/sha256.o"
"$root/ps5/app/ps5cc" -O2 -c "$root/ps5/app/compat_app.c" -o "$obj/compat_app.o"
# In an archive, so --exclude-libs keeps them local: a stub module defines some
# of the same names, and lld would otherwise export ours to interpose them,
# which the title converter refuses.
rm -f "$obj/libps5app_compat.a"
"$sdk/bin/llvm-ar" rcs "$obj/libps5app_compat.a" "$obj/compat_app.o"
cxx -std=c++20 -fno-exceptions -fno-rtti -c "$vk/tooling/native/app_crt.cpp" -o "$obj/app_crt.o"
cxx -std=c++20 -fexceptions -fcxx-exceptions -c "$root/ps5/app/cpp_runtime.cpp" -o "$obj/app_cpp_runtime.o"

# The screen: ps5-opengl (EGL, OpenGL 4.6) is a Mesa of its own, and shares
# thousands of symbol names with RADV's. Linked into one object with only the
# GL and EGL API left global, the two never meet. Rebuilt when the SDK changes.
gl="$out/gl/gl_api.o"
if [ ! -f "$gl" ] || [ "$glsdk/lib/libPS5OpenGL.a" -nt "$gl" ]; then
    mkdir -p "$out/gl"
    gllibs=()
    for l in $(sed -n '/GROUP (/,/)/p' "$glsdk/lib/libPS5OpenGL.a" | grep -oE 'lib[a-z0-9_.]+\.a'); do
        gllibs+=("$glsdk/lib/$l")
    done
    "$sdk/bin/llvm-nm" --defined-only -g "$glsdk/lib/libps5_opengl_core33.a" "$glsdk/lib/libglapi_bridge.a" \
        "$glsdk/lib/libglapi.a" 2>/dev/null | awk 'NF>=3 && $2 ~ /[TW]/ {print $3}' |
        grep -E '^(gl|egl)[A-Z]|^ps5_opengl' | sort -u > "$out/gl/api.txt"
    us=()
    while read -r name; do us+=(-u "$name"); done < "$out/gl/api.txt"
    "$sdk/bin/ld.lld" -r "${us[@]}" -u ps5_agc_gate2_run --start-group "${gllibs[@]}" --end-group \
        -o "$out/gl/gl_all.o"
    "$sdk/bin/llvm-objcopy" --keep-global-symbols="$out/gl/api.txt" "$out/gl/gl_all.o" "$gl"
    rm -f "$out/gl/gl_all.o" "$out/gl/libps5lm_gl.a"
    # In an archive, so --exclude-libs keeps the API local: SDK stub modules
    # define EGL names too, and lld would export ours to interpose them.
    "$sdk/bin/llvm-ar" rcs "$out/gl/libps5lm_gl.a" "$gl"
fi
# The dashboard: ps5-homebrew-ui's kit (gfx, ui, core, audio and its console
# platform layer, without its shell, designs or demo), drawn through ps5-opengl.
kit="$root/third_party/ps5-homebrew-ui"
kitflags=(-std=c++20 -O2 -ffunction-sections -fdata-sections -DGL_GLEXT_PROTOTYPES=1 -I "$kit/src" -I "$kit/third_party"
    -I "$glsdk/include" -I "$root/ps5/app")
mkdir -p "$obj/kit"
mapfile -t kit_sources < <(find "$kit/src/gfx" "$kit/src/ui" "$kit/src/core" "$kit/src/audio" "$kit/src/third_party" \
    "$kit/src/platform/ps5" -type f \( -name '*.cpp' -o -name '*.c' \) ! -name display_egl.cpp | sort)
kit_objects=()
for src in "${kit_sources[@]}"; do
    o="$obj/kit/$(echo "${src#"$kit/src/"}" | tr '/' '_').o"
    kit_objects+=("$o")
    [ -f "$o" ] && [ "$o" -nt "$src" ] && continue
    if [[ $src == *.c ]]; then
        "$root/ps5/app/ps5cc" -std=c11 -O2 -w -I "$kit/src" -c "$src" -o "$o" &
    else
        cxx "${kitflags[@]}" -w -c "$src" -o "$o" &
    fi
    (( $(jobs -r | wc -l) >= $(nproc) )) && wait -n
done
wait
rm -f "$obj/libps5lm_kit.a"
"$sdk/bin/llvm-ar" rcs "$obj/libps5lm_kit.a" "${kit_objects[@]}"
for src in display dashboard stats; do
    cxx -fexceptions -fcxx-exceptions "${kitflags[@]}" -c "$root/ps5/app/$src.cpp" -o "$obj/$src.o"
done

# AGC comes from system modules; these stubs only name the imports, every
# one either driver (the GL object, RADV) makes.
agc_imports=$("$sdk/bin/llvm-nm" -u "$gl" "$archive" 2>/dev/null | awk '{print $NF}' | grep -E '^sceAgc' | sort -u)
for lib in libSceAgc libSceAgcDriver; do
    stub_c="$out/stubs/$lib.c"
    : > "$stub_c"
    for name in $agc_imports; do
        case $lib:$name in
            libSceAgcDriver:sceAgcDriver*) echo "void $name(void) {}" >> "$stub_c" ;;
            libSceAgc:sceAgcDriver*) ;;
            libSceAgc:*) echo "void $name(void) {}" >> "$stub_c" ;;
        esac
    done
    "$root/ps5/app/ps5cc" -O2 -fPIC -c "$stub_c" -o "$obj/${lib}_stub.o"
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
radv_link_flags=("${flags[@]}" --wrap=sceAgcInit)
mapfile -t libs < <(find "$out/llama" -name '*.a' | sort)
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --version-script "$vk/tooling/native/app-symbols.map" --exclude-libs=ALL \
    -e _start -o "$out/llvm-pie.elf" \
    "$obj/app_crt.o" "$obj/app_cpp_runtime.o" "$obj/main.o" "$obj/display.o" "$obj/model_plan.o" "$obj/settings.o" "$obj/fetch.o" "$obj/market.o" "$obj/sha256.o" "$obj/dashboard.o" "$obj/stats.o" \
    --start-group "${libs[@]}" "$obj/libps5lm_kit.a" "$obj/libps5app_compat.a" "$out/gl/libps5lm_gl.a" --end-group \
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
# The dashboard's fonts and interface sounds (ps5-homebrew-ui's, with their licences).
mkdir -p "$app/assets/fonts" "$app/assets/audio"
for font in inter-regular inter-semibold montserrat-medium dejavu-sans-mono; do
    cp "$root/third_party/ps5-homebrew-ui/assets/fonts/$font.huifont" "$app/assets/fonts/"
done
cp "$root/third_party/ps5-homebrew-ui/assets/fonts/"*LICENSE* "$app/assets/fonts/"
cp -r "$root/third_party/ps5-homebrew-ui/assets/audio/sfx" "$app/assets/audio/"
# ps5-exporter (Marice, GPL-3.0): a title may not read the temperature, fan or
# SoC power sensors, a payload may. The app sends it to elfldr when it is not
# running. Pinned by version and hash, downloaded once into .deps.
exporter="$root/.deps/ps5-exporter-0.2.0.elf"
exporter_sha=0d19f7a293eeb30178ba562a3712abfdc50f6b6ebbe180ceaad92dc557948ba8
if [ ! -f "$exporter" ]; then
    curl -sSfL -o "$exporter.part" https://github.com/Marice/ps5-exporter/releases/download/v0.2.0/ps5-exporter.elf
    mv "$exporter.part" "$exporter"
fi
echo "$exporter_sha  $exporter" | sha256sum -c --quiet || { echo "build-app: ps5-exporter hash mismatch" >&2; exit 1; }
mkdir -p "$app/payloads"
cp "$exporter" "$app/payloads/ps5-exporter.elf"
printf '%s\n' "ps5-exporter v0.2.0 by Marice, GPL-3.0-or-later: https://github.com/Marice/ps5-exporter" \
    "Source: https://github.com/Marice/ps5-exporter/tree/v0.2.0" > "$app/payloads/ps5-exporter-NOTICE.txt"
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
echo "build-app: $app ($(stat -c %s "$app/eboot.bin") bytes)"
