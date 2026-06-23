
#include "Precomp.h"
#include "CommandBufferManager.h"
#include "UVulkanRenderDevice.h"
#include <android/log.h>
#define VKT(...) ((void)0)   // [KHG] diagnostic logging pulled (was KHG_VKT per-submit spam)

CommandBufferManager::CommandBufferManager(UVulkanRenderDevice* renderer) : renderer(renderer)
{
	SwapChain = VulkanSwapChainBuilder()
		.Create(renderer->Device.get());

	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
	{
		ImageAvailableSemaphores[i] = SemaphoreBuilder()
			.DebugName("ImageAvailableSemaphore")
			.Create(renderer->Device.get());

		RenderFinishedSemaphores[i] = SemaphoreBuilder()
			.DebugName("RenderFinishedSemaphore")
			.Create(renderer->Device.get());

		DrawFinishedSemaphores[i] = SemaphoreBuilder()
			.DebugName("DrawFinishedSemaphores")
			.Create(renderer->Device.get());

		TransferSemaphores[i] = SemaphoreBuilder()
			.DebugName("TransferSemaphore")
			.Create(renderer->Device.get());

		RenderFinishedFences[i] = FenceBuilder()
			.DebugName("RenderFinishedFence")
			.Flags(VK_FENCE_CREATE_SIGNALED_BIT)
			.Create(renderer->Device.get());

		FrameDeleteLists[i] = std::make_unique<DeleteList>();
	}

	// [KHG/Adreno] Per-frame command buffers are re-recorded every frame via begin() (implicit reset),
	// which needs VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT or Adreno faults. Verified: ZVulkan's
	// VulkanCommandPool ctor (vulkanobjects.h) already sets TRANSIENT_BIT | RESET_COMMAND_BUFFER_BIT.
	CommandPool = CommandPoolBuilder()
		.QueueFamily(renderer->Device.get()->GraphicsFamily)
		.DebugName("CommandPool")
		.Create(renderer->Device.get());
}

CommandBufferManager::~CommandBufferManager()
{
	DeleteFrameObjects();
}

void CommandBufferManager::BeginFrame()
{
	VkFence currentFence = RenderFinishedFences[CurrentFrameIndex]->fence;
	vkWaitForFences(renderer->Device.get()->device, 1, &currentFence, VK_TRUE, std::numeric_limits<uint64_t>::max());
	vkResetFences(renderer->Device.get()->device, 1, &currentFence);

	// Safely clear old Vulkan objects now that the GPU is 100% done with this frame index
	FrameDeleteLists[CurrentFrameIndex] = std::make_unique<DeleteList>();

	// Reset per-frame CPU write positions now that this frame index is safe to reuse
	renderer->Buffers->UploadBufferPositions[CurrentFrameIndex] = 0;

}

void CommandBufferManager::WaitForTransfer()
{
	renderer->Uploads->SubmitUploads();

	auto& TransferCommands = TransferCommandsArray[CurrentFrameIndex];
	auto& RenderFinishedFence = RenderFinishedFences[CurrentFrameIndex];

	if (TransferCommands)
	{
		TransferCommands->end();

		QueueSubmit()
			.AddCommandBuffer(TransferCommands.get())
			.Execute(renderer->Device.get(), renderer->Device.get()->GraphicsQueue, RenderFinishedFence.get());
		vkWaitForFences(renderer->Device.get()->device, 1, &RenderFinishedFence->fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
		vkResetFences(renderer->Device.get()->device, 1, &RenderFinishedFence->fence);

		TransferCommands->begin();
	}
}

void CommandBufferManager::SubmitCommands(bool present, int presentWidth, int presentHeight, bool presentFullscreen)
{
	renderer->Uploads->SubmitUploads();

	auto& ImageAvailableSemaphore = ImageAvailableSemaphores[CurrentFrameIndex];
	auto& RenderFinishedSemaphore = RenderFinishedSemaphores[CurrentFrameIndex];
	auto& TransferSemaphore = TransferSemaphores[CurrentFrameIndex];
	auto& RenderFinishedFence = RenderFinishedFences[CurrentFrameIndex];
	auto& DrawCommands = DrawCommandsArray[CurrentFrameIndex];
	auto& TransferCommands = TransferCommandsArray[CurrentFrameIndex];

	static int scTrace = 80;
	bool trace = (scTrace > 0);
	if (trace) { scTrace--; VKT("SubmitCommands: present=%d lost=%d %dx%d", present?1:0, SwapChain->Lost()?1:0, presentWidth, presentHeight); }

	if (present)
	{
		if (SwapChain->Lost() || SwapChain->Width() != presentWidth || SwapChain->Height() != presentHeight || UsingVsync != renderer->UseVSync || UsingHdr != renderer->Hdr)
		{
			if (trace) VKT("SubmitCommands: (re)creating swapchain + framebuffers");
			UsingVsync = renderer->UseVSync;
			UsingHdr = renderer->Hdr;
			renderer->Framebuffers->DestroySwapChainFramebuffers();
			SwapChain->Create(presentWidth, presentHeight, renderer->UseVSync ? 2 : 3, renderer->UseVSync, renderer->Hdr, renderer->VkExclusiveFullscreen && presentFullscreen);
			renderer->Framebuffers->CreateSwapChainFramebuffers();
			if (trace) VKT("SubmitCommands: swapchain+framebuffers created (images=%d)", SwapChain->ImageCount());
		}

		PresentImageIndex = SwapChain->AcquireImage(ImageAvailableSemaphore.get());
		if (trace) VKT("SubmitCommands: AcquireImage -> %d", PresentImageIndex);
		if (PresentImageIndex != -1)
		{
			renderer->DrawPresentTexture(presentWidth, presentHeight);
			if (trace) VKT("SubmitCommands: DrawPresentTexture returned");
		}
	}

	// [KHG/Adreno] DrawFinishedSemaphores[CurrentFrameIndex] is signalled EVERY frame, but the upstream
	// UT99 code only WAITS it from the next frame's transfer submit. On idle frames SubmitUploads early-
	// outs (no PendingUploads) so TransferCommands stays null and no transfer submit runs — the binary
	// semaphore is then never waited, and the next reuse double-signals a still-pending semaphore. That
	// is illegal and faults the Adreno driver inside vkQueueSubmit (the desktop driver tolerates it).
	// Fix: wait DrawFinishedSemaphores[PrevFrame] exactly once per frame — by the transfer submit when it
	// exists (preserves the upload-after-prev-draw hazard guard), otherwise by the main submit.
	const uint32_t PrevFrame = (CurrentFrameIndex + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
	bool prevDrawWaited = false;

	// [KHG/Adreno] TransferCommandsArray[idx] is a PERSISTENT unique_ptr — it is NOT reset after submit.
	// On a frame with no uploads, SubmitUploads early-outs so the buffer is never begin()'d this frame,
	// yet it stays non-null (executable state) from an earlier frame. Guarding the end()+submit on the
	// pointer (as upstream does) then calls vkEndCommandBuffer on a non-recording buffer — undefined, and
	// it faults the Adreno driver. Gate on whether a transfer was actually recorded THIS frame instead.
	const bool didTransfer = TransferCommands && TransferCommandsBegun[CurrentFrameIndex];

	if (trace) VKT("SubmitCommands: TransferCommands=%p begunThisFrame=%d", (void*)TransferCommands.get(), didTransfer?1:0);
	if (didTransfer)
	{
		if (trace) VKT("SubmitCommands: transfer end() begin");
		TransferCommands->end();
		if (trace) VKT("SubmitCommands: transfer end() done");

		auto SubmitTransfer = QueueSubmit();
		SubmitTransfer.AddCommandBuffer(TransferCommands.get());
		SubmitTransfer.AddSignal(TransferSemaphore.get());

		if (!IsFirstFrame)
		{
			SubmitTransfer.AddWait(VK_PIPELINE_STAGE_TRANSFER_BIT, DrawFinishedSemaphores[PrevFrame].get());
			prevDrawWaited = true;
		}

		if (trace) VKT("SubmitCommands: transfer vkQueueSubmit begin (wait DFS[%u]=%d)", PrevFrame, IsFirstFrame?0:1);
		SubmitTransfer.Execute(renderer->Device.get(), renderer->Device.get()->GraphicsQueue);
		if (trace) VKT("SubmitCommands: transfer vkQueueSubmit done");
	}

	if (trace) VKT("SubmitCommands: transfer block done (prevDrawWaited=%d)", prevDrawWaited?1:0);

	if (DrawCommands)
		DrawCommands->end();
	if (trace) VKT("SubmitCommands: DrawCommands->end() done; about to vkQueueSubmit");

	QueueSubmit submit;
	if (DrawCommands)
	{
		submit.AddCommandBuffer(DrawCommands.get());
	}
	if (didTransfer)
	{
		submit.AddWait(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, TransferSemaphore.get());
	}
	if (!IsFirstFrame && !prevDrawWaited)
	{
		// No transfer this frame consumed the previous frame's draw-finished signal — consume it here so
		// the binary semaphore is paired 1:1 (signalled once, waited once) and never double-signalled.
		submit.AddWait(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, DrawFinishedSemaphores[PrevFrame].get());
	}
	if (present && PresentImageIndex != -1)
	{
		submit.AddWait(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, ImageAvailableSemaphore.get());
		submit.AddSignal(RenderFinishedSemaphore.get());
	}
	submit.AddSignal(DrawFinishedSemaphores[CurrentFrameIndex].get());
	submit.Execute(renderer->Device.get(), renderer->Device.get()->GraphicsQueue, RenderFinishedFence.get());

	FrameBegun = false;
	IsFirstFrame = false;

	if (trace) VKT("SubmitCommands: queue submit done");

	if (present && PresentImageIndex != -1)
	{
		SwapChain->QueuePresent(PresentImageIndex, RenderFinishedSemaphore.get());
		if (trace) VKT("SubmitCommands: QueuePresent done");
	}

	// Advance frame index. NO vkWaitForFences here!
	CurrentFrameIndex = (CurrentFrameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
}

VulkanCommandBuffer* CommandBufferManager::GetTransferCommands()
{
	if (!FrameBegun)
	{
		BeginFrame();
		DrawCommandsBegun[CurrentFrameIndex] = false;
		TransferCommandsBegun[CurrentFrameIndex] = false;
		FrameBegun = true;
	}

	auto& TransferCommands = TransferCommandsArray[CurrentFrameIndex];
	if (!TransferCommands)
		TransferCommands = CommandPool->createBuffer();

	if (!TransferCommandsBegun[CurrentFrameIndex])
	{
		TransferCommands->begin();
		TransferCommandsBegun[CurrentFrameIndex] = true;
	}
	return TransferCommands.get();
}

VulkanCommandBuffer* CommandBufferManager::GetDrawCommands()
{
	if (!FrameBegun)
	{
		BeginFrame();
		DrawCommandsBegun[CurrentFrameIndex] = false;
		TransferCommandsBegun[CurrentFrameIndex] = false;
		FrameBegun = true;
	}

	auto& DrawCommands = DrawCommandsArray[CurrentFrameIndex];
	if (!DrawCommands)
		DrawCommands = CommandPool->createBuffer();

	if (!DrawCommandsBegun[CurrentFrameIndex])
	{
		DrawCommands->begin();
		DrawCommandsBegun[CurrentFrameIndex] = true;
	}
	return DrawCommands.get();
}

void CommandBufferManager::DeleteFrameObjects()
{
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		FrameDeleteLists[i] = std::make_unique<DeleteList>();
}
