// A sample of the picture's pixels, as cheaply as the GPU gives it, for the LEDs' colour (led_light.h).
//
// The finished frame is already in TRANSFER_SRC_OPTIMAL when the window copies it into fb0, so the probe
// reuses it: one NEAREST blit takes every 8th pixel of every 8th row (128x96 from 1024x768) into a small
// image, and a copy brings those into a host buffer. The pixels are sampled, not averaged: a box average
// blends a thin bright line into the dark around it before anyone looks at its colour (a 1-pixel line
// becomes an 8th of itself), so a dim background would outweigh it; a sample keeps every pixel's own colour,
// and its histogram is the frame's in small. The phase moves each frame through all 64 offsets of the 8x8
// grid, so a line that runs exactly along the grid is not always missed (or always hit). The pixels are
// read when the slot comes round again, its fence passed (the window waits on it anyway), so nothing waits
// on the GPU for them.
#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace dream::render::vk {

class Context;

class FrameProbe {
public:
    static constexpr unsigned kSlots = 2;  // the window's frames in flight
    static constexpr unsigned kStep = 8;   // one pixel sampled in each kStep x kStep block

    FrameProbe() = default;
    ~FrameProbe() { destroy(); }
    FrameProbe(const FrameProbe&) = delete;
    FrameProbe& operator=(const FrameProbe&) = delete;

    // False where the device cannot blit into and out of RGBA8 images (the probe is then never ready).
    bool create(const Context& ctx);
    void destroy();
    bool ready() const noexcept { return device_ != VK_NULL_HANDLE; }

    // Records the probe of `src` (`size`, in TRANSFER_SRC_OPTIMAL, its colour writes made visible to
    // transfers) into `cmd`, the slot's command buffer.
    void record(VkCommandBuffer cmd, VkImage src, VkExtent2D size, unsigned slot);
    // The slot's last recorded probe: `width` x `height` RGBA8 pixels (red first in memory), valid until the
    // slot is recorded again. Call once the slot's fence has signalled. False when nothing was recorded since.
    bool read(unsigned slot, const std::uint8_t*& rgba, unsigned& width, unsigned& height);

private:
    struct Slot {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory image_memory = VK_NULL_HANDLE;
        VkExtent2D extent{};  // the sample's
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        bool coherent = true;
        const std::uint8_t* mapped = nullptr;
        VkExtent2D source{};
        bool valid = false;
    };

    bool build(Slot& s, VkExtent2D size);
    void drop(Slot& s);

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    std::uint32_t frame_ = 0;  // moves the sampling phase
    Slot slots_[kSlots];
};

}  // namespace dream::render::vk
