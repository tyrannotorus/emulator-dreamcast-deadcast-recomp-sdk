// See leds.h.
#include "dream/render/vk/leds.h"

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
#endif

namespace dream::render::vk {

#if defined(__linux__)

namespace {

constexpr const char* kDir = "/sys/class/led_anim/";
constexpr const char* kStatic = "4";  // the driver's effect_names: 4 - static

// One attribute of the driver, kept open: written and read at offset 0, as sysfs wants.
class Attr {
public:
    explicit Attr(std::string name) : name_(std::move(name)) {
        fd_ = open((kDir + name_).c_str(), O_RDWR | O_CLOEXEC);
    }
    ~Attr() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    Attr(const Attr&) = delete;
    Attr& operator=(const Attr&) = delete;
    Attr(Attr&& o) noexcept : name_(std::move(o.name_)), fd_(o.fd_) { o.fd_ = -1; }

    bool ok() const noexcept { return fd_ >= 0; }
    // The value as the driver shows it, without its trailing spaces and newline.
    std::string read() const {
        char text[64];
        const ssize_t n = fd_ >= 0 ? pread(fd_, text, sizeof text - 1, 0) : -1;
        std::string s(text, n > 0 ? static_cast<std::size_t>(n) : 0u);
        while (!s.empty() && (s.back() == '\n' || s.back() == ' '))
            s.pop_back();
        return s;
    }
    bool write(const std::string& value) const {
        const std::string text = value + "\n";
        return fd_ >= 0 && pwrite(fd_, text.data(), text.size(), 0) == static_cast<ssize_t>(text.size());
    }

private:
    std::string name_;
    int fd_ = -1;
};

// A zone's attributes. The colour applies once the effect is written, which is why NextUI writes the
// effect last; so does everything here.
struct Zone {
    Attr colour, effect, duration, cycles;
    bool front;  // on the face of the device (the Brick's F1 and F2): off in a back-only show
    std::string saved_colour, saved_effect, saved_duration, saved_cycles;
    explicit Zone(const std::string& z)
        : colour("effect_rgb_hex_" + z), effect("effect_" + z), duration("effect_duration_" + z),
          cycles("effect_cycles_" + z), front(z == "f1" || z == "f2") {}
    bool ok() const noexcept { return colour.ok() && effect.ok() && duration.ok() && cycles.ok(); }
};

// The zones and brightness attributes NextUI drives on this device (its PLAT_initDefaultLeds and
// PLAT_setLedBrightness, by the DEVICE its launcher exports): the Brick's top bar, F1, F2 and triggers.
void device_layout(std::vector<std::string>& zones, std::vector<std::string>& brightness) {
    const char* d = std::getenv("DEVICE");
    const std::string device = d ? d : "brick";
    if (device == "brick" || device == "brickpro") {
        zones = {"m", "f1", "f2", "lr"};
        brightness = {"max_scale", "max_scale_f1f2", "max_scale_lr"};
        if (device == "brickpro") {
            zones.push_back("rear");
            brightness.push_back("max_scale_rear");
        }
    } else {
        zones = {"m", "l", "r"};
        brightness = {"max_scale"};
    }
}

std::string hex(std::uint32_t rgb) {
    char s[8];
    std::snprintf(s, sizeof s, "%06X", rgb & 0xFFFFFFu);
    return s;
}

class Driver {
public:
    Driver() {
        std::vector<std::string> zones, brightness;
        device_layout(zones, brightness);
        for (const std::string& z : zones) {
            Zone zone(z);
            if (zone.ok())
                zones_.push_back(std::move(zone));
        }
        for (const std::string& b : brightness) {
            Attr a(b);
            if (a.ok())
                brightness_.push_back(std::move(a));
        }
        saved_brightness_.resize(brightness_.size());
    }

    bool usable() const noexcept { return !zones_.empty(); }
    bool held() const noexcept { return held_; }

    // The driver's state read, then every zone static, black, with no transition, at `percent`.
    void take_over(unsigned percent) {
        for (Zone& z : zones_) {
            z.saved_colour = z.colour.read();
            z.saved_effect = z.effect.read();
            z.saved_duration = z.duration.read();
            z.saved_cycles = z.cycles.read();
        }
        for (std::size_t i = 0; i < brightness_.size(); ++i) saved_brightness_[i] = brightness_[i].read();
        set_brightness(percent);
        for (Zone& z : zones_) {
            z.cycles.write("-1");
            z.duration.write("0");
            z.colour.write("000000");
            z.effect.write(kStatic);
        }
        held_ = true;
        std::printf("leds: the port's (%zu zones, brightness %u%%)\n", zones_.size(), percent);
        std::fflush(stdout);
    }

    void set_brightness(unsigned percent) {
        const std::string v = std::to_string(percent);
        for (const Attr& a : brightness_) a.write(v);
    }

    // `rgb` on every zone, or on the back ones only (the front ones off).
    void show(std::uint32_t rgb, bool back_only) {
        const std::string lit = hex(rgb), dark = hex(0);
        for (const Zone& z : zones_) {
            z.colour.write(back_only && z.front ? dark : lit);
            z.effect.write(kStatic);
        }
    }

    // Everything as it was read, each zone's effect last.
    void release() {
        for (std::size_t i = 0; i < brightness_.size(); ++i)
            if (!saved_brightness_[i].empty())
                brightness_[i].write(saved_brightness_[i]);
        for (const Zone& z : zones_) {
            if (!z.saved_cycles.empty())
                z.cycles.write(z.saved_cycles);
            if (!z.saved_duration.empty())
                z.duration.write(z.saved_duration);
            if (!z.saved_colour.empty())
                z.colour.write(z.saved_colour);
            if (!z.saved_effect.empty())
                z.effect.write(z.saved_effect);
        }
        held_ = false;
        std::printf("leds: given back\n");
        std::fflush(stdout);
    }

private:
    std::vector<Zone> zones_;
    std::vector<Attr> brightness_;
    std::vector<std::string> saved_brightness_;
    bool held_ = false;
};

}  // namespace

struct Leds::Impl {
    bool present = false;
    std::mutex mutex;
    std::unique_ptr<Driver> driver;  // opened at the first show
    // What the port asked for last, shown again when the host lets go.
    bool wanted = false;
    std::uint32_t rgb = 0;
    bool back_only = false;
    unsigned percent = 100;
    // What the driver shows now, while held (so an unchanged show writes nothing).
    bool shown = false;
    std::uint32_t shown_rgb = 0;
    bool shown_back_only = false;
    unsigned shown_percent = 0;
    bool host_holds = false, closed = false;

    // The port's last show on the LEDs, taking them over if they are not held. Under `mutex`.
    void apply() {
        if (!driver) {
            driver = std::make_unique<Driver>();
            if (!driver->usable()) {
                std::printf("leds: the driver has none of this device's zones\n");
                std::fflush(stdout);
            }
        }
        if (!driver->usable())
            return;
        if (!driver->held()) {
            driver->take_over(percent);
            shown_percent = percent;
            shown_rgb = 0, shown_back_only = false, shown = true;  // take_over leaves every zone black
        } else if (percent != shown_percent) {
            driver->set_brightness(percent);
            shown_percent = percent;
        }
        if (rgb != shown_rgb || back_only != shown_back_only) {
            driver->show(rgb, back_only);
            shown_rgb = rgb, shown_back_only = back_only;
        }
    }

    void give_back() {
        if (driver && driver->held())
            driver->release();
        shown = false;
    }
};

Leds::Leds() : impl_(new Impl) {
    impl_->present = access((std::string(kDir) + "effect_enable").c_str(), W_OK) == 0;
}

Leds::~Leds() {
    close();
}

bool Leds::available() const noexcept {
    return impl_->present;
}

bool Leds::show(std::uint32_t rgb, bool back_only, unsigned percent) noexcept {
    Impl& m = *impl_;
    if (!m.present)
        return false;
    std::lock_guard<std::mutex> lock(m.mutex);
    if (m.closed)
        return false;
    m.wanted = true;
    m.rgb = rgb & 0xFFFFFFu, m.back_only = back_only;
    m.percent = percent < 10 ? 10 : percent > 100 ? 100 : percent;
    if (m.host_holds)
        return false;
    m.apply();
    return m.shown;
}

void Leds::release() noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->wanted = false;
    impl_->give_back();
}

void Leds::hold(bool held) noexcept {
    Impl& m = *impl_;
    std::lock_guard<std::mutex> lock(m.mutex);
    if (m.closed || m.host_holds == held)
        return;
    m.host_holds = held;
    if (held)
        m.give_back();
    else if (m.wanted)
        m.apply();
}

void Leds::close() noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->closed = true;
    impl_->wanted = false;
    impl_->give_back();
}

#else  // no LED driver

struct Leds::Impl {};
Leds::Leds() : impl_(new Impl) {}
Leds::~Leds() = default;
bool Leds::available() const noexcept { return false; }
bool Leds::show(std::uint32_t, bool, unsigned) noexcept { return false; }
void Leds::release() noexcept {}
void Leds::hold(bool) noexcept {}
void Leds::close() noexcept {}

#endif

}  // namespace dream::render::vk
