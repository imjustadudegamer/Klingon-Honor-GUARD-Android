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

// [KHG Phase 4] UT99-manager build-219 shim: 219 has no FMipmapBase base type, so alias it to FMipmap (same shape).
typedef FMipmap FMipmapBase;
// [KHG Phase 4] 219 Core has no BITFIELD type; reproduce UT99's classic typedef so the managers/device compile.
typedef DWORD BITFIELD;
// [KHG Phase 4] Frames-in-flight + current frame index are UT99-manager globals (defined in VulkanDrv.cpp; advanced each Lock).
constexpr int MAX_FRAMES_IN_FLIGHT = 2;
extern uint32_t CurrentFrameIndex;
