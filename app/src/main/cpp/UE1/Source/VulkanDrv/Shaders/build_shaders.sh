#!/usr/bin/env bash
# Regenerate the embedded SPIR-V headers (TileShaders.h / Mesh3DShaders.h /
# SceneShaders.h) from the GLSL in this dir. Run from the VulkanDrv dir:
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
"$GLSLC" -fshader-stage=vert "$HERE/tile.vert" -o "$OUT/tile.vert.spv"
"$GLSLC" -fshader-stage=frag "$HERE/tile.frag" -o "$OUT/tile.frag.spv"
"$GLSLC" -fshader-stage=frag -DALPHATEST "$HERE/tile.frag" -o "$OUT/tile.frag.masked.spv"
"$GLSLC" -fshader-stage=vert "$HERE/mesh3d.vert" -o "$OUT/m3d.vert.spv"
"$GLSLC" -fshader-stage=frag "$HERE/mesh3d.frag" -o "$OUT/m3d.frag.spv"
"$GLSLC" -fshader-stage=frag -DALPHATEST "$HERE/mesh3d.frag" -o "$OUT/m3d.frag.masked.spv"
# Phase 4: bindless Scene pipeline (UT99 manager architecture). Needs vulkan1.1 for descriptor indexing.
"$GLSLC" --target-env=vulkan1.1 -fshader-stage=vert "$HERE/Scene.vert" -o "$OUT/Scene.vert.spv"
"$GLSLC" --target-env=vulkan1.1 -fshader-stage=frag "$HERE/Scene.frag" -o "$OUT/Scene.frag.spv"
"$GLSLC" --target-env=vulkan1.1 -DALPHATEST -fshader-stage=frag "$HERE/Scene.frag" -o "$OUT/Scene.frag.masked.spv"
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
o=["// [KHG] Auto-generated from Shaders/tile.{vert,frag} via glslc (NDK 27). Phase 2 embedded SPIR-V.",
   "// Regenerate with Shaders/build_shaders.sh after editing the GLSL. Do not hand-edit.",
   "#pragma once","#include <stdint.h>","",
   emit(out_dir+"/tile.vert.spv","g_TileVertSpv"),"",
   emit(out_dir+"/tile.frag.spv","g_TileFragSpv"),"",
   emit(out_dir+"/tile.frag.masked.spv","g_TileFragMaskedSpv"),""]
open(dst+"/TileShaders.h","w").write("\n".join(o))
o2=["// [KHG] Auto-generated from Shaders/mesh3d.{vert,frag} via glslc (NDK 27). Phase 3 embedded SPIR-V.",
   "// Regenerate with Shaders/build_shaders.sh. Do not hand-edit.","#pragma once","#include <stdint.h>","",
   emit(out_dir+"/m3d.vert.spv","g_Mesh3DVertSpv"),"",
   emit(out_dir+"/m3d.frag.spv","g_Mesh3DFragSpv"),"",
   emit(out_dir+"/m3d.frag.masked.spv","g_Mesh3DFragMaskedSpv"),""]
open(dst+"/Mesh3DShaders.h","w").write("\n".join(o2))
o3=["// [KHG] Auto-generated from Shaders/Scene.{vert,frag} via glslc (NDK 27, vulkan1.1). Phase 4 bindless.",
   "// Regenerate with Shaders/build_shaders.sh. Do not hand-edit.","#pragma once","#include <stdint.h>","",
   emit(out_dir+"/Scene.vert.spv","g_SceneVertSpv"),"",
   emit(out_dir+"/Scene.frag.spv","g_SceneFragSpv"),"",
   emit(out_dir+"/Scene.frag.masked.spv","g_SceneFragMaskedSpv"),"",
   emit(out_dir+"/PPStep.vert.spv","g_PPStepVertSpv"),"",
   emit(out_dir+"/Present.frag.spv","g_PresentFragSpv"),"",
   emit(out_dir+"/BloomExtract.frag.spv","g_BloomExtractSpv"),"",
   emit(out_dir+"/BloomCombine.frag.spv","g_BloomCombineSpv"),"",
   emit(out_dir+"/BlurV.frag.spv","g_BlurVSpv"),"",
   emit(out_dir+"/BlurH.frag.spv","g_BlurHSpv"),""]
open(dst+"/SceneShaders.h","w").write("\n".join(o3))
print("wrote TileShaders.h + Mesh3DShaders.h + SceneShaders.h")
PY
rm -rf "$OUT"
