/*=============================================================================
	Precomp.h: VulkanDrv Android/NDK include glue.

	Include ZVulkan (Vulkan + std) BEFORE the UE1 engine headers so UE1's macros/
	typedefs (DWORD/BYTE/check/...) don't clash with Vulkan. This is build-219 UE1
	(KHG), so OLDUNREAL469SDK / UNREALGOLD are deliberately NOT defined.
=============================================================================*/
#pragma once

#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkaninstance.h>
#include <zvulkan/vulkansurface.h>
#include <zvulkan/vulkanswapchain.h>
#include <zvulkan/vulkanobjects.h>
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkancompatibledevice.h>

#include <memory>
#include <vector>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <mutex>
#include <cstdint>

// UE1 engine render-device interface + scene structs (same header the GLES driver uses).
#include "RenderPrivate.h"

// [KHG Phase 4] UT99-manager compatibility shims for build-219:
//  - 219's mipmap class is FMipmap (no FMipmapBase base type as in the 469 SDK). The managers refer to
//    FMipmapBase*; alias it. FTextureInfo::Mips is FMipmap*[] in 219, same shape.
typedef FMipmap FMipmapBase;
//  - 219 Core has no BITFIELD type (only NEXT_BITFIELD/FIRST_BITFIELD macros). The UT99 managers + the
//    device declare config flags as `BITFIELD`. Reproduce UT99's classic typedef so they compile.
typedef DWORD BITFIELD;
//  - Frames-in-flight + the current frame index are globals in the UT99 managers (469 Precomp had them).
//    Defined in VulkanDrv.cpp; the device advances CurrentFrameIndex each Lock.
constexpr int MAX_FRAMES_IN_FLIGHT = 2;
extern uint32_t CurrentFrameIndex;
