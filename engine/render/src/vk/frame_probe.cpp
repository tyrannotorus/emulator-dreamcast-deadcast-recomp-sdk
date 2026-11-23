// See frame_probe.h.
#include "dream/render/vk/frame_probe.h"

#include <algorithm>
#include <cstdio>

#include "dream/render/vk/context.h"
#include "dream/render/vk/resources.h"

namespace dream::render::vk {

namespace {

constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;  // bytes in memory: R, G, B, A

void layout(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
            VkAccessFlags dst_access) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
                         from == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                           : VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// 0..7 with its 3 bits reversed: consecutive frames sample far-apart phases.
std::uint32_t reversed3(std::uint32_t v) {
    return ((v & 1u) << 2) | (v & 2u) | ((v >> 2) & 1u);
}

}  // namespace

bool FrameProbe::create(const Context& ctx) {
    destroy();
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(ctx.physical_device(), kFormat, &fp);
    constexpr VkFormatFeatureFlags kNeeds = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((fp.optimalTilingFeatures & kNeeds) != kNeeds) {
        std::fprintf(stderr, "frame probe: the device cannot blit RGBA8 images: no colour for the LEDs\n");
        return false;
    }
    device_ = ctx.device();
    physical_ = ctx.physical_device();
    return device_ != VK_NULL_HANDLE;
}

void FrameProbe::drop(Slot& s) {
    if (s.image)
        vkDestroyImage(device_, s.image, nullptr);
    if (s.image_memory)
        vkFreeMemory(device_, s.image_memory, nullptr);
    if (s.mapped)
        vkUnmapMemory(device_, s.memory);
    if (s.buffer)
        vkDestroyBuffer(device_, s.buffer, nullptr);
    if (s.memory)
        vkFreeMemory(device_, s.memory, nullptr);
    s = Slot{};
}

void FrameProbe::destroy() {
    if (!device_)
        return;
    vkDeviceWaitIdle(device_);
    for (Slot& s : slots_) drop(s);
    device_ = VK_NULL_HANDLE;
}

// The slot's sample image and its readback buffer, for a source of this size: one pixel of each kStep x
// kStep block, less one block across and down, so that every phase's blocks lie inside the source.
bool FrameProbe::build(Slot& s, VkExtent2D size) {
    drop(s);
    if (size.width < 2 * kStep || size.height < 2 * kStep)
        return false;
    s.extent = {size.width / kStep - 1, size.height / kStep - 1};
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = kFormat;
    ici.extent = {s.extent.width, s.extent.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &s.image) != VK_SUCCESS) {
        drop(s);
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, s.image, &req);
    std::uint32_t type = find_memory_type(physical_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
        type = find_memory_type(physical_, req.memoryTypeBits, 0);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(device_, &mai, nullptr, &s.image_memory) != VK_SUCCESS ||
        vkBindImageMemory(device_, s.image, s.image_memory, 0) != VK_SUCCESS) {
        drop(s);
        return false;
    }
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = static_cast<VkDeviceSize>(s.extent.width) * s.extent.height * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bci, nullptr, &s.buffer) != VK_SUCCESS) {
        drop(s);
        return false;
    }
    vkGetBufferMemoryRequirements(device_, s.buffer, &req);
    // Cached host memory first: the CPU reads it, and an uncached read is a bus transaction a word.
    constexpr VkMemoryPropertyFlags kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags wants[] = {
        kHost | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        kHost | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, kHost | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    type = UINT32_MAX;
    for (const VkMemoryPropertyFlags w : wants) {
        type = find_memory_type(physical_, req.memoryTypeBits, w);
        if (type != UINT32_MAX) {
            s.coherent = (w & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            break;
        }
    }
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    void* mapped = nullptr;
    if (type == UINT32_MAX || vkAllocateMemory(device_, &mai, nullptr, &s.memory) != VK_SUCCESS ||
        vkBindBufferMemory(device_, s.buffer, s.memory, 0) != VK_SUCCESS ||
        vkMapMemory(device_, s.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        drop(s);
        return false;
    }
    s.mapped = static_cast<const std::uint8_t*>(mapped);
    s.source = size;
    return true;
}

void FrameProbe::record(VkCommandBuffer cmd, VkImage src, VkExtent2D size, unsigned slot) {
    if (!device_ || slot >= kSlots || !size.width || !size.height)
        return;
    Slot& s = slots_[slot];
    s.valid = false;
    // The slot's last frame is finished (the window waited on its fence), so its image and buffer can be
    // rebuilt at once when the picture changed size (a change of resolution).
    if ((!s.image || s.source.width != size.width || s.source.height != size.height) && !build(s, size))
        return;
    // This frame's phase: the sampled pixel's offset in its block, through all 64 in 64 frames. A
    // NEAREST blit at kStep:1 takes the pixel at the middle of each destination pixel's footprint
    // (offset + kStep * x + kStep / 2), so the offset shifts which one.
    const std::uint32_t n = frame_++;
    const auto ox = static_cast<std::int32_t>(reversed3(n & 7u)), oy = static_cast<std::int32_t>(reversed3((n >> 3) & 7u));
    layout(cmd, s.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {ox, oy, 0};
    blit.srcOffsets[1] = {ox + static_cast<std::int32_t>(kStep * s.extent.width),
                          oy + static_cast<std::int32_t>(kStep * s.extent.height), 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[1] = {static_cast<std::int32_t>(s.extent.width), static_cast<std::int32_t>(s.extent.height), 1};
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_NEAREST);
    layout(cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {s.extent.width, s.extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buffer, 1, &copy);
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
    s.valid = true;
}

bool FrameProbe::read(unsigned slot, const std::uint8_t*& rgba, unsigned& width, unsigned& height) {
    if (!device_ || slot >= kSlots || !slots_[slot].valid)
        return false;
    Slot& s = slots_[slot];
    s.valid = false;
    if (!s.coherent) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = s.memory;
        range.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(device_, 1, &range);
    }
    rgba = s.mapped;
    width = s.extent.width;
    height = s.extent.height;
    return true;
}

}  // namespace dream::render::vk
