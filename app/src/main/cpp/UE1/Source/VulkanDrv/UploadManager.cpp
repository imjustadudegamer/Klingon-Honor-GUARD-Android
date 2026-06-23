
#include "Precomp.h"
#include "UploadManager.h"
#include "UVulkanRenderDevice.h"
#include "CachedTexture.h"

UploadManager::UploadManager(UVulkanRenderDevice* renderer) : renderer(renderer)
{
}

UploadManager::~UploadManager()
{
}

void UploadManager::ClearCache()
{
	PendingUploads.clear();
}

bool UploadManager::SupportsTextureFormat(ETextureFormat Format) const
{
	return TextureUploader::GetUploader(Format);
}

void UploadManager::UploadTexture(CachedTexture* tex, const FTextureInfo& Info, bool masked)
{
	int width = Info.USize;
	int height = Info.VSize;
	int mipcount = Info.NumMips;

	TextureUploader* uploader = TextureUploader::GetUploader(Info.Format);

	// [KHG] FMV frames (UnGame.cpp UE1FMVPresentFrame) wrap 8-bit full-range BGRA video as TEXF_RGB32,
	// which routes to TextureUploader_BGRA8_LM — the 7-bit->8-bit lightmap path that does `<<1` on every
	// channel. Correct for KHG's 7-bit lightmaps, but it corrupts the 8-bit video (bright pixels wrap to
	// dark) => the FMV showed black. Route the two fixed FMV cache slots through the direct, non-doubling
	// BGRA uploader instead (matches the lean device's old direct DrawTile upload). Lightmaps/other RGB32
	// keep the <<1. Slots: "KHGFMV\0\1" content, "KHGFMV\0\2" overlay (see UnGame.cpp).
	if (Info.Format == TEXF_RGB32 &&
		(Info.CacheID == (QWORD)0x4B4847464D560001ULL || Info.CacheID == (QWORD)0x4B4847464D560002ULL))
	{
		// 219 has no TEXF_BGRA8 enum (extended formats are OLDUNREAL469-only), so hold a direct
		// VK_FORMAT_B8G8R8A8_UNORM uploader here: plain memcpy of the BGRA bytes, no channel swap, no <<1.
		static TextureUploader_Simple FMVDirectUploader(VK_FORMAT_B8G8R8A8_UNORM, 4);
		uploader = &FMVDirectUploader;
	}

	if ((uint32_t)Info.USize > renderer->Device.get()->PhysicalDevice.Properties.Properties.limits.maxImageDimension2D ||
		(uint32_t)Info.VSize > renderer->Device.get()->PhysicalDevice.Properties.Properties.limits.maxImageDimension2D ||
		!uploader)
	{
		width = 1;
		height = 1;
		mipcount = 1;
		uploader = nullptr;
	}

	VkFormat format = uploader ? uploader->GetVkFormat() : VK_FORMAT_R8G8B8A8_UNORM;

	// [KHG] Realtime slots (FMV content/overlay 0x..0001/0002, ScriptedTextures) reuse ONE CacheID across
	// clips of DIFFERENT sizes (intro 480x360 -> splash 512x372 -> comm briefings 640x480). The image was
	// created once at the first size (`if (!tex->image)`) and never recreated, so a later, larger frame got
	// copied into the smaller image -> the diagonal-sheared "garbled comm FMV". Recreate when the size or
	// format changes: defer-delete the old image/view (it may still be referenced by an in-flight frame) and
	// invalidate the bindless slots so the new view is re-registered next GetTextureArrayIndex.
	if (tex->image && (tex->image->width != width || tex->image->height != height || tex->imageFormat != format))
	{
		renderer->Commands->GetCurrentDeleteList()->imageViews.push_back(std::move(tex->imageView));
		renderer->Commands->GetCurrentDeleteList()->images.push_back(std::move(tex->image));
		for (int& bi : tex->BindlessIndex)
			bi = -1;
		tex->imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	}

	if (!tex->image)
	{
		tex->image = ImageBuilder()
			.Format(format)
			.Size(width, height, mipcount)
			.Usage(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
			.DebugName("CachedTexture.Image")
			.Create(renderer->Device.get());

		tex->imageView = ImageViewBuilder()
			.Image(tex->image.get(), format)
			.DebugName("CachedTexture.ImageView")
			.Create(renderer->Device.get());

		tex->imageFormat = format;
	}

	tex->pendingUploads[0].clear();
	tex->pendingUploads[1].clear();

	if (uploader)
		UploadData(tex, Info, masked, uploader);
	else
		UploadWhite(tex);
}

void UploadManager::UploadTextureRect(CachedTexture* tex, const FTextureInfo& Info, int x, int y, int w, int h)
{
	TextureUploader* uploader = TextureUploader::GetUploader(Info.Format);
	if (!uploader || Info.NumMips < 1 || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > Info.Mips[0]->USize || y + h > Info.Mips[0]->VSize || !Info.Mips[0]->DataPtr)
		return;

	size_t pixelsSize = uploader->GetUploadSize(x, y, w, h);
	pixelsSize = (pixelsSize + 15) / 16 * 16; // memory alignment

	WaitIfUploadBufferIsFull(pixelsSize);

	uint8_t* data = renderer->Buffers->UploadDataArray[CurrentFrameIndex];
	size_t& UploadBufferPos = renderer->Buffers->UploadBufferPositions[CurrentFrameIndex];
	uint8_t* Ptr = data + UploadBufferPos;
	uploader->UploadRect(Ptr, Info.Mips[0], x, y, w, h, Info.Palette, false);

	VkBufferImageCopy region = {};
	region.bufferOffset = (VkDeviceSize)(Ptr - data);
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.layerCount = 1;
	region.imageOffset = { (int32_t)x, (int32_t)y, 0 };
	region.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };

	AddPendingUpload(tex, region, true);

	UploadBufferPos += pixelsSize;
}

void UploadManager::UploadData(CachedTexture* tex, const FTextureInfo& Info, bool masked, TextureUploader* uploader)
{
	size_t pixelsSize = 0;
	for (INT level = 0; level < Info.NumMips; level++)
	{
		FMipmapBase* Mip = Info.Mips[level];
		if (Mip->DataPtr)
		{
			INT mipsize = uploader->GetUploadSize(0, 0, Mip->USize, Mip->VSize);
			mipsize = (mipsize + 15) / 16 * 16; // memory alignment
			pixelsSize += mipsize;
		}
	}

	WaitIfUploadBufferIsFull(pixelsSize);

	size_t& UploadBufferPos = renderer->Buffers->UploadBufferPositions[CurrentFrameIndex];
	uint8_t* UploadData = renderer->Buffers->UploadDataArray[CurrentFrameIndex];
	for (INT level = 0; level < Info.NumMips; level++)
	{
		FMipmapBase* Mip = Info.Mips[level];
		if (Mip->DataPtr)
		{
			uint32_t mipwidth = Mip->USize;
			uint32_t mipheight = Mip->VSize;

			VkBufferImageCopy region = {};
			region.bufferOffset = UploadBufferPos;
			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.mipLevel = level;
			region.imageSubresource.layerCount = 1;
			region.imageOffset = { 0, 0, 0 };
			region.imageExtent = { mipwidth, mipheight, 1 };
			AddPendingUpload(tex, region, false);

			uploader->UploadRect(UploadData + UploadBufferPos, Mip, 0, 0, Mip->USize, Mip->VSize, Info.Palette, masked);

			INT mipsize = uploader->GetUploadSize(0, 0, Mip->USize, Mip->VSize);
			mipsize = (mipsize + 15) / 16 * 16; // memory alignment
			UploadBufferPos += mipsize;
		}
	}
}

void UploadManager::UploadWhite(CachedTexture* tex)
{
	WaitIfUploadBufferIsFull(16);

	size_t& UploadBufferPos = renderer->Buffers->UploadBufferPositions[CurrentFrameIndex];
	auto data = (uint32_t*)(renderer->Buffers->UploadDataArray[CurrentFrameIndex] + UploadBufferPos);
	data[0] = 0xffffffff;

	VkBufferImageCopy region = {};
	region.bufferOffset = UploadBufferPos;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent = { 1, 1, 1 };
	AddPendingUpload(tex, region, false);

	UploadBufferPos += 16; // 16-byte aligned
}

void UploadManager::WaitIfUploadBufferIsFull(int bytes)
{
	size_t UploadBufferPos = renderer->Buffers->UploadBufferPositions[CurrentFrameIndex];
	if (UploadBufferPos + (size_t)bytes > (size_t)BufferManager::UploadBufferSize)
	{
		renderer->Commands->WaitForTransfer();
	}
}

void UploadManager::AddPendingUpload(CachedTexture* tex, const VkBufferImageCopy& region, bool isPartial)
{
	if (!tex->inPendingUploads)
	{
		PendingUploads.push_back(tex);
		tex->inPendingUploads = true;
	}

	tex->pendingUploads[isPartial].push_back(region);
}

void UploadManager::SubmitUploads()
{
	if (PendingUploads.empty())
		return;

	auto cmdbuffer = renderer->Commands->GetTransferCommands();

	// Transition images to transfer
	PipelineBarrier beforeBarrier;
	for (CachedTexture* tex : PendingUploads)
	{
		beforeBarrier.AddImage(
			tex->image->image,
			tex->imageLayout,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_IMAGE_ASPECT_COLOR_BIT,
			0, tex->image->mipLevels);

		tex->imageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	}
	beforeBarrier.Execute(cmdbuffer, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

	// Do full texture uploads, then partial
	for (int i = 0; i < 2; i++)
	{
		// Copy from buffer to images
		VkBuffer buffer = renderer->Buffers->UploadBuffers[CurrentFrameIndex]->buffer;
		for (CachedTexture* tex : PendingUploads)
		{
			if (!tex->pendingUploads[i].empty())
			{
				if (i == 0)
					renderer->Stats.Uploads++;
				else
					renderer->Stats.RectUploads++;

				cmdbuffer->copyBufferToImage(buffer, tex->image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, tex->pendingUploads[i].size(), tex->pendingUploads[i].data());
			}
		}
	}

	// Transition images to texture sampling
	PipelineBarrier afterBarrier;
	for (CachedTexture* tex : PendingUploads)
	{
		afterBarrier.AddImage(
			tex->image->image,
			tex->imageLayout,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_SHADER_READ_BIT,
			VK_IMAGE_ASPECT_COLOR_BIT,
			0, tex->image->mipLevels);

		tex->imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}
	afterBarrier.Execute(cmdbuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	// Remove textures from pending uploads
	for (CachedTexture* tex : PendingUploads)
	{
		tex->pendingUploads[0].clear();
		tex->pendingUploads[1].clear();
		tex->inPendingUploads = false;
	}
	PendingUploads.clear();
	renderer->Buffers->UploadBufferPositions[CurrentFrameIndex] = 0;
}
