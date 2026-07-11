#!/usr/bin/env bash
# Regenerate the embedded SPIR-V header (SceneShaders.h) from the GLSL in this dir.
# Run from the VulkanDrv dir:
#   ./Shaders/build_shaders.sh
# Requires glslc (ships with the Android NDK under shader-tools/, or Vulkan SDK).
# Override the compiler path with the GLSLC env var, or set ANDROID_NDK_HOME.
set -e
if [ -z "$GLSLC" ] && [ -n "$ANDROID_NDK_HOME" ]; then
  GLSLC="$ANDROID_NDK_HOME/shader-tools/linux-x86_64/glslc"
fi
GLSLC="${GLSLC:-glslc}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(mktemp -d)"
# Bindless Scene pipeline (UT99 manager architecture). Needs vulkan1.1 for descriptor indexing.
"$GLSLC" --target-env=vulkan1.1 -fshader-stage=vert "$HERE/Scene.vert" -o "$OUT/Scene.vert.spv"
"$GLSLC" --target-env=vulkan1.1 -fshader-stage=frag "$HERE/Scene.frag" -o "$OUT/Scene.frag.spv"
"$GLSLC" --target-env=vulkan1.1 -DALPHATEST -fshader-stage=frag "$HERE/Scene.frag" -o "$OUT/Scene.frag.masked.spv"
# Non-bindless COMPATIBILITY Scene pipeline (4 fixed samplers, no descriptor indexing). Target vulkan1.0
# (SPIR-V 1.0) so it loads on GPUs without descriptor indexing, down to Vulkan 1.0.
"$GLSLC" --target-env=vulkan1.0 -fshader-stage=vert "$HERE/SceneCompat.vert" -o "$OUT/SceneCompat.vert.spv"
"$GLSLC" --target-env=vulkan1.0 -fshader-stage=frag "$HERE/SceneCompat.frag" -o "$OUT/SceneCompat.frag.spv"
"$GLSLC" --target-env=vulkan1.0 -DALPHATEST -fshader-stage=frag "$HERE/SceneCompat.frag" -o "$OUT/SceneCompat.frag.masked.spv"
"$GLSLC" --target-env=vulkan1.1 -fshader-stage=vert "$HERE/PPStep.vert" -o "$OUT/PPStep.vert.spv"
"$GLSLC" --target-env=vulkan1.1 -fshader-stage=frag "$HERE/Present.frag" -o "$OUT/Present.frag.spv"
"$GLSLC" --target-env=vulkan1.1 -fshader-stage=frag "$HERE/BloomExtract.frag" -o "$OUT/BloomExtract.frag.spv"
"$GLSLC" --target-env=vulkan1.1 -fshader-stage=frag "$HERE/BloomCombine.frag" -o "$OUT/BloomCombine.frag.spv"
"$GLSLC" --target-env=vulkan1.1 -DBLUR_VERTICAL -fshader-stage=frag "$HERE/Blur.frag" -o "$OUT/BlurV.frag.spv"
"$GLSLC" --target-env=vulkan1.1 -DBLUR_HORIZONTAL -fshader-stage=frag "$HERE/Blur.frag" -o "$OUT/BlurH.frag.spv"
python3 - "$OUT" "$HERE/.." <<'PY'
import struct, sys
out_dir, dst = sys.argv[1], sys.argv[2]
def emit(path, name):
    data = open(path,'rb').read()
    words = struct.unpack('<%dI'%(len(data)//4), data)
    L=["static const uint32_t %s[] = {"%name]
    for i in range(0,len(words),8):
        L.append("    "+", ".join("0x%08x"%w for w in words[i:i+8])+",")
    L.append("};"); return "\n".join(L)
o3=["// [KHG] Auto-generated from Shaders/Scene.{vert,frag} via glslc (NDK 27, vulkan1.1). Phase 4 bindless.",
   "// Regenerate with Shaders/build_shaders.sh. Do not hand-edit.","#pragma once","#include <stdint.h>","",
   emit(out_dir+"/Scene.vert.spv","g_SceneVertSpv"),"",
   emit(out_dir+"/Scene.frag.spv","g_SceneFragSpv"),"",
   emit(out_dir+"/Scene.frag.masked.spv","g_SceneFragMaskedSpv"),"",
   emit(out_dir+"/SceneCompat.vert.spv","g_SceneCompatVertSpv"),"",
   emit(out_dir+"/SceneCompat.frag.spv","g_SceneCompatFragSpv"),"",
   emit(out_dir+"/SceneCompat.frag.masked.spv","g_SceneCompatFragMaskedSpv"),"",
   emit(out_dir+"/PPStep.vert.spv","g_PPStepVertSpv"),"",
   emit(out_dir+"/Present.frag.spv","g_PresentFragSpv"),"",
   emit(out_dir+"/BloomExtract.frag.spv","g_BloomExtractSpv"),"",
   emit(out_dir+"/BloomCombine.frag.spv","g_BloomCombineSpv"),"",
   emit(out_dir+"/BlurV.frag.spv","g_BlurVSpv"),"",
   emit(out_dir+"/BlurH.frag.spv","g_BlurHSpv"),""]
open(dst+"/SceneShaders.h","w").write("\n".join(o3))
print("wrote SceneShaders.h")
PY
rm -rf "$OUT"
