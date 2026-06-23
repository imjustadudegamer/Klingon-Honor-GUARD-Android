#pragma once

class CachedTexture
{
public:
	std::unique_ptr<VulkanImage> image;
	std::unique_ptr<VulkanImageView> imageView;
	VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkFormat imageFormat = VK_FORMAT_UNDEFINED;   // [KHG] tracked so realtime slots recreate on size/format change

	int BindlessIndex[4] = { -1, -1, -1, -1 };
	int RealtimeChangeCount = 0;

	std::vector<VkBufferImageCopy> pendingUploads[2];
	bool inPendingUploads = false;
};
