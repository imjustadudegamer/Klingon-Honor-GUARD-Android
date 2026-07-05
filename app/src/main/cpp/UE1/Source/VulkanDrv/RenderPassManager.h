#pragma once

class UVulkanRenderDevice;

struct PipelineState
{
	std::unique_ptr<VulkanPipeline> Pipeline;
	float MinDepth = 0.1f;
	float MaxDepth = 1.0f;
};

class RenderPassManager
{
public:
	RenderPassManager(UVulkanRenderDevice* renderer);
	~RenderPassManager();

	void CreateRenderPass();
	void CreatePipelines();

	void CreatePresentRenderPass();
	void CreatePresentPipeline();
	void CreateScreenshotPipeline();

	void CreatePostprocessRenderPass();
	void CreateBloomPipeline();

	PipelineState* GetPipeline(DWORD polyflags);
	PipelineState* GetEndFlashPipeline();
	PipelineState* GetLinePipeline(bool occludeLines) { return &Scene.LinePipeline[occludeLines]; }
	PipelineState* GetPointPipeline(bool occludeLines) { return &Scene.PointPipeline[occludeLines]; }

	struct
	{
		std::unique_ptr<VulkanPipelineLayout> BindlessPipelineLayout;
		std::unique_ptr<VulkanRenderPass> RenderPass;
		std::unique_ptr<VulkanRenderPass> RenderPassContinue;
		PipelineState Pipeline[32];
		PipelineState LinePipeline[2];
		PipelineState PointPipeline[2];
	} Scene;

	struct
	{
		std::unique_ptr<VulkanPipelineLayout> PipelineLayout;
		std::unique_ptr<VulkanRenderPass> RenderPass;
		std::unique_ptr<VulkanPipeline> Pipeline[16];
		std::unique_ptr<VulkanPipeline> ScreenshotPipeline[16];
	} Present;

	struct
	{
		std::unique_ptr<VulkanPipelineLayout> PipelineLayout;
		std::unique_ptr<VulkanPipeline> Extract;
		std::unique_ptr<VulkanPipeline> Combine;
		std::unique_ptr<VulkanPipeline> Scale;
		std::unique_ptr<VulkanPipeline> BlurVertical;
		std::unique_ptr<VulkanPipeline> BlurHorizontal;
	} Bloom;

	struct
	{
		std::unique_ptr<VulkanRenderPass> RenderPass;
		std::unique_ptr<VulkanRenderPass> RenderPassCombine;
	} Postprocess;

	// [KHG perf] Disk-persisted driver pipeline cache. Feeds every GraphicsPipelineBuilder so the driver
	// reuses previously compiled pipeline ISA across launches (all pipelines are prewarmed at init/resize).
	// Safe across devices/driver updates: the driver validates the embedded header and ignores a mismatch.
	std::unique_ptr<VulkanPipelineCache> PipelineCache;

private:
	void CreateSceneBindlessPipelineLayout();
	void CreatePresentPipelineLayout();
	void CreateBloomPipelineLayout();
	void CreatePipelineCache();     // [KHG perf] create + load the cache from disk
	void SavePipelineCache();       // [KHG perf] write the cache to disk after a prewarm pass

	UVulkanRenderDevice* renderer = nullptr;
};
