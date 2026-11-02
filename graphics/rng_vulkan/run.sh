#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ "$(uname -s)" = Darwin ] && command -v brew >/dev/null 2>&1; then
    if [ -z "${VK_DRIVER_FILES:-}" ] && [ -z "${VK_ICD_FILENAMES:-}" ]; then
        molten_prefix=$(brew --prefix molten-vk 2>/dev/null || true)
        if [ -f "$molten_prefix/etc/vulkan/icd.d/MoltenVK_icd.json" ]; then
            export VK_DRIVER_FILES="$molten_prefix/etc/vulkan/icd.d/MoltenVK_icd.json"
        fi
    fi
    if [ -z "${VK_LAYER_PATH:-}" ]; then
        layer_prefix=$(brew --prefix vulkan-validationlayers 2>/dev/null || true)
        if [ -d "$layer_prefix/share/vulkan/explicit_layer.d" ]; then
            export VK_LAYER_PATH="$layer_prefix/share/vulkan/explicit_layer.d"
        fi
    fi
fi
build_dir=${RNG_BUILD_DIR:-"$script_dir/build"}
executable="$build_dir/rng_vulkan"
if [ ! -x "$executable" ] && [ -x "$build_dir/Release/rng_vulkan.exe" ]; then
    executable="$build_dir/Release/rng_vulkan.exe"
fi
if [ ! -x "$executable" ]; then
    echo "Build first: cmake -S \"$script_dir\" -B \"$build_dir\" -DCMAKE_BUILD_TYPE=Release" >&2
    echo "Then: cmake --build \"$build_dir\" --parallel" >&2
    exit 1
fi
exec "$executable" "$@"
