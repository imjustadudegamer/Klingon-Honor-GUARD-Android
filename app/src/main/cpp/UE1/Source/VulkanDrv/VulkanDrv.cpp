/*=============================================================================
	VulkanDrv.cpp: package + class registration for the KHG Android Vulkan driver.
	Produces the ini render-device class string "VulkanDrv.VulkanRenderDevice".
=============================================================================*/

#include "UVulkanRenderDevice.h"

IMPLEMENT_PACKAGE(VulkanDrv);
IMPLEMENT_CLASS(UVulkanRenderDevice);

// [KHG Phase 4] Frame index shared by the UT99 manager classes (per-frame command buffers, vertex
// buffers, upload buffers). Advanced by the device once per Lock. Declared extern in Precomp.h.
uint32_t CurrentFrameIndex = 0;
