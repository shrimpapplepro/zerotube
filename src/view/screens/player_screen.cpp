/*
 * SPDX-FileCopyrightText: 2026 ZeroTube contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "player_screen.h"

#include "asset_manager.h"
#include "label_util.h"
#include "linux_input.h"
#include "ui_const.h"
#include "youtube_service.h"

#include "src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace screen {

namespace {

constexpr int kW = 320;
constexpr int kH = 170;
constexpr uint32_t kRed = 0xff0033;
constexpr uint32_t kOsdMs = 3000;
constexpr int kTrackX = 10;
constexpr int kTrackW = kW - 20;

// Frames in the panel's own format: the CP0 renders RGB565, so a frame blits
// as a copy instead of being converted from 32-bit on every draw.
#if USE_DESKTOP
constexpr platform::FrameFormat kFrameFormat = platform::FrameFormat::XRGB8888;
constexpr lv_color_format_t kFrameCf = LV_COLOR_FORMAT_XRGB8888;
constexpr int kFrameBpp = 4;
#else
constexpr platform::FrameFormat kFrameFormat = platform::FrameFormat::RGB565;
constexpr lv_color_format_t kFrameCf = LV_COLOR_FORMAT_RGB565;
constexpr int kFrameBpp = 2;
#endif

std::string clock_text(double seconds) {
    const int s = std::max(0, static_cast<int>(seconds));
    char buf[16];
    if (s >= 3600) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else std::snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    return buf;
}

void plain(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* text) {
    auto* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

}  // namespace

PlayerScreen::PlayerScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
    platform::set_screen_key_handler(key_handler, this);
    timer_ = lv_timer_create(tick_cb, 15, this);
    resolve();
}

PlayerScreen::~PlayerScreen() {
    if (timer_) lv_timer_delete(timer_);
    platform::clear_screen_key_handler(this);
    player_.stop();
    lv_image_cache_drop(&frame_dsc_);
}

void PlayerScreen::build_content(lv_obj_t* content) {
    const auto& req = view_model().play_request();
    auto load = [this](const char* f, uint32_t size, const lv_font_t* fb) -> const lv_font_t* {
        const lv_font_t* font = assets().load_font(f, size);
        return font ? font : fb;
    };
    const lv_font_t* f_title = load("inter-semibold.ttf", 12, &lv_font_montserrat_12);
    const lv_font_t* f_small = load("inter-regular.ttf", 10, &lv_font_montserrat_12);
    const lv_font_t* f_time = load("inter-medium.ttf", 10, &lv_font_montserrat_12);
    const lv_font_t* f_icon = load("Phosphor-Fill.ttf", 30, &lv_font_montserrat_28);
    const lv_font_t* f_status = load("inter-semibold.ttf", 13, &lv_font_montserrat_14);

    lv_obj_set_style_bg_color(content, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);

    video_ = lv_image_create(content);
    lv_obj_add_flag(video_, LV_OBJ_FLAG_HIDDEN);

    // Loading / error, centred.
    status_ = lv_obj_create(content);
    plain(status_);
    lv_obj_set_size(status_, kW - 30, 110);
    lv_obj_align(status_, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_flex_flow(status_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(status_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(status_, 6, 0);
    status_spinner_ = lv_spinner_create(status_);
    lv_obj_set_size(status_spinner_, 30, 30);
    lv_obj_set_style_arc_width(status_spinner_, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(status_spinner_, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(status_spinner_, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_arc_color(status_spinner_, lv_color_hex(kRed), LV_PART_INDICATOR);
    status_title_ = label(status_, f_status, 0xf1f1f1, "");
    status_detail_ = label(status_, f_small, 0xaaaaaa, "");
    lv_obj_set_width(status_detail_, kW - 40);
    lv_label_set_long_mode(status_detail_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(status_detail_, LV_TEXT_ALIGN_CENTER, 0);

    // Top: title and channel over a dark band.
    top_ = lv_obj_create(content);
    plain(top_);
    lv_obj_set_size(top_, kW, 36);
    lv_obj_set_pos(top_, 0, 0);
    lv_obj_set_style_bg_color(top_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(top_, LV_OPA_60, 0);
    auto* t = label(top_, f_title, 0xffffff, req.title.c_str());
    lv_obj_set_pos(t, 8, 3);
    lv_obj_set_size(t, kW - 16, 16);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    auto* c = label(top_, f_small, 0xcccccc, req.channel.c_str());
    lv_obj_set_pos(c, 8, 19);
    lv_obj_set_size(c, kW - 16, 14);
    lv_label_set_long_mode(c, LV_LABEL_LONG_DOT);

    // Bottom: time and the red progress bar.
    bottom_ = lv_obj_create(content);
    plain(bottom_);
    lv_obj_set_size(bottom_, kW, 26);
    lv_obj_set_pos(bottom_, 0, kH - 26);
    lv_obj_set_style_bg_color(bottom_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bottom_, LV_OPA_60, 0);
    time_ = label(bottom_, f_time, 0xffffff, "");
    lv_obj_set_pos(time_, kTrackX, 2);
    track_ = lv_obj_create(bottom_);
    plain(track_);
    lv_obj_set_size(track_, kTrackW, 3);
    lv_obj_set_pos(track_, kTrackX, 18);
    lv_obj_set_style_bg_color(track_, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(track_, LV_OPA_30, 0);
    fill_ = lv_obj_create(bottom_);
    plain(fill_);
    lv_obj_set_size(fill_, 0, 3);
    lv_obj_set_pos(fill_, kTrackX, 18);
    lv_obj_set_style_bg_color(fill_, lv_color_hex(kRed), 0);
    lv_obj_set_style_bg_opa(fill_, LV_OPA_COVER, 0);
    knob_ = lv_obj_create(bottom_);
    plain(knob_);
    lv_obj_set_size(knob_, 9, 9);
    lv_obj_set_style_radius(knob_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob_, lv_color_hex(kRed), 0);
    lv_obj_set_style_bg_opa(knob_, LV_OPA_COVER, 0);
    lv_obj_set_pos(knob_, kTrackX - 4, 15);

    center_icon_ = label(content, f_icon, 0xffffff, view::ICON_PAUSE);
    lv_obj_set_style_bg_color(center_icon_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(center_icon_, LV_OPA_50, 0);
    lv_obj_set_style_radius(center_icon_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(center_icon_, 8, 0);
    lv_obj_center(center_icon_);
    lv_obj_add_flag(center_icon_, LV_OBJ_FLAG_HIDDEN);

    toast_ = label(content, f_time, 0xffffff, "");
    lv_obj_set_style_bg_color(toast_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(toast_, LV_OPA_70, 0);
    lv_obj_set_style_radius(toast_, 10, 0);
    lv_obj_set_style_pad_hor(toast_, 10, 0);
    lv_obj_set_style_pad_ver(toast_, 4, 0);
    lv_obj_align(toast_, LV_ALIGN_TOP_RIGHT, -8, 42);
    lv_obj_add_flag(toast_, LV_OBJ_FLAG_HIDDEN);

    set_status("Loading", "", true);
    lv_obj_add_flag(bottom_, LV_OBJ_FLAG_HIDDEN);
}

void PlayerScreen::set_status(const char* title, const char* detail, bool spinner) {
    view::set_visible_if_changed(status_, title != nullptr);
    if (!title) return;
    view::set_visible_if_changed(status_spinner_, spinner);
    view::set_label_text_if_changed(status_title_, title);
    view::set_label_text_if_changed(status_detail_, detail);
    view::set_visible_if_changed(status_detail_, detail && *detail);
}

void PlayerScreen::resolve() {
    const auto& req = view_model().play_request();
    resolving_ = true;
    error_.clear();
    set_status("Loading", "", true);
    net::YouTube::instance().request(
        {{"cmd", "resolve"}, {"vid", req.id}}, alive_, [this](const net::YouTube::Json& reply) {
            resolving_ = false;
            if (!reply.value("ok", false)) {
                error_ = reply.value("error", "Cannot play this video");
                set_status("Can't play this video", (error_ + "\nR to retry   Esc back").c_str(), false);
                return;
            }
            resolved_ = true;
            video_url_ = reply.value("video", "");
            audio_url_ = reply.value("audio", "");
            player_.set_volume(volume_);
            player_.open(video_url_, kW, kH, kFrameFormat, "", last_position_, audio_url_);
            player_.set_volume(volume_);
        });
}

void PlayerScreen::show_osd() {
    osd_until_ = lv_tick_get() + kOsdMs;
    if (timer_) lv_timer_ready(timer_);
}

void PlayerScreen::tick_cb(lv_timer_t* timer) {
    static_cast<PlayerScreen*>(lv_timer_get_user_data(timer))->tick();
}

void PlayerScreen::tick() {
    using State = platform::MediaStatus::State;
    const auto st = player_.status();

    int w = 0, h = 0;
    if (player_.take_frame(frame_seq_, frame_pixels_, w, h)) {
        lv_image_cache_drop(&frame_dsc_);
        frame_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        frame_dsc_.header.cf = kFrameCf;
        frame_dsc_.header.w = static_cast<uint32_t>(w);
        frame_dsc_.header.h = static_cast<uint32_t>(h);
        frame_dsc_.header.stride = static_cast<uint32_t>(w * kFrameBpp);
        frame_dsc_.data = frame_pixels_.data();
        frame_dsc_.data_size = static_cast<uint32_t>(frame_pixels_.size());
        if (w != last_w_ || h != last_h_ || lv_obj_has_flag(video_, LV_OBJ_FLAG_HIDDEN)) {
            lv_image_set_src(video_, &frame_dsc_);
            lv_obj_set_size(video_, w, h);
            lv_obj_center(video_);
            lv_obj_remove_flag(video_, LV_OBJ_FLAG_HIDDEN);
            last_w_ = w;
            last_h_ = h;
        } else {
            lv_obj_invalidate(video_);
        }
    }

    // Status line in the middle.
    if (!error_.empty()) {
        // shown by resolve()
    } else if (resolving_ || st.state == State::Opening || (resolved_ && st.state == State::Idle)) {
        set_status("Loading", "", true);
    } else if (st.state == State::Error) {
        error_ = st.error;
        set_status("Playback stopped", (st.error + "\nR to retry   Esc back").c_str(), false);
    } else if (st.seeking && lv_obj_has_flag(video_, LV_OBJ_FLAG_HIDDEN)) {
        set_status("", "", true);
    } else {
        set_status(nullptr, nullptr, false);
    }
    if (st.state == State::Playing || st.state == State::Paused) last_position_ = st.position;

    // Controls.
    const uint32_t now = lv_tick_get();
    const bool playing_ok = st.state == State::Playing || st.state == State::Paused || st.state == State::Ended;
    const bool osd = resolving_ || !error_.empty() || st.state != State::Playing || st.seeking ||
                     static_cast<int32_t>(osd_until_ - now) > 0;
    view::set_visible_if_changed(top_, osd);
    view::set_visible_if_changed(bottom_, osd && playing_ok);
    if (osd && playing_ok) {
        std::string t = clock_text(st.position);
        if (st.duration > 0) t += " / " + clock_text(st.duration);
        view::set_label_text_if_changed(time_, t.c_str());
        const int fw = st.duration > 0 ? static_cast<int>(std::lround(kTrackW * std::clamp(st.position / st.duration, 0.0, 1.0)))
                                       : 0;
        if (fw != fill_w_) {
            fill_w_ = fw;
            lv_obj_set_width(fill_, fw);
            lv_obj_set_x(knob_, kTrackX + fw - 4);
        }
    }
    const bool paused = st.state == State::Paused;
    const bool ended = st.state == State::Ended;
    view::set_visible_if_changed(center_icon_, (paused || ended) && error_.empty());
    if (paused || ended) {
        view::set_label_text_if_changed(center_icon_, ended ? view::ICON_ARROW_CLOCKWISE : view::ICON_PLAY);
    }
    view::set_visible_if_changed(toast_, static_cast<int32_t>(toast_until_ - now) > 0);

    // Idle (paused, ended, failed): slow the poll right down.
    const bool active = resolving_ || st.state == State::Playing || st.state == State::Opening || st.seeking ||
                        static_cast<int32_t>(osd_until_ - now) > 0 || static_cast<int32_t>(toast_until_ - now) > 0;
    lv_timer_set_period(timer_, active ? 15 : 250);
}

bool PlayerScreen::key_handler(uint32_t key, void* user_data) {
    return static_cast<PlayerScreen*>(user_data)->on_key(key);
}

bool PlayerScreen::on_key(uint32_t key) {
    using State = platform::MediaStatus::State;
    const auto st = player_.status();
    switch (key) {
        case LV_KEY_ESC:
        case LV_KEY_BACKSPACE:
            player_.stop();
            view_model().open_page(model::AppPage::Browse);
            return true;
        case LV_KEY_ENTER:
        case ' ':
            if (st.state == State::Ended) {
                player_.seek_to(0);
                player_.set_paused(false);
            } else {
                player_.toggle_pause();
            }
            show_osd();
            return true;
        case LV_KEY_LEFT:
            player_.seek_by(-10);
            show_osd();
            return true;
        case LV_KEY_RIGHT:
            player_.seek_by(10);
            show_osd();
            return true;
        case LV_KEY_UP:
        case LV_KEY_DOWN: {
            volume_ = std::clamp(volume_ + (key == LV_KEY_UP ? 10 : -10), 0, 150);
            player_.set_volume(volume_);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Volume %d%%", volume_);
            view::set_label_text_if_changed(toast_, buf);
            toast_until_ = lv_tick_get() + 1200;
            if (timer_) lv_timer_ready(timer_);
            return true;
        }
        case 'r':
        case 'R':
            if (!error_.empty() || st.state == State::Error) {
                player_.stop();
                resolved_ = false;
                lv_obj_add_flag(video_, LV_OBJ_FLAG_HIDDEN);
                resolve();
            }
            return true;
        default:
            show_osd();
            return true;
    }
}

}  // namespace screen
