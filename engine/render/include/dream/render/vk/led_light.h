// The handheld's LEDs lit by the game (NextUI's TrimUI devices; inert where there is no LED driver).
//
// They pulse with the game's vibration, in the picture's colour: a quick rise as a vibration event
// starts, a quick fall as it ends. They follow what the game asks of the motor, so they pulse with the
// player's vibration off too (but not if the game itself asks for none). The colour is the picture's
// dominant vivid hue (dominant_colour), found in the frame probe's sample of each frame's pixels
// (frame_probe.h).
//
//   off   every LED off while the game plays.
//   back  the back LEDs pulse (on the Brick: the top bar and the triggers); the front ones, F1 and F2, stay off.
//   all   every LED pulses.
//
// The LEDs are written as NextUI's own ambient mode writes them: each zone's colour, then its static
// effect, which applies it. A worker thread of its own does the writing and the colour's analysis, so
// the game's threads never wait on either. The driver's state is read before the first write and written
// back when the LEDs are released (the game paused, the run ended): whatever NextUI showed (its own
// colours, or a charging or low-battery indicator) shows again. Everything here is thread safe.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace dream::render::vk {

struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Rgb& o) const noexcept { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const Rgb& o) const noexcept { return !(*this == o); }
};

// The colour an RGBA8 picture (red first in memory) is most vividly: the hue that carries the most
// saturation and brightness (each pixel weighted by both, squared, so a dim background, grey or white
// counts for little against a bright coloured line), as the mean of its pixels, at full value. False
// when the picture has no colour to speak of (black, grey or white), when the last colour should stay.
bool dominant_colour(const std::uint8_t* rgba, std::size_t pixels, Rgb& out) noexcept;

class LedLight {
public:
    // Which LEDs the game lights: none of them (NextUI's own, kReleased), or off, the back ones, or all.
    enum Lit : unsigned { kReleased = 0, kOff = 1, kBack = 2, kAll = 3 };

    LedLight();
    ~LedLight();
    LedLight(const LedLight&) = delete;
    LedLight& operator=(const LedLight&) = delete;

    // True where the device has the LED driver (/sys/class/led_anim).
    bool available() const noexcept;
    // Which LEDs (a Lit), and their brightness, 10 to 100 percent (the driver's max_scale, as NextUI's own
    // brightness setting writes it). Released gives the LEDs back as they were. Takes effect within a frame.
    void configure(unsigned lit, unsigned percent) noexcept;
    // True while the LEDs pulse in the picture's colour (so the probe is worth recording).
    bool wants_colour() const noexcept {
        const unsigned l = lit_.load(std::memory_order_relaxed);
        return l == kBack || l == kAll;
    }
    // The newest sample of the picture: `width` x `height` RGBA8 pixels, copied before this returns.
    void submit_pixels(const std::uint8_t* rgba, unsigned width, unsigned height) noexcept;
    // The game's vibration motor: an event of `ms` milliseconds starts (or is renewed), or ends.
    void rumble_begin(unsigned ms) noexcept;
    void rumble_end() noexcept;
    // Stops the worker and gives the LEDs back. Safe to call twice.
    void shutdown() noexcept;

    // The pulse's envelope, in milliseconds: the rise, the fall, and the least time an event is held at
    // full (so that a short one still shows).
    static constexpr unsigned kRiseMs = 30, kFallMs = 70, kMinHoldMs = 20;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<unsigned> lit_{kReleased};
};

}  // namespace dream::render::vk
