// See led_light.h.
#include "dream/render/vk/led_light.h"

#include <algorithm>
#include <cstdio>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#endif

namespace dream::render::vk {

namespace {

// `c` brightened until its largest channel is 255, its hue and saturation kept. Black stays black.
Rgb at_full_value(Rgb c) noexcept {
    const unsigned top = std::max(c.r, std::max(c.g, c.b));
    if (!top)
        return c;
    auto up = [top](unsigned v) { return static_cast<std::uint8_t>(std::min(255u, (v * 255u + top / 2) / top)); };
    return Rgb{up(c.r), up(c.g), up(c.b)};
}

}  // namespace

bool dominant_colour(const std::uint8_t* rgba, std::size_t pixels, Rgb& out) noexcept {
    // Hue in 24 bins of 15 degrees. A pixel counts with weight saturation^2 * value^2: a bright, fully
    // coloured pixel 1, a dim background (value 0.15) 0.02, grey and white nothing; below a quarter of
    // saturation, or nearly black, it has no hue worth showing and is left out.
    constexpr unsigned kBins = 24;
    constexpr int kDarkest = 32;
    float weight[kBins] = {}, sum_r[kBins] = {}, sum_g[kBins] = {}, sum_b[kBins] = {};
    for (std::size_t i = 0; i < pixels; ++i) {
        const int r = rgba[4 * i], g = rgba[4 * i + 1], b = rgba[4 * i + 2];
        const int top = std::max(r, std::max(g, b)), d = top - std::min(r, std::min(g, b));
        if (top < kDarkest || d * 4 < top)
            continue;
        const float fr = static_cast<float>(r), fg = static_cast<float>(g), fb = static_cast<float>(b);
        const float ftop = static_cast<float>(top), fd = static_cast<float>(d);
        float h = top == r ? (fg - fb) / fd : top == g ? 2.0f + (fb - fr) / fd : 4.0f + (fr - fg) / fd;
        if (h < 0.0f)
            h += 6.0f;
        const unsigned bin = std::min(kBins - 1, static_cast<unsigned>(h * (kBins / 6.0f)));
        const float s = fd / ftop, v = ftop / 255.0f;
        const float w = s * s * v * v;
        weight[bin] += w;
        sum_r[bin] += w * fr, sum_g[bin] += w * fg, sum_b[bin] += w * fb;
    }
    // The bin with the most weight, its neighbours counting half (a hue on a bin's edge is not split).
    unsigned best = 0;
    float best_score = 0.0f;
    for (unsigned i = 0; i < kBins; ++i) {
        const float score = weight[(i + kBins - 1) % kBins] + 2.0f * weight[i] + weight[(i + 1) % kBins];
        if (score > best_score)
            best = i, best_score = score;
    }
    float w = 0.0f, r = 0.0f, g = 0.0f, b = 0.0f;
    for (const unsigned i : {(best + kBins - 1) % kBins, best, (best + 1) % kBins})
        w += weight[i], r += sum_r[i], g += sum_g[i], b += sum_b[i];
    // At least what two bright, fully coloured pixels of the sample would weigh: less is noise.
    if (w < 2.0f)
        return false;
    // The band's mean, its weakest channel halved (an LED shows a pale colour as white), at full value.
    r /= w, g /= w, b /= w;
    const float low = std::min(r, std::min(g, b)) * 0.5f;
    auto channel = [low](float v) { return static_cast<std::uint8_t>(std::clamp(v - low + 0.5f, 0.0f, 255.0f)); };
    out = at_full_value(Rgb{channel(r), channel(g), channel(b)});
    return true;
}

#if defined(__linux__)

namespace {

using Clock = std::chrono::steady_clock;
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
            close(fd_);
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
    bool front;  // on the face of the device (the Brick's F1 and F2): off in kBack
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

std::string hex(Rgb c) {
    char s[8];
    std::snprintf(s, sizeof s, "%02X%02X%02X", c.r, c.g, c.b);
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
        std::printf("leds: the game's (%zu zones, brightness %u%%)\n", zones_.size(), percent);
        std::fflush(stdout);
    }

    void set_brightness(unsigned percent) {
        const std::string v = std::to_string(percent);
        for (const Attr& a : brightness_) a.write(v);
    }

    // `c` on every zone, or on the back ones only (the front ones off).
    void show(Rgb c, bool back_only) {
        const std::string lit = hex(c), dark = hex(Rgb{});
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

std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

}  // namespace

struct LedLight::Impl {
    bool present = false;
    std::atomic<unsigned> lit{kReleased}, percent{100};
    std::atomic<std::int64_t> hold_until{0};  // full until here, then the fall
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::condition_variable cv;
    std::thread thread;
    // The newest sample of the picture, handed over by submit_pixels.
    std::mutex pixels_mutex;
    std::vector<std::uint8_t> pixels;
    bool pixels_new = false;

    void wake() {
        { std::lock_guard<std::mutex> lock(mutex); }
        cv.notify_all();
    }

    // The newest sample into `into`, if one came since the last call.
    bool take_pixels(std::vector<std::uint8_t>& into) {
        std::lock_guard<std::mutex> lock(pixels_mutex);
        if (!pixels_new)
            return false;
        into.swap(pixels);
        pixels_new = false;
        return true;
    }

    template <class Pred>
    void wait_for(std::chrono::milliseconds ms, Pred pred) {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, ms, [&] { return stop.load() || pred(); });
    }

    void run() {
        Driver drv;
        if (!drv.usable()) {
            std::printf("leds: the driver has none of this device's zones\n");
            std::fflush(stdout);
            return;
        }
        unsigned applied_pct = 0, shown = kReleased;
        float level = 0.0f;                           // the pulse's envelope, 0 to 1
        Rgb target{255, 255, 255};                    // the picture's colour: white until a picture gives one
        float sr = 255.0f, sg = 255.0f, sb = 255.0f;  // that colour, smoothed while lit
        std::vector<std::uint8_t> picture;
        Rgb last{};
        bool have_last = false, pulsed = false;
        auto prev = Clock::now();
        while (!stop.load()) {
            const unsigned m = lit.load();
            const unsigned pct = percent.load();
            if (m == kReleased) {
                if (drv.held())
                    drv.release();
                level = 0.0f;
                have_last = false;
                wait_for(std::chrono::milliseconds(500), [&] { return lit.load() != kReleased; });
                prev = Clock::now();
                continue;
            }
            if (!drv.held()) {
                drv.take_over(pct);
                applied_pct = pct;
                have_last = pulsed = false;
                level = 0.0f;
            } else if (pct != applied_pct) {
                drv.set_brightness(pct);
                applied_pct = pct;
            }
            if (m == kOff) {
                if (!have_last || last != Rgb{} || shown != kOff)
                    drv.show(Rgb{}, false);
                last = Rgb{}, have_last = true, shown = kOff, level = 0.0f;
                wait_for(std::chrono::milliseconds(500), [&] { return lit.load() != kOff; });
                prev = Clock::now();
                continue;
            }
            const auto t = Clock::now();
            const float dt_ms = std::chrono::duration<float, std::milli>(t - prev).count();
            prev = t;
            const bool rising = now_ns() < hold_until.load();
            if (rising && !pulsed) {
                pulsed = true;
                std::printf("leds: pulsing with the game's vibration (whatever the motor is set to)\n");
                std::fflush(stdout);
            }
            const bool was_dark = level == 0.0f;
            level = std::clamp(level + (rising ? dt_ms / kRiseMs : -dt_ms / kFallMs), 0.0f, 1.0f);
            if (level > 0.0f) {
                // The picture's colour, analysed here (only while lit) so the render thread only copies
                // the sample over. A picture with no colour to speak of keeps the last one.
                if (Rgb c; take_pixels(picture) && dominant_colour(picture.data(), picture.size() / 4, c))
                    target = c;
                // A new pulse starts in the picture's colour; while lit, half way to a new colour each
                // frame (as NextUI's ambient mode blends), so one odd frame does not flicker.
                const float k = was_dark ? 1.0f : 0.5f;
                sr += (target.r - sr) * k, sg += (target.g - sg) * k, sb += (target.b - sb) * k;
            }
            auto channel = [level](float v) {
                return static_cast<std::uint8_t>(std::clamp(v * level + 0.5f, 0.0f, 255.0f));
            };
            const Rgb out{channel(sr), channel(sg), channel(sb)};
            if (!have_last || out != last || m != shown) {
                drv.show(out, m == kBack);
                last = out, have_last = true, shown = m;
            }
            // A frame apart while lit; dark, it waits for the next event.
            if (level == 0.0f && !rising) {
                wait_for(std::chrono::milliseconds(250),
                         [&] { return lit.load() != m || now_ns() < hold_until.load(); });
                prev = Clock::now();
            } else {
                std::this_thread::sleep_until(t + std::chrono::milliseconds(16));
            }
        }
        if (drv.held())
            drv.release();
    }
};

LedLight::LedLight() : impl_(new Impl) {
    impl_->present = access((std::string(kDir) + "effect_enable").c_str(), W_OK) == 0;
}

LedLight::~LedLight() {
    shutdown();
}

bool LedLight::available() const noexcept {
    return impl_->present;
}

// Called from one thread (the host link's commands, on the guest's).
void LedLight::configure(unsigned l, unsigned percent) noexcept {
    if (!impl_->present || impl_->stop.load())
        return;
    l = std::min<unsigned>(l, kAll);
    impl_->percent.store(std::clamp(percent, 10u, 100u));
    impl_->lit.store(l);
    lit_.store(l, std::memory_order_relaxed);
    if (l != kReleased && !impl_->thread.joinable())
        impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
    impl_->wake();
}

void LedLight::submit_pixels(const std::uint8_t* rgba, unsigned width, unsigned height) noexcept {
    std::lock_guard<std::mutex> lock(impl_->pixels_mutex);
    impl_->pixels.assign(rgba, rgba + static_cast<std::size_t>(width) * height * 4);
    impl_->pixels_new = true;
}

void LedLight::rumble_begin(unsigned ms) noexcept {
    if (!wants_colour())
        return;
    const std::int64_t now = now_ns();
    const std::int64_t end = now + static_cast<std::int64_t>(ms) * 1'000'000;
    // Full until the fall can finish at the event's end, but long enough to be seen.
    impl_->hold_until.store(std::max(end - static_cast<std::int64_t>(kFallMs) * 1'000'000,
                                     now + static_cast<std::int64_t>(kMinHoldMs) * 1'000'000));
    impl_->wake();
}

void LedLight::rumble_end() noexcept {
    impl_->hold_until.store(0);
}

void LedLight::shutdown() noexcept {
    if (impl_->stop.exchange(true))
        return;
    lit_.store(kReleased, std::memory_order_relaxed);
    impl_->wake();
    if (impl_->thread.joinable())
        impl_->thread.join();  // the worker gives the LEDs back on its way out
}

#else  // no LED driver

struct LedLight::Impl {};
LedLight::LedLight() : impl_(new Impl) {}
LedLight::~LedLight() = default;
bool LedLight::available() const noexcept { return false; }
void LedLight::configure(unsigned, unsigned) noexcept {}
void LedLight::submit_pixels(const std::uint8_t*, unsigned, unsigned) noexcept {}
void LedLight::rumble_begin(unsigned) noexcept {}
void LedLight::rumble_end() noexcept {}
void LedLight::shutdown() noexcept {}

#endif

}  // namespace dream::render::vk
