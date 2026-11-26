/*
 * SPDX-FileCopyrightText: 2026 ZeroTube contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lvgl.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace net {

struct Video {
    std::string id;
    std::string title;
    std::string channel;
    int duration = 0;  // seconds, 0 unknown
    long long views = 0;
    bool live = false;
};

std::vector<Video> parse_videos(const nlohmann::json& reply);

// The app's one connection to YouTube: the Python helper (yt-dlp) run as a
// child process, spoken to in JSON lines. Everything here is called on the
// LVGL thread, and every callback runs there too.
//
// Callbacks are tied to an owner (a screen's `alive` token): once the owner is
// gone the reply is dropped, so a page switch mid-request is safe.
class YouTube {
public:
    using Json = nlohmann::json;
    using Reply = std::function<void(const Json&)>;
    using Owner = std::weak_ptr<void>;

    static YouTube& instance();

    void start();
    void stop();

    // Helper state from its "ready" line.
    bool ready() const { return ready_; }
    bool engine_installed() const { return ready_ && import_error_.empty(); }
    const std::string& engine_version() const { return engine_; }
    bool signed_in() const { return signed_in_; }

    // Sends {"cmd": ..., ...}; `reply` gets the whole reply object ("ok",
    // "error", payload). Queued until the helper is ready.
    void request(Json req, Owner owner, Reply reply);

    // Events with no request: "ready", "signed_in", "engine_step",
    // "engine_done", "thumb_ready" (with "vid").
    int add_listener(Owner owner, Reply fn);
    void remove_listener(int id);

    // Installs or updates yt-dlp (helper/setup-engine.sh), restarting the
    // helper afterwards. Progress arrives as "engine_step"/"engine_done".
    void setup_engine();
    bool engine_busy() const { return engine_busy_; }

    // 64x36 rounded thumbnail for a video, or null while it is being fetched
    // (a "thumb_ready" event follows). Kept for the app's lifetime, bounded.
    const lv_image_dsc_t* thumbnail(const std::string& vid);
    void prefetch_thumbnails(const std::vector<std::string>& vids);
    // The signed-in account's picture (32x32 circle) once decoded.
    const lv_image_dsc_t* avatar() const;
    void load_avatar(const std::string& path);

    void set_signed_in(bool value) { signed_in_ = value; }

    struct Impl;

private:
    YouTube();
    void handle(const Json& message);
    void emit(const Json& event);
    void pump();
    static void wake_cb(void* self);
    static void timer_cb(lv_timer_t* timer);

    std::shared_ptr<Impl> impl_;
    bool ready_ = false;
    bool signed_in_ = false;
    bool engine_busy_ = false;
    std::string engine_;
    std::string import_error_;
};

}  // namespace net
