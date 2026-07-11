#pragma once

#include "SceneTextures.h"
#include <unordered_map>
#include <vector>

class UVulkanRenderDevice;
class CachedTexture;
class VulkanImageView;

// [KHG compat] Key for a non-bindless per-draw texture descriptor set: the four image VIEWS (base, lightmap,
// detail/fog, macro) plus the sampler mode. Keyed on the VulkanImageView* (resource identity) — NOT the
// CachedTexture* — so a size/format re-upload that recreates the view (UploadManager) naturally yields a
// fresh set instead of a dangling one.
struct CompatTexKey
{
	VulkanImageView* base;
	VulkanImageView* lightmap;
	VulkanImageView* detail;
	VulkanImageView* macro;
	uint32_t sampler;

	bool operator==(const CompatTexKey& o) const
	{
		return base == o.base && lightmap == o.lightmap && detail == o.detail && macro == o.macro && sampler == o.sampler;
	}
};

template<> struct std::hash<CompatTexKey>
{
	std::size_t operator()(const CompatTexKey& k) const
	{
		std::size_t h = (std::size_t)k.base;
		h = h * 1099511628211ULL ^ (std::size_t)k.lightmap;
		h = h * 1099511628211ULL ^ (std::size_t)k.detail;
		h = h * 1099511628211ULL ^ (std::size_t)k.macro;
		h = h * 1099511628211ULL ^ (std::size_t)k.sampler;
		return h;
	}
};

struct TexDescriptorKey
{
	TexDescriptorKey(CachedTexture* tex, CachedTexture* lightmap, CachedTexture* detailtex, CachedTexture* macrotex, uint32_t sampler) : tex(tex), lightmap(lightmap), detailtex(detailtex), macrotex(macrotex), sampler(sampler) { }

	bool operator==(const TexDescriptorKey& other) const
	{
		return tex == other.tex && lightmap == other.lightmap && detailtex == other.detailtex && macrotex == other.macrotex && sampler == other.sampler;
	}

	bool operator<(const TexDescriptorKey& other) const
	{
		if (tex != other.tex)
			return tex < other.tex;
		else if (lightmap != other.lightmap)
			return lightmap < other.lightmap;
		else if (detailtex != other.detailtex)
			return detailtex < other.detailtex;
		else if (macrotex != other.macrotex)
			return macrotex < other.macrotex;
		else
			return sampler < other.sampler;
	}

	CachedTexture* tex;
	CachedTexture* lightmap;
	CachedTexture* detailtex;
	CachedTexture* macrotex;
	uint32_t sampler;
};

template<> struct std::hash<TexDescriptorKey>
{
	std::size_t operator()(const TexDescriptorKey& k) const
	{
		return (((std::size_t)k.tex ^ (std::size_t)k.lightmap));
	}
};

class DescriptorSetManager
{
public:
	DescriptorSetManager(UVulkanRenderDevice* renderer);
	~DescriptorSetManager();

	void ClearCache();

	bool IsTextureArrayFull() const { return Textures.NextBindlessIndex + 4 > BindlessCount; }
	int GetTextureArrayIndex(DWORD PolyFlags, CachedTexture* tex, bool clamp = false);

	// [KHG compat] Non-bindless path: resolve up to four textures to a cached per-combo descriptor set
	// (4 fixed COMBINED_IMAGE_SAMPLER bindings). Returns nullptr only on catastrophic allocation failure.
	VulkanDescriptorSet* GetCompatTextureSet(DWORD PolyFlags, CachedTexture* tex, CachedTexture* lightmap, CachedTexture* macrotex, CachedTexture* detailtex, bool clamp);
	VulkanDescriptorSetLayout* GetCompatTextureLayout() { return Compat.Layout.get(); }
	// Drop the combo->set cache (map only; never frees GPU sets mid-frame). Safe to call any time — e.g.
	// when a texture view is recreated. The orphaned sets are reclaimed at teardown / on a rare hard reset.
	void ClearCompatCache();

	VulkanDescriptorSet* GetBindlessSet() { return Textures.BindlessSet.get(); }
	VulkanDescriptorSet* GetPresentSet() { return Present.Set.get(); }
	// [KHG perf] Present set that samples the scene ColorBuffer directly (fast path that skips the
	// ColorBuffer->PPImage blit when there is no MSAA resolve and no bloom). See BlitSceneToPostprocess.
	VulkanDescriptorSet* GetPresentSetColorBuffer() { return Present.SetColorBuffer.get(); }
	VulkanDescriptorSet* GetBloomPPImageSet() { return Bloom.PPImageSet.get(); }
	VulkanDescriptorSet* GetBloomVTextureSet(int level) { return Bloom.VTextureSets[level].get(); }
	VulkanDescriptorSet* GetBloomHTextureSet(int level) { return Bloom.HTextureSets[level].get(); }

	void UpdateBindlessSet();
	void UpdateFrameDescriptors();

	static const int MaxBindlessTextures = 16536;
	// [KHG Mali fix] Actual size of the bindless array we create — clamped at bring-up to what the
	// device's descriptor-indexing limits allow (== MaxBindlessTextures on Adreno; possibly smaller on
	// old Mali). Used for the pool/layout/allocate AND the "array full" guard so we never overrun it.
	int BindlessCount = MaxBindlessTextures;

	VulkanDescriptorSetLayout* GetTextureBindlessLayout() { return Textures.BindlessLayout.get(); }
	VulkanDescriptorSetLayout* GetPresentLayout() { return Present.Layout.get(); }
	VulkanDescriptorSetLayout* GetBloomLayout() { return Bloom.Layout.get(); }

private:
	void CreateBindlessTextureSet();
	void CreateCompatTextureLayout();
	void CreatePresentLayout();
	void CreatePresentSet();
	void CreateBloomLayout();
	void CreateBloomSets();

	UVulkanRenderDevice* renderer = nullptr;

	struct
	{
		std::unique_ptr<VulkanDescriptorSetLayout> BindlessLayout;
		std::unique_ptr<VulkanDescriptorPool> BindlessPool;
		std::unique_ptr<VulkanDescriptorSet> BindlessSet;
		WriteDescriptors WriteBindless;
		int NextBindlessIndex = 0;

	} Textures;

	// [KHG compat] Non-bindless texture descriptor sets (built only when !renderer->UseBindlessTextures).
	// Declaration order matters: Pools before Sets, so at teardown the Sets (owning unique_ptrs) destruct
	// FIRST — vkFreeDescriptorSets runs while the owning Pool is still alive — then the Pools destruct.
	static const int CompatSetsPerPool = 1024;
	struct
	{
		std::unique_ptr<VulkanDescriptorSetLayout> Layout;
		std::vector<std::unique_ptr<VulkanDescriptorPool>> Pools;
		std::vector<std::unique_ptr<VulkanDescriptorSet>> Sets;   // owns the per-combo sets
		std::unordered_map<CompatTexKey, VulkanDescriptorSet*> Cache;
	} Compat;

	VulkanDescriptorSet* AllocateCompatSet();

	struct
	{
		std::unique_ptr<VulkanDescriptorSetLayout> Layout;
		std::unique_ptr<VulkanDescriptorPool> Pool;
		std::unique_ptr<VulkanDescriptorSet> Set;
		std::unique_ptr<VulkanDescriptorSet> SetColorBuffer;   // [KHG perf] samples ColorBuffer directly (blit-skip fast path)
	} Present;

	struct
	{
		std::unique_ptr<VulkanDescriptorSetLayout> Layout;
		std::unique_ptr<VulkanDescriptorPool> Pool;
		std::unique_ptr<VulkanDescriptorSet> VTextureSets[NumBloomLevels];
		std::unique_ptr<VulkanDescriptorSet> HTextureSets[NumBloomLevels];
		std::unique_ptr<VulkanDescriptorSet> PPImageSet;
	} Bloom;
};
