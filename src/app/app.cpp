/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"

#include "application_config.h"
#include "asset_manager.h"
#include "logger.h"
#include "linux_input.h"
#include "screen_manager.h"
#include "base_viewmodel.h"
#include "theme.h"
#include "screenshot_service.h"
#include "youtube_service.h"

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if USE_DESKTOP
#include "desktop_launcher.h"
#include "desktop_simulator_frame.h"
#endif

#if !USE_DESKTOP
#if APP_USE_DRM
#include "src/drivers/display/drm/lv_linux_drm.h"
#else
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#endif
#include <unistd.h>
#endif

#ifndef APP_FRAMEBUFFER_DEVICE
#define APP_FRAMEBUFFER_DEVICE "/dev/fb0"
#endif

#ifndef APP_DRM_DEVICE
#define APP_DRM_DEVICE "/dev/dri/card0"
#endif

#ifndef APP_DRM_CONNECTOR_ID
#define APP_DRM_CONNECTOR_ID -1
#endif

#ifndef APP_CONFIG_FILE
#define APP_CONFIG_FILE "zerotube.conf"
#endif

namespace app {
namespace {

void quit_requested_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* running = static_cast<bool*>(lv_observer_get_user_data(observer));
    if (running && lv_subject_get_int(subject)) {
        *running = false;
    }
}

struct DarkModePersistence {
    std::string config_path;
    bool last_dark_mode;
};

std::string writable_config_path() {
#if USE_DESKTOP
    return APP_CONFIG_FILE;
#else
    if (const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME")) {
        const std::filesystem::path root(xdg_config_home);
        if (!root.empty() && root.is_absolute()) {
            return (root / "zerotube" / "zerotube.conf").string();
        }
    }
    if (const char* home = std::getenv("HOME")) {
        const std::filesystem::path root(home);
        if (!root.empty() && root.is_absolute()) {
            return (root / ".config" / "zerotube" / "zerotube.conf").string();
        }
    }
    return APP_CONFIG_FILE;
#endif
}

void persist_dark_mode_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* persistence = static_cast<DarkModePersistence*>(lv_observer_get_user_data(observer));
    if (!persistence) {
        return;
    }
    const bool dark_mode = lv_subject_get_int(subject) != 0;
    if (dark_mode == persistence->last_dark_mode) {
        return;
    }
    persistence->last_dark_mode = dark_mode;

    ApplicationConfig config;
    config.dark_mode = dark_mode;
    std::string error;
    if (save_application_config(persistence->config_path, config, error)) {
        LOG_INFO("saved config: {} (dark_mode={})",
                 persistence->config_path,
                 config.dark_mode ? "yes" : "no");
    } else {
        LOG_WARN("failed to save config {}: {}", persistence->config_path, error);
    }
}

#if !USE_DESKTOP
#if !APP_USE_DRM
// The CP0's ST7789V LCD is not reliably /dev/fb0 -- that node is often HDMI or
// another framebuffer, and the panel commonly lands on /dev/fb1. The upstream
// template hardcodes APP_FRAMEBUFFER_DEVICE and returns a null display when it
// guesses wrong, which looks like a dead app rather than a misconfiguration.
// Resolve it the way M5's own apps do: explicit env override, then /proc/fb,
// then the compiled-in default as a last resort.
std::string resolve_framebuffer_device() {
    if (const char* forced = std::getenv("LV_LINUX_FBDEV_DEVICE"); forced && *forced) {
        LOG_INFO("framebuffer forced by LV_LINUX_FBDEV_DEVICE: {}", forced);
        return forced;
    }

    // /proc/fb lines are "<index> <driver-name>", e.g. "1 fb_st7789v".
    std::ifstream proc_fb("/proc/fb");
    if (proc_fb) {
        std::string line;
        while (std::getline(proc_fb, line)) {
            std::istringstream fields(line);
            int index = -1;
            std::string driver;
            if (!(fields >> index >> driver)) {
                continue;
            }
            if (driver.find("st7789v") != std::string::npos) {
                std::string path = "/dev/fb" + std::to_string(index);
                LOG_INFO("framebuffer detected from /proc/fb: {} ({})", path, driver);
                return path;
            }
        }
        LOG_WARN("no st7789v entry in /proc/fb; falling back to {}", APP_FRAMEBUFFER_DEVICE);
    } else {
        LOG_WARN("cannot read /proc/fb; falling back to {}", APP_FRAMEBUFFER_DEVICE);
    }

    return APP_FRAMEBUFFER_DEVICE;
}
#endif

// The CP0 LCD is a DRM SPI panel; /dev/fb0 is the kernel's fbdev emulation,
// which sends mmap writes to the panel from a deferred-I/O timer (every 50 ms),
// capping what reaches the glass at ~10-20 fps and adding up to 50 ms to every
// key press. fsync() on the fbdev runs that flush at once, so a frame that
// changed a large part of the screen goes out immediately; small updates (the
// clock) keep the batched pace. ZEROTUBE_FB_SYNC=0 turns it off.
int g_frame_push_fd = -1;
uint32_t g_frame_pixels = 0;
uint32_t g_frame_push_min_pixels = 0;

void frame_push_cb(lv_event_t* event) {
    if (lv_event_get_code(event) == LV_EVENT_FLUSH_START) {
        if (const auto* area = static_cast<const lv_area_t*>(lv_event_get_param(event))) {
            g_frame_pixels += static_cast<uint32_t>(lv_area_get_size(area));
        }
        return;
    }
    if (g_frame_pixels > 0 && g_frame_pixels >= g_frame_push_min_pixels) {
        fsync(g_frame_push_fd);
    }
    g_frame_pixels = 0;
}

[[maybe_unused]] void enable_frame_push(lv_display_t* display, const std::string& framebuffer) {
    const char* enabled = std::getenv("ZEROTUBE_FB_SYNC");
    if (enabled && std::strcmp(enabled, "0") == 0) {
        return;
    }
    g_frame_push_fd = open(framebuffer.c_str(), O_RDWR | O_CLOEXEC);
    if (g_frame_push_fd < 0) {
        return;
    }
    const uint32_t screen = static_cast<uint32_t>(lv_display_get_horizontal_resolution(display)) *
                            static_cast<uint32_t>(lv_display_get_vertical_resolution(display));
    g_frame_push_min_pixels = screen * 12U / 100U;  // a page switch, not the clock
    lv_display_add_event_cb(display, frame_push_cb, LV_EVENT_FLUSH_START, nullptr);
    lv_display_add_event_cb(display, frame_push_cb, LV_EVENT_REFR_READY, nullptr);
}

lv_display_t* init_device_display() {
#if APP_USE_DRM
    auto* display = lv_linux_drm_create();
    if (!display) {
        return nullptr;
    }

    if (lv_linux_drm_set_file(display, APP_DRM_DEVICE, APP_DRM_CONNECTOR_ID) != LV_RESULT_OK) {
        lv_display_delete(display);
        return nullptr;
    }

    platform::init_key_input(display);
    return display;
#else
    auto* display = lv_linux_fbdev_create();
    if (!display) {
        return nullptr;
    }

    const std::string framebuffer = resolve_framebuffer_device();
    if (lv_linux_fbdev_set_file(display, framebuffer.c_str()) != LV_RESULT_OK) {
        LOG_ERROR("cannot open framebuffer {}", framebuffer);
        lv_display_delete(display);
        return nullptr;
    }

    // No lv_linux_fbdev_set_force_refresh(). On the CP0's DRM-backed fbdev
    // (panel-mipi-dbi) each FBIOPUT_VSCREENINFO+FORCE is a forced mode-set and a
    // full SPI push; per flushed strip it made redraws slow and tear (measured on
    // a CardputerZero, 2026-11-19). CardBili does the same and reaches 30 fps.

    enable_frame_push(display, framebuffer);
    platform::init_key_input(display);
    return display;
#endif
}
#endif

#if USE_DESKTOP
// ZEROTUBE_SCRIPT="2000:shot;500:s;300:l;..." drives the simulator for
// testing: each step waits its milliseconds, then presses a key (one
// character, or UP DOWN LEFT RIGHT ENTER ESC BS) or takes a screenshot
// ("shot" -> ~/Pictures/Screenshots). "quit" ends the app.
struct ScriptStep {
    uint32_t delay;
    std::string action;
};
std::vector<ScriptStep> g_script;
std::size_t g_script_pos = 0;
bool* g_script_running = nullptr;

void script_timer_cb(lv_timer_t* timer) {
    if (g_script_pos >= g_script.size()) {
        lv_timer_delete(timer);
        return;
    }
    const auto& step = g_script[g_script_pos++];
    const std::string& a = step.action;
    if (a == "shot") {
        const auto r = platform::screenshot::capture_active_screen();
        LOG_INFO("script shot: {}", r.success ? r.path : r.error);
    } else if (a == "quit") {
        if (g_script_running) *g_script_running = false;
    } else {
        uint32_t key = 0;
        if (a == "UP") key = LV_KEY_UP;
        else if (a == "DOWN") key = LV_KEY_DOWN;
        else if (a == "LEFT") key = LV_KEY_LEFT;
        else if (a == "RIGHT") key = LV_KEY_RIGHT;
        else if (a == "ENTER") key = LV_KEY_ENTER;
        else if (a == "ESC") key = LV_KEY_ESC;
        else if (a == "BS") key = LV_KEY_BACKSPACE;
        else if (a == "SPACE") key = ' ';
        else if (!a.empty()) key = static_cast<unsigned char>(a[0]);
        if (key) platform::inject_key(key);
    }
    if (g_script_pos < g_script.size()) lv_timer_set_period(timer, g_script[g_script_pos].delay);
}

void start_script(bool* running) {
    const char* text = std::getenv("ZEROTUBE_SCRIPT");
    if (!text || !*text) return;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ';')) {
        const auto colon = item.find(':');
        if (colon == std::string::npos) continue;
        g_script.push_back({static_cast<uint32_t>(std::stoul(item.substr(0, colon))), item.substr(colon + 1)});
    }
    if (g_script.empty()) return;
    g_script_running = running;
    lv_timer_create(script_timer_cb, g_script.front().delay, nullptr);
}
#endif

} // namespace

int Application::run() {
    logger::Logger::init();
    logger::Logger::set_tag("zerotube");

    lv_init();

    AssetManager assets;
    for (const auto& root : assets.roots()) {
        LOG_INFO("asset root: {}", root.string());
    }

    viewmodel::BaseViewModel view_model;
    const std::string user_config_path = writable_config_path();
    std::string loaded_config_path = APP_CONFIG_FILE;
    if (user_config_path != APP_CONFIG_FILE) {
        std::error_code filesystem_error;
        if (std::filesystem::is_regular_file(user_config_path, filesystem_error)) {
            loaded_config_path = user_config_path;
        }
    }

    ApplicationConfig config;
    std::string config_error;
    if (load_application_config(loaded_config_path, config, config_error)) {
        view_model.set_dark_mode(config.dark_mode);
        LOG_INFO("loaded config: {} (dark_mode={})",
                 loaded_config_path,
                 config.dark_mode ? "yes" : "no");
    } else {
        LOG_WARN("failed to load config {}: {}; using defaults", loaded_config_path, config_error);
    }

#if USE_DESKTOP
    DesktopSimulatorFrame simulator_frame(assets);
    auto* display = simulator_frame.display();
#else
    auto* display = init_device_display();
    // The fbdev/DRM drivers register a tick source but no delay. Without one,
    // lv_delay_ms() in the main loop spin-waits, pinning a whole core (99% CPU
    // measured) -- on the CP0 that is battery. The simulator sets
    // SDL_Delay for the same reason.
    lv_delay_set_cb([](uint32_t ms) { usleep(ms * 1000U); });
#endif
    if (!display) {
        LOG_ERROR("failed to initialize display");
        return 1;
    }

    view::apply_lvgl_theme(display, view_model.is_dark_mode());

#if USE_DESKTOP
    simulator_frame.bind_dark_mode(view_model.dark_mode_subject());
#endif

    ScreenManager screen_manager(view_model, assets);

#if USE_DESKTOP
    // Boot into the launcher stand-in, the way the device does, so the icon
    // and the open/return flow are visible in the simulator. It starts the
    // screen manager when something is launched -- until then nothing may
    // load a screen over the launcher.
    DesktopLauncher launcher(assets, view_model, screen_manager);
    simulator_frame.set_home_handler([&launcher]() { launcher.show(); });
    launcher.show();
#else
    screen_manager.start();
#endif

    // The YouTube helper (Python + yt-dlp) starts now, while the first screen
    // draws: importing yt-dlp takes a few seconds on the CM0.
    auto& service = net::YouTube::instance();
    service.start();

    DarkModePersistence dark_mode_persistence{user_config_path, view_model.is_dark_mode()};
    auto* dark_mode_observer = lv_subject_add_observer(view_model.dark_mode_subject(),
                                                        persist_dark_mode_observer,
                                                        &dark_mode_persistence);

    bool running = true;
    auto* quit_observer = lv_subject_add_observer(view_model.quit_requested_subject(),
                                                  quit_requested_observer,
                                                  &running);

#if USE_DESKTOP
    start_script(&running);
#endif
    LOG_INFO("LVGL app started at {}x{}", lv_display_get_horizontal_resolution(display),
             lv_display_get_vertical_resolution(display));
    while (running
#if USE_DESKTOP
           && simulator_frame.process_events()
#endif
    ) {
        // Sleep until LVGL's next timer is due (refresh 16 ms, input read 30 ms) instead of
        // waking every 5 ms: ~60 wakeups/s instead of ~200 on a battery device.
        const uint32_t next = lv_timer_handler();
#if USE_DESKTOP
        lv_delay_ms(next == LV_NO_TIMER_READY ? 16 : LV_CLAMP(1, next, 16));
#else
        // Sleep until a key, a worker's wake_main_loop(), or LVGL's next timer.
        // LVGL pauses its refresh timer while nothing is invalid, so an idle
        // screen wakes only for its own timers instead of ~60 times a second.
        platform::wait_for_key_input(next == LV_NO_TIMER_READY ? -1 : static_cast<int>(next));
#endif
    }

    if (quit_observer) {
        lv_observer_remove(quit_observer);
    }
    service.stop();
    if (dark_mode_observer) {
        lv_observer_remove(dark_mode_observer);
    }


    return 0;
}

} // namespace app
