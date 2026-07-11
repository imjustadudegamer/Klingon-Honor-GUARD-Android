
#include "Precomp.h"
#include "DescriptorSetManager.h"
#include "UVulkanRenderDevice.h"
#include "CachedTexture.h"

DescriptorSetManager::DescriptorSetManager(UVulkanRenderDevice* renderer) : renderer(renderer)
{
	// [KHG compat] Only build the descriptor infrastructure for the SELECTED path. On a GPU without
	// descriptor indexing we must NOT create the bindless UpdateAfterBind array at all — build the small
	// fixed 4-binding compatibility layout instead.
	if (renderer->UseBindlessTextures)
		CreateBindlessTextureSet();
	else
		CreateCompatTextureLayout();
	CreatePresentLayout();
	CreatePresentSet();
	CreateBloomLayout();
	CreateBloomSets();
}

DescriptorSetManager::~DescriptorSetManager()
{
}

void DescriptorSetManager::ClearCache()
{
	Textures.WriteBindless = WriteDescriptors();
	Textures.NextBindlessIndex = 0;
	ClearCompatCache();
}

void DescriptorSetManager::CreateCompatTextureLayout()
{
	// Four fixed COMBINED_IMAGE_SAMPLER bindings (0=base, 1=lightmap, 2=detail/fog, 3=macro). No descriptor
	// indexing, no UpdateAfterBind, no variable count — valid on any Vulkan 1.0 device. Matches SceneCompat.frag.
	Compat.Layout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.DebugName("CompatTextureLayout")
		.Create(renderer->Device.get());
	debugf(NAME_Log, "[KHGBoot] compat: created 4-binding texture layout (non-bindless path)");
}

VulkanDescriptorSet* DescriptorSetManager::AllocateCompatSet()
{
	std::unique_ptr<VulkanDescriptorSet> set;
	if (!Compat.Pools.empty())
		set = Compat.Pools.back()->tryAllocate(Compat.Layout.get());
	if (!set)   // no pool yet, or the last pool is exhausted -> grow
	{
		Compat.Pools.push_back(
			DescriptorPoolBuilder()
				.Flags(VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)   // so deferred vkFreeDescriptorSets is legal
				.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, CompatSetsPerPool * 4)
				.MaxSets(CompatSetsPerPool)
				.DebugName("CompatTexturePool")
				.Create(renderer->Device.get()));
		set = Compat.Pools.back()->tryAllocate(Compat.Layout.get());
	}
	if (!set)
		return nullptr;
	VulkanDescriptorSet* raw = set.get();
	Compat.Sets.push_back(std::move(set));
	return raw;
}

VulkanDescriptorSet* DescriptorSetManager::GetCompatTextureSet(DWORD PolyFlags, CachedTexture* tex, CachedTexture* lightmap, CachedTexture* macrotex, CachedTexture* detailtex, bool clamp)
{
	uint32_t samplermode = 0;
	if (PolyFlags & PF_NoSmooth) samplermode |= 1;
	if (clamp) samplermode |= 2;

	// View identity keys the cache. Absent textures use the 1x1 null view (the compat shader still statically
	// references all four bindings, so all must be valid even when the corresponding flag is off).
	VulkanImageView* nullView = renderer->Textures->NullTextureView.get();
	VulkanImageView* baseView = tex       ? tex->imageView.get()       : nullView;
	VulkanImageView* lmView   = lightmap  ? lightmap->imageView.get()  : nullView;
	VulkanImageView* dtView   = detailtex ? detailtex->imageView.get() : nullView;
	VulkanImageView* mcView   = macrotex  ? macrotex->imageView.get()  : nullView;

	CompatTexKey key { baseView, lmView, dtView, mcView, samplermode };
	auto it = Compat.Cache.find(key);
	if (it != Compat.Cache.end())
		return it->second;

	VulkanDescriptorSet* set = AllocateCompatSet();
	if (!set)
		return nullptr;

	// Base samples with the surface's sampler mode; the auxiliary maps use linear-repeat (sampler 0), matching
	// the bindless path where GetTextureIndexes passes PolyFlags only to the base and 0 to the others.
	VulkanSampler* sBase = renderer->Samplers->Samplers[samplermode].get();
	VulkanSampler* s0    = renderer->Samplers->Samplers[0].get();
	WriteDescriptors write;
	write.AddCombinedImageSampler(set, 0, baseView, sBase, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	write.AddCombinedImageSampler(set, 1, lmView,   s0,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	write.AddCombinedImageSampler(set, 2, dtView,   s0,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	write.AddCombinedImageSampler(set, 3, mcView,   s0,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	write.Execute(renderer->Device.get());

	Compat.Cache[key] = set;
	return set;
}

void DescriptorSetManager::ClearCompatCache()
{
	if (Compat.Cache.empty() && Compat.Sets.empty())
		return;
	Compat.Cache.clear();
	// The orphaned sets may still be bound in the command buffer being recorded or a frame in flight. Defer
	// their free to the current frame's delete list, which is destroyed only when this frame index is reused
	// (after its GPU work completed). Freed descriptors return capacity to their pools, which we keep.
	CommandBufferManager::DeleteList* dl = renderer->Commands ? renderer->Commands->GetCurrentDeleteList() : nullptr;
	if (dl)
	{
		for (auto& s : Compat.Sets)
			dl->descriptors.push_back(std::move(s));
	}
	Compat.Sets.clear();
}

int DescriptorSetManager::GetTextureArrayIndex(DWORD PolyFlags, CachedTexture* tex, bool clamp)
{
	if (Textures.NextBindlessIndex >= BindlessCount)
	{
		static bool firstCall = true;
		if (firstCall)
		{
			debugf("============================================================================");
			debugf("VulkanDrv encountered more than %d textures!!!", BindlessCount);
			debugf("============================================================================");
			firstCall = false;
		}
		return 0; // Oh oh, we are out of texture slots
	}

	if (Textures.NextBindlessIndex == 0)
	{
		Textures.WriteBindless.AddCombinedImageSampler(Textures.BindlessSet.get(), 0, 0, renderer->Textures->NullTextureView.get(), renderer->Samplers->Samplers[0].get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		Textures.NextBindlessIndex = 1;
	}

	if (!tex)
		return 0;

	uint32_t samplermode = 0;
	if (PolyFlags & PF_NoSmooth) samplermode |= 1;
	if (clamp) samplermode |= 2;

	int index = tex->BindlessIndex[samplermode];
	if (index != -1)
		return index;

	index = Textures.NextBindlessIndex++;

	VulkanSampler* sampler = renderer->Samplers->Samplers[samplermode].get();
	Textures.WriteBindless.AddCombinedImageSampler(Textures.BindlessSet.get(), 0, index, tex->imageView.get(), sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	tex->BindlessIndex[samplermode] = index;
	return index;
}

void DescriptorSetManager::UpdateBindlessSet()
{
	Textures.WriteBindless.Execute(renderer->Device.get());
	Textures.WriteBindless = WriteDescriptors();
}

void DescriptorSetManager::CreateBindlessTextureSet()
{
	// [KHG Mali fix — Adreno-safe, no-op there] Some older Mali drivers SIGSEGV here inside BringUpVulkan.
	// Two ways this can be invalid usage that an old driver faults on instead of erroring cleanly:
	//   (a) we set the UPDATE_AFTER_BIND flags even when descriptorBindingSampledImageUpdateAfterBind was
	//       NOT enabled at vkCreateDevice (the old bindless check never verified that feature), and
	//   (b) the array is a fixed 16536, which may exceed the device's UpdateAfterBind sampled-image limit.
	// Adreno reports the feature true and limits in the millions, so both guards below leave the Adreno
	// path byte-identical (BindlessCount stays 16536, UAB flags stay on). On a device that lacks/limits
	// UAB we build a legal, correctly-sized set instead of crashing.
	const auto& DIe = renderer->Device->EnabledFeatures.DescriptorIndexing;
	const auto& DIp = renderer->Device->PhysicalDevice.Properties.DescriptorIndexing;
	const auto& Lim = renderer->Device->PhysicalDevice.Properties.Properties.limits;

	bool useUAB = (DIe.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE);

	uint32_t devLimit = useUAB
		? (DIp.maxPerStageDescriptorUpdateAfterBindSampledImages < DIp.maxDescriptorSetUpdateAfterBindSampledImages
			? DIp.maxPerStageDescriptorUpdateAfterBindSampledImages : DIp.maxDescriptorSetUpdateAfterBindSampledImages)
		: Lim.maxPerStageDescriptorSampledImages;
	if (devLimit == 0) devLimit = 1;
	if (devLimit > 8) devLimit -= 8;   // headroom: samplers/other bindings share this budget on some GPUs
	BindlessCount = (devLimit < (uint32_t)MaxBindlessTextures) ? (int)devLimit : MaxBindlessTextures;
	if (BindlessCount < 1) BindlessCount = 1;

	VkDescriptorPoolCreateFlags poolFlags = useUAB ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT : 0;
	VkDescriptorSetLayoutCreateFlags layoutFlags = useUAB ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT_EXT : 0;
	VkDescriptorBindingFlags bindingFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT;
	if (useUAB) bindingFlags |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT_EXT;

	debugf(NAME_Log, "[KHGBoot] bindless: useUAB=%d effectiveCount=%d (ceiling %d, devLimit %u)", (int)useUAB, BindlessCount, (int)MaxBindlessTextures, devLimit);

	debugf(NAME_Log, "[KHGBoot] bindless step A: create pool");
	Textures.BindlessPool = DescriptorPoolBuilder()
		.Flags(poolFlags)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, BindlessCount)
		.MaxSets(BindlessCount)
		.DebugName("TextureBindlessPool")
		.Create(renderer->Device.get());

	debugf(NAME_Log, "[KHGBoot] bindless step B: create layout");
	Textures.BindlessLayout = DescriptorSetLayoutBuilder()
		.Flags(layoutFlags)
		.AddBinding(
			0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			BindlessCount,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			bindingFlags)
		.DebugName("TextureBindlessLayout")
		.Create(renderer->Device.get());

	debugf(NAME_Log, "[KHGBoot] bindless step C: allocate set (variable count %d)", BindlessCount);
	Textures.BindlessSet = Textures.BindlessPool->allocate(Textures.BindlessLayout.get(), (uint32_t)BindlessCount);
	debugf(NAME_Log, "[KHGBoot] bindless: OK");
}

void DescriptorSetManager::CreatePresentLayout()
{
	Present.Layout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.DebugName("PresentLayout")
		.Create(renderer->Device.get());
}

void DescriptorSetManager::CreatePresentSet()
{
	Present.Pool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4)   // 2 bindings x 2 sets (PPImage + ColorBuffer)
		.MaxSets(2)
		.DebugName("PresentPool")
		.Create(renderer->Device.get());
	Present.Set = Present.Pool->allocate(Present.Layout.get());
	Present.SetColorBuffer = Present.Pool->allocate(Present.Layout.get());
}

void DescriptorSetManager::CreateBloomLayout()
{
	Bloom.Layout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.DebugName("BloomLayout")
		.Create(renderer->Device.get());
}

void DescriptorSetManager::CreateBloomSets()
{
	Bloom.Pool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (NumBloomLevels * 2 + 1) * 2)
		.MaxSets(NumBloomLevels * 2 + 1)
		.DebugName("BloomPool")
		.Create(renderer->Device.get());

	for (int level = 0; level < NumBloomLevels; level++)
	{
		Bloom.HTextureSets[level] = Bloom.Pool->allocate(Bloom.Layout.get());
		Bloom.VTextureSets[level] = Bloom.Pool->allocate(Bloom.Layout.get());
	}

	Bloom.PPImageSet = Bloom.Pool->allocate(Bloom.Layout.get());
}

void DescriptorSetManager::UpdateFrameDescriptors()
{
	auto textures = renderer->Textures.get();
	auto samplers = renderer->Samplers.get();

	WriteDescriptors write;
	write.AddCombinedImageSampler(Present.Set.get(), 0, textures->Scene->PPImageView[0].get(), samplers->PPLinearClamp.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	write.AddCombinedImageSampler(Present.Set.get(), 1, textures->DitherImageView.get(), samplers->PPNearestRepeat.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	// [KHG perf] Second present set that samples the scene ColorBuffer directly (blit-skip fast path).
	write.AddCombinedImageSampler(Present.SetColorBuffer.get(), 0, textures->Scene->ColorBufferView.get(), samplers->PPLinearClamp.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	write.AddCombinedImageSampler(Present.SetColorBuffer.get(), 1, textures->DitherImageView.get(), samplers->PPNearestRepeat.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	for (int level = 0; level < NumBloomLevels; level++)
	{
		write.AddCombinedImageSampler(GetBloomHTextureSet(level), 0, textures->Scene->BloomBlurLevels[level].HTextureView.get(), samplers->PPLinearClamp.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		write.AddCombinedImageSampler(GetBloomVTextureSet(level), 0, textures->Scene->BloomBlurLevels[level].VTextureView.get(), samplers->PPLinearClamp.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	write.AddCombinedImageSampler(Bloom.PPImageSet.get(), 0, textures->Scene->PPImageView[0].get(), samplers->PPLinearClamp.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	write.Execute(renderer->Device.get());
}
