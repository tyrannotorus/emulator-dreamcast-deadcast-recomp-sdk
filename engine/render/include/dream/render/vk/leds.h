// The handheld's LEDs (NextUI's TrimUI devices, /sys/class/led_anim; inert where there is no LED driver), for a
// port that lights them itself (dream::System::leds). Only the device is here: what to show, and when, is the
// port's.
//
// The LEDs are written as NextUI's own ambient mode writes them: each zone's colour, then its static effect,
// which applies it. The driver's state is read when they are taken and written back when they are released:
// whatever NextUI showed (its own colours, or a charging or low-battery indicator) shows again. While the host
// holds them (its menu over the game) they are NextUI's; the port's last show comes back when it lets go.
// Everything here is thread safe.
#pragma once

#include <cstdint>
#include <memory>

namespace dream::render::vk {

class Leds {
public:
    Leds();
    ~Leds();
    Leds(const Leds&) = delete;
    Leds& operator=(const Leds&) = delete;

    // True where the device has the LED driver.
    bool available() const noexcept;
    // `rgb` (0xRRGGBB) on every zone, or on the back ones only (on the Brick the top bar and the triggers; the
    // front ones, F1 and F2, off), at `percent` brightness (10-100: the driver's max_scale, as NextUI's own
    // brightness setting writes it). The first show takes the LEDs over. False when nothing is shown: while the
    // host holds them (this show is then the one shown when it lets go), once closed, or where there are none.
    bool show(std::uint32_t rgb, bool back_only, unsigned percent) noexcept;
    // The LEDs given back as NextUI had them, until the next show.
    void release() noexcept;
    // The host's menu over the game: the LEDs NextUI's while `held`, the port's last show again after.
    void hold(bool held) noexcept;
    // The run's end: the LEDs given back for good, every later show ignored.
    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dream::render::vk
