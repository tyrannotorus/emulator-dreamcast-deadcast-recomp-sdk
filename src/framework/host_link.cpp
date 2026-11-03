// A recomp module's link to the emulator that hosts it. The host owns the MENU button, the menu, Options
// and the store; the module plays the game and does as it is told. Two pipes, DEADUI_HOST=<read fd>,<write fd>,
// lines of text:
//   from the host: pause, resume, set <key> <value>, quit
//   to the host:   ready (the first vblank), paused (nothing more of this process reaches the screen)
// Paused, the guest is held at its vblank (clock, sound and game logic stopped) and nothing is drawn: the screen
// is the host's until resume. Run without a host, the game simply plays, with no menu.
#include "dream/render/input.h"
#include "dream/runtime/host_ext.h"
#include "dream/runtime/system.h"
#include "dreamcomp/port.h"
#include "dreamcomp/settings.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace dreamcomp {

namespace {

// frames with nothing new presented before the screen counts as handed over
constexpr int kStableTicks = 4;
// a setting naming a control of the game: control.<its file key>
const std::string kControl = "control.";

class HostLink final : public dream::host::Extension {
public:
    const char* name() const override { return "host"; }

    // MENU is the host's, never a quit (the window then leaves it alone)
    bool wants_overlay() override { return true; }

    void on_start(dream::System& sys) override {
        sys_ = &sys;
        const char* fds = std::getenv("DEADUI_HOST");
        if (fds == nullptr || std::sscanf(fds, "%d,%d", &in_, &out_) != 2) {
            in_ = out_ = -1;
            std::printf("host link: no host (DEADUI_HOST): the game plays with no menu\n");
            std::fflush(stdout);
            return;
        }
        fcntl(in_, F_SETFL, O_NONBLOCK);
        fcntl(in_, F_SETFD, FD_CLOEXEC);
        fcntl(out_, F_SETFD, FD_CLOEXEC);
    }

    void on_vblank(dream::System&) override {
        perf();
        if (!ready_) {
            ready_ = true;
            controlsAtStart();
            say("ready");
        }
        commands();
    }

    bool guest_paused() override { return paused_; }
    bool screen_released() override { return paused_; }

    void on_paused_tick() override {
        commands();
        if (!paused_ || answered_)
            return;
        // the frames on their way out first, or one lands over the host's page
        auto& hc = dream::host::host_controls();
        const std::uint64_t presented = hc.presented_frames ? hc.presented_frames() : 0;
        if (presented != presentedLast_) {
            presentedLast_ = presented;
            stable_ = 0;
            return;
        }
        if (++stable_ < kStableTicks)
            return;
        answered_ = true;
        say("paused");
    }

private:
    void say(const char* line) {
        if (out_ < 0)
            return;
        const std::string text = std::string(line) + "\n";
        if (write(out_, text.data(), text.size()) != static_cast<ssize_t>(text.size()))
            std::printf("host link: the host did not take \"%s\"\n", line);
    }

    void commands() {
        if (in_ < 0)
            return;
        char data[512];
        for (;;) {
            const ssize_t size = read(in_, data, sizeof data);
            if (size == 0) {
                std::printf("host link: the host has gone: quitting\n");
                std::fflush(stdout);
                close(in_);
                in_ = -1;
                quit();
                return;
            }
            if (size < 0)
                return;
            pending_.append(data, static_cast<std::size_t>(size));
            for (std::size_t end; (end = pending_.find('\n')) != std::string::npos; pending_.erase(0, end + 1))
                command(pending_.substr(0, end));
        }
    }

    void command(const std::string& line) {
        if (line == "pause") {
            if (paused_)
                return;
            paused_ = true;
            answered_ = false;
            stable_ = 0;
            auto& hc = dream::host::host_controls();
            presentedLast_ = hc.presented_frames ? hc.presented_frames() : 0;
            corner(nullptr);
            if (sys_ != nullptr && sys_->rumble)
                sys_->rumble(0, 0.0f, 0);
        } else if (line == "resume") {
            paused_ = false;
            vblanks_ = 0;  // the time held is not the game's
        } else if (line == "quit") {
            quit();
        } else if (line.rfind("set ", 0) == 0) {
            const std::size_t space = line.find(' ', 4);
            if (space != std::string::npos)
                set(line.substr(4, space - 4), line.substr(space + 1));
            else
                std::printf("host link: bad command: %s\n", line.c_str());
        } else {
            std::printf("host link: unknown command: %s\n", line.c_str());
        }
        std::fflush(stdout);
    }

    void set(const std::string& key, const std::string& value) {
        auto& hc = dream::host::host_controls();
        if (key == "scale")
            set_render_scale(value == "panel" ? kScalePanel : std::atoi(value.c_str()));
        else if (key == "rumble" && hc.set_rumble)
            hc.set_rumble(static_cast<float>(std::atoi(value.c_str())) / 100.0f);
        else if (key == "fps")
            fps_ = value == "on";
        else if (key == "label")
            readCells(value);
        else if (key.rfind(kControl, 0) == 0)
            control(key.substr(kControl.size()), value);
    }

    // a control of the game on the pad's inputs: the SDK's default until told otherwise
    static void control(const std::string& key, const std::string& inputs) {
        auto& hc = dream::host::host_controls();
        if (!hc.set_pad_control || !hc.set_pad_control(key, inputs))
            std::printf("host link: control %s cannot be on \"%s\": it stays as it was\n", key.c_str(), inputs.c_str());
    }

    static void controlsAtStart() {
        for (unsigned i = 0; i < dream::render::kPadControlCount; ++i) {
            const std::string key = dream::render::pad_control_key(static_cast<dream::render::PadControl>(i));
            const std::string inputs = settings().get(kControl + key);
            if (!inputs.empty())
                control(key, inputs);
        }
        std::fflush(stdout);
    }

    static void quit() {
        if (auto& hc = dream::host::host_controls(); hc.quit)
            hc.quit();
    }

    // once a second: the game's vblanks (60 at full speed) and the frames shown, which Show FPS puts in the corner
    void perf() {
        const auto now = std::chrono::steady_clock::now();
        auto& hc = dream::host::host_controls();
        const std::uint64_t presented = hc.presented_frames ? hc.presented_frames() : 0;
        if (vblanks_++ == 0) {
            perfStart_ = since_ = now;
            presentedAt_ = presented;
            return;
        }
        const double s = std::chrono::duration<double>(now - since_).count();
        if (s < 1.0)
            return;
        const double shown = static_cast<double>(presented - presentedAt_) / s;
        std::printf("Deadcast perf %6.1f s: vblank %5.2f/s presented %5.2f/s\n",
                    std::chrono::duration<double>(now - perfStart_).count(), (vblanks_ - 1) / s, shown);
        std::fflush(stdout);
        if (fps_ && !paused_) {
            char text[32];
            std::snprintf(text, sizeof text, "%4.1f", shown);
            corner(text);
        } else if (!fps_ && cornerShown_) {
            corner(nullptr);
        }
        vblanks_ = 1;
        since_ = now;
        presentedAt_ = presented;
    }

    // The characters of the frame counter, drawn by the host (the module has no fonts): its padding and their
    // height, then for each its code, its width and its pixels. All as 32-bit values.
    void readCells(const std::string& file) {
        cells_.clear();
        FILE* f = std::fopen(file.c_str(), "rb");
        if (f == nullptr) {
            std::printf("host link: no label cells at %s\n", file.c_str());
            return;
        }
        std::uint32_t head[3] = {};
        if (std::fread(head, sizeof head[0], 3, f) == 3) {
            cellPad_ = head[0];
            cellHeight_ = head[1];
            for (std::uint32_t i = 0; i < head[2]; ++i) {
                std::uint32_t cell[2] = {};
                if (std::fread(cell, sizeof cell[0], 2, f) != 2)
                    break;
                std::vector<std::uint32_t> px(static_cast<std::size_t>(cell[1]) * cellHeight_);
                if (std::fread(px.data(), sizeof px[0], px.size(), f) != px.size())
                    break;
                cells_[static_cast<char>(cell[0])] = {cell[1], std::move(px)};
            }
        }
        std::fclose(f);
    }

    // the frame counter in the corner, or none
    void corner(const char* text) {
        auto& hc = dream::host::host_controls();
        if (!hc.set_corner)
            return;
        cornerShown_ = false;
        if (text == nullptr || cells_.empty()) {
            hc.set_corner(nullptr, 0, 0);
            return;
        }
        std::uint32_t width = cellPad_ * 2;
        for (const char* c = text; *c; ++c)
            if (auto cell = cells_.find(*c); cell != cells_.end())
                width += cell->second.width;
        const std::uint32_t height = cellHeight_ + cellPad_ * 2;
        std::vector<std::uint32_t> px(static_cast<std::size_t>(width) * height, kCornerBox);
        std::uint32_t x = cellPad_;
        for (const char* c = text; *c; ++c) {
            const auto cell = cells_.find(*c);
            if (cell == cells_.end())
                continue;
            for (std::uint32_t y = 0; y < cellHeight_; ++y)
                std::memcpy(&px[static_cast<std::size_t>(y + cellPad_) * width + x],
                            &cell->second.px[static_cast<std::size_t>(y) * cell->second.width],
                            cell->second.width * sizeof px[0]);
            x += cell->second.width;
        }
        hc.set_corner(px.data(), width, height);
        cornerShown_ = true;
    }

    static constexpr std::uint32_t kCornerBox = 0x202020;  // as the host's label

    struct Cell {
        std::uint32_t width = 0;
        std::vector<std::uint32_t> px;
    };

    dream::System* sys_ = nullptr;
    int in_ = -1;
    int out_ = -1;
    std::string pending_;
    bool ready_ = false;
    std::atomic<bool> paused_{false};
    bool answered_ = false;
    int stable_ = 0;
    std::uint64_t presentedLast_ = 0;
    bool fps_ = false;
    bool cornerShown_ = false;
    std::map<char, Cell> cells_;
    std::uint32_t cellPad_ = 0;
    std::uint32_t cellHeight_ = 0;
    std::uint64_t presentedAt_ = 0;
    unsigned vblanks_ = 0;
    std::chrono::steady_clock::time_point perfStart_, since_;
};

HostLink g_link;
dream::host::Register g_register(g_link);

}  // namespace

}  // namespace dreamcomp
