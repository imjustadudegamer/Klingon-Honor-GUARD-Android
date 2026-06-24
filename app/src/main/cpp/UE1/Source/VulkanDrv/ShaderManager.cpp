
#include "Precomp.h"
#include "ShaderManager.h"
#include "UVulkanRenderDevice.h"
#include "SceneShaders.h"   // [KHG] precompiled SPIR-V (glslc, vulkan1.1) — no glslang on Android

// [KHG Phase 4] Build a VulkanShader directly from embedded SPIR-V (no glslang on Android, unlike UT99's runtime GLSL compile): vkCreateShaderModule + wrap.
static std::unique_ptr<VulkanShader> MakeShader(VulkanDevice* device, const uint32_t* words, size_t byteSize, const char* name)
{
	VkShaderModuleCreateInfo ci = {};
	ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	ci.codeSize = byteSize;
	ci.pCode = words;
	VkShaderModule module = VK_NULL_HANDLE;
	VkResult r = vkCreateShaderModule(device->device, &ci, nullptr, &module);
	if (r != VK_SUCCESS)
		throw std::runtime_error("vkCreateShaderModule failed");
	auto sh = std::make_unique<VulkanShader>(device, module);
	sh->SetDebugName(name);
	return sh;
}

ShaderManager::ShaderManager(UVulkanRenderDevice* renderer) : renderer(renderer)
{
	VulkanDevice* dev = renderer->Device.get();

	// Bindless scene shaders = the OpenGL 1:1 math (base*color, *lightmap*2 overbright, macro/detail/fog).
	Scene.VertexShader            = MakeShader(dev, g_SceneVertSpv,       sizeof(g_SceneVertSpv),       "Scene.vert");
	Scene.FragmentShader          = MakeShader(dev, g_SceneFragSpv,       sizeof(g_SceneFragSpv),       "Scene.frag");
	Scene.FragmentShaderAlphaTest = MakeShader(dev, g_SceneFragMaskedSpv, sizeof(g_SceneFragMaskedSpv), "Scene.frag(masked)");

	// Present (fullscreen blit + gamma/dither): single baked variant (D3D9 gamma, colorcorrect mode 0) shared by all 16 slots since the gamma/colour-correct mode matrix isn't exposed.
	Postprocess.VertexShader = MakeShader(dev, g_PPStepVertSpv, sizeof(g_PPStepVertSpv), "PPStep.vert");
	for (int i = 0; i < 16; i++)
		Postprocess.FragmentPresentShader[i] = MakeShader(dev, g_PresentFragSpv, sizeof(g_PresentFragSpv), "Present.frag");

	// Bloom is disabled at runtime (Bloom=0, not 1:1); these shaders exist only so RenderPassManager's bloom pipeline creation compiles — the pass never executes.
	Bloom.Extract        = MakeShader(dev, g_BloomExtractSpv, sizeof(g_BloomExtractSpv), "BloomExtract.frag");
	Bloom.Combine        = MakeShader(dev, g_BloomCombineSpv, sizeof(g_BloomCombineSpv), "BloomCombine.frag");
	Bloom.BlurVertical   = MakeShader(dev, g_BlurVSpv,        sizeof(g_BlurVSpv),        "Blur.frag(V)");
	Bloom.BlurHorizontal = MakeShader(dev, g_BlurHSpv,        sizeof(g_BlurHSpv),        "Blur.frag(H)");
}

ShaderManager::~ShaderManager()
{
}

std::string ShaderManager::LoadShaderCode(const std::string& filename, const std::string& defines)
{
	// [KHG] Unused on Android (shaders are precompiled to SPIR-V). Kept for header-signature compatibility.
	return std::string();
}
