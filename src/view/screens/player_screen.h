/*
 * SPDX-FileCopyrightText: 2026 ZeroTube contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_screen.h"
#include "media_player.h"

#include <memory>
#include <string>
#include <vector>

namespace screen {

// Full-screen playback. Asks the helper for the streams, then plays them with
// the FFmpeg player. Any key shows the controls for 3 s; they stay while
// paused, loading or at the end.
class PlayerScreen : public BaseScreen {
public:
    PlayerScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);
    ~PlayerScreen() override;

protected:
    void build_content(lv_obj_t* content) override;

private:
    static bool key_handler(uint32_t key, void* user_data);
    static void tick_cb(lv_timer_t* timer);
    bool on_key(uint32_t key);
    void resolve();
    void tick();
    void show_osd();
    void set_status(const char* title, const char* detail, bool spinner);

    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    platform::MediaPlayer player_;
    lv_timer_t* timer_ = nullptr;
    bool resolved_ = false;
    bool resolving_ = false;
    std::string error_;
    std::string video_url_, audio_url_;
    int volume_ = 100;
    double last_position_ = 0;

    std::vector<std::uint8_t> frame_pixels_;
    lv_image_dsc_t frame_dsc_{};
    std::uint32_t frame_seq_ = 0;
    int last_w_ = 0, last_h_ = 0;

    uint32_t osd_until_ = 0;
    uint32_t toast_until_ = 0;
    lv_obj_t* video_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_obj_t* status_spinner_ = nullptr;
    lv_obj_t* status_title_ = nullptr;
    lv_obj_t* status_detail_ = nullptr;
    lv_obj_t* top_ = nullptr;
    lv_obj_t* bottom_ = nullptr;
    lv_obj_t* time_ = nullptr;
    lv_obj_t* track_ = nullptr;
    lv_obj_t* fill_ = nullptr;
    lv_obj_t* knob_ = nullptr;
    lv_obj_t* center_icon_ = nullptr;
    lv_obj_t* toast_ = nullptr;
    int fill_w_ = -1;
    bool osd_shown_ = true;
};

}  // namespace screen
