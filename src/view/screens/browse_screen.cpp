/*
 * SPDX-FileCopyrightText: 2026 ZeroTube contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "browse_screen.h"

#ifndef APP_LAUNCHER_NAME
#define APP_LAUNCHER_NAME "ZeroTube"
#endif

#include "asset_manager.h"
#include "label_util.h"
#include "linux_input.h"
#include "theme.h"
#include "ui_const.h"
#include "youtube_service.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace screen {

namespace {

// ---- YouTube dark palette ---------------------------------------------------
constexpr uint32_t kBg = 0x0f0f0f;
constexpr uint32_t kRed = 0xff0033;
constexpr uint32_t kText = 0xf1f1f1;
constexpr uint32_t kMuted = 0xaaaaaa;
constexpr uint32_t kChip = 0x272727;
constexpr uint32_t kChipActive = 0xf1f1f1;
constexpr uint32_t kSelected = 0x272727;
constexpr uint32_t kPlaceholder = 0x222222;
constexpr uint32_t kSearchBg = 0x121212;
constexpr uint32_t kSearchBorder = 0x303030;
constexpr uint32_t kLink = 0x3ea6ff;

// ---- layout (320x170) -----------------------------------------------------
constexpr int kW = 320;
constexpr int kHeaderH = 28;
constexpr int kHintH = 15;
constexpr int kBodyH = 170 - kHeaderH - kHintH;
constexpr int kRowH = 42;
constexpr int kThumbW = 64;
constexpr int kThumbH = 36;

enum List { kHome = 0, kSubs = 1, kAccountList = 2, kSearch = 3, kListCount = 4 };

struct AccountFeed {
    const char* label;
    const char* icon;
    const char* feed;
};
constexpr AccountFeed kAccountFeeds[] = {
    {"History", view::ICON_CLOCK_COUNTER, "history"},
    {"Watch later", view::ICON_CLOCK_COUNTER, "watchlater"},
    {"Liked videos", view::ICON_THUMBS_UP, "liked"},
};
constexpr int kAccountFeedCount = 3;

enum class MenuAction { Feed0, Feed1, Feed2, SignIn, SignOut, Engine };

struct ListState {
    std::vector<net::Video> items;
    bool loaded = false;
    bool loading = false;
    bool needs_signin = false;
    std::string error;
    int selected = 0;
};

}  // namespace

struct BrowseState {
    int tab = 0;  // 0 For you, 1 Subs, 2 Account
    bool typing = false;
    std::string draft;
    std::string query;
    bool in_search = false;
    ListState lists[kListCount];
    int account_feed = -1;  // which of kAccountFeeds is open in the Account tab; -1 = the menu
    int menu_selected = 0;
    // account
    bool account_loaded = false;
    bool account_loading = false;
    std::string name, handle, account_error;
    bool expired = false;
    // sign-in page
    bool signin = false;
    bool signin_loading = false;
    std::vector<std::string> signin_urls;
    // engine
    std::string engine_step;
    bool engine_failed = false;
    bool last_signed_in = false;  // what the lists were loaded under

    BrowseScreen* live = nullptr;
    // Requests made for the state (not a screen) are owned by this, which
    // never expires: a feed that finishes while the player is open is kept.
    std::shared_ptr<int> owner = std::make_shared<int>(0);
};

namespace {

BrowseState& S() {
    static BrowseState state;
    return state;
}

void notify() {
    if (S().live) S().live->refresh();
}

int current_list() {
    auto& s = S();
    if (s.in_search) return kSearch;
    if (s.tab == 0) return kHome;
    if (s.tab == 1) return kSubs;
    return s.account_feed >= 0 ? kAccountList : -1;
}

std::string format_duration(int seconds) {
    if (seconds <= 0) return "";
    char buf[16];
    const int h = seconds / 3600, m = (seconds / 60) % 60, sec = seconds % 60;
    if (h > 0) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, sec);
    else std::snprintf(buf, sizeof(buf), "%d:%02d", m, sec);
    return buf;
}

std::string format_views(long long views) {
    if (views <= 0) return "";
    char buf[32];
    if (views >= 1000000000LL) std::snprintf(buf, sizeof(buf), "%.1fB views", views / 1e9);
    else if (views >= 1000000) std::snprintf(buf, sizeof(buf), "%.1fM views", views / 1e6);
    else if (views >= 1000) std::snprintf(buf, sizeof(buf), "%.0fK views", views / 1e3);
    else std::snprintf(buf, sizeof(buf), "%lld views", views);
    std::string out = buf;
    const auto dot0 = out.find(".0");
    if (dot0 != std::string::npos && dot0 + 2 < out.size() && !std::isdigit(static_cast<unsigned char>(out[dot0 + 2]))) {
        out.erase(dot0, 2);  // "1.0M" -> "1M"
    }
    return out;
}

void load_list(int list, const std::string& feed_or_query) {
    auto& s = S();
    auto& l = s.lists[list];
    l.loading = true;
    l.error.clear();
    l.needs_signin = false;
    net::YouTube::Json req;
    if (list == kSearch) {
        req = {{"cmd", "search"}, {"q", feed_or_query}, {"limit", 25}};
    } else {
        req = {{"cmd", "feed"}, {"feed", feed_or_query}, {"limit", 30}};
    }
    net::YouTube::instance().request(req, s.owner, [list](const net::YouTube::Json& reply) {
        auto& l = S().lists[list];
        l.loading = false;
        l.loaded = true;
        if (!reply.value("ok", false)) {
            l.error = reply.value("error", "Something went wrong");
        } else {
            l.items = net::parse_videos(reply);
            l.needs_signin = reply.value("needs_signin", false);
            l.selected = 0;
            std::vector<std::string> ids;
            for (const auto& v : l.items) ids.push_back(v.id);
            net::YouTube::instance().prefetch_thumbnails(ids);
        }
        notify();
    });
    notify();
}

const char* feed_for_list(int list) {
    if (list == kHome) return "home";
    if (list == kSubs) return "subs";
    if (list == kAccountList && S().account_feed >= 0) return kAccountFeeds[S().account_feed].feed;
    return "";
}

void load_account() {
    auto& s = S();
    if (s.account_loading) return;
    s.account_loading = true;
    net::YouTube::instance().request({{"cmd", "account"}}, s.owner, [](const net::YouTube::Json& reply) {
        auto& s = S();
        s.account_loading = false;
        s.account_loaded = true;
        s.account_error = reply.value("ok", false) ? reply.value("error", "") : reply.value("error", "Failed");
        s.name = reply.value("name", "");
        s.handle = reply.value("handle", "");
        s.expired = reply.value("expired", false);
        net::YouTube::instance().set_signed_in(reply.value("signed_in", false));
        net::YouTube::instance().load_avatar(reply.value("avatar", ""));
        notify();
    });
}

// What a fresh look at the current tab needs.
void ensure_loaded() {
    auto& s = S();
    auto& yt = net::YouTube::instance();
    if (!yt.engine_installed()) return;
    const int list = current_list();
    if (list >= 0 && list != kSearch) {
        auto& l = s.lists[list];
        if (!l.loaded && !l.loading) load_list(list, feed_for_list(list));
    }
    if (s.tab == 2 && yt.signed_in() && !s.account_loaded) load_account();
}

std::vector<MenuAction> menu_actions() {
    auto& yt = net::YouTube::instance();
    std::vector<MenuAction> out;
    if (!yt.engine_installed()) {
        out.push_back(MenuAction::Engine);
        return out;
    }
    if (yt.signed_in()) {
        out = {MenuAction::Feed0, MenuAction::Feed1, MenuAction::Feed2, MenuAction::SignOut};
    } else {
        out = {MenuAction::SignIn};
    }
    return out;
}

void style_plain(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* text) {
    auto* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

}  // namespace

// ---------------------------------------------------------------------------

BrowseScreen::BrowseScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
    S().live = this;
    platform::set_screen_key_handler(key_handler, this);
    if (S().typing) platform::set_text_input_mode(true);
    BrowseScreen* self = this;
    listener_ = net::YouTube::instance().add_listener(alive_, [self](const net::YouTube::Json& ev) {
        const std::string name = ev.value("ev", "");
        auto& s = S();
        auto& yt = net::YouTube::instance();
        if (name == "thumb_ready") {
            const std::string vid = ev.value("vid", "");
            if (vid == "@avatar") self->refresh();
            else self->thumbnail_ready(vid);
        } else if (name == "ready") {
            // New cookies (a sender that restarts the helper after uploading)
            // or removed ones: everything loaded under the old state is stale.
            if (yt.signed_in() != s.last_signed_in) {
                s.last_signed_in = yt.signed_in();
                s.account_loaded = false;
                s.name.clear();
                s.handle.clear();
                for (auto& l : s.lists) l = ListState{};
                s.account_feed = -1;
                s.menu_selected = 0;
            }
            if (!yt.engine_installed() && !yt.engine_busy() && !s.engine_failed) {
                yt.setup_engine();  // first run: fetch yt-dlp without asking
            }
            ensure_loaded();
            self->refresh();
        } else if (name == "engine_step") {
            s.engine_step = ev.value("text", "");
            self->refresh();
        } else if (name == "engine_done") {
            s.engine_failed = !ev.value("ok", false);
            s.engine_step = ev.value("text", "");
            self->refresh();
        } else if (name == "signed_in") {
            s.signin = false;
            yt.set_signed_in(true);
            s.last_signed_in = true;
            s.account_loaded = false;
            for (auto& l : s.lists) {
                l.loaded = false;  // everything changes once signed in
                l.items.clear();
            }
            s.menu_selected = 0;
            ensure_loaded();
            self->refresh();
        }
    });
}

BrowseScreen::~BrowseScreen() {
    net::YouTube::instance().remove_listener(listener_);
    platform::clear_screen_key_handler(this);
    if (S().live == this) S().live = nullptr;
}

const lv_font_t* BrowseScreen::font(const char* file, uint32_t size, const lv_font_t* fallback) {
    const lv_font_t* f = assets().load_font(file, size);
    return f ? f : fallback;
}

void BrowseScreen::build_content(lv_obj_t* content) {
    f_logo_ = font("inter-bold.ttf", 15, &lv_font_montserrat_14);
    f_chip_ = font("inter-medium.ttf", 11, &lv_font_montserrat_12);
    f_title_ = font("inter-medium.ttf", 12, &lv_font_montserrat_12);
    f_small_ = font("inter-regular.ttf", 10, &lv_font_montserrat_12);
    f_big_ = font("inter-semibold.ttf", 14, &lv_font_montserrat_14);
    f_icon_ = font("Phosphor-Fill.ttf", 12, &lv_font_montserrat_12);
    f_icon_big_ = font("Phosphor-Fill.ttf", 28, &lv_font_montserrat_28);

    lv_obj_set_style_bg_color(content, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);

    build_header(content);

    body_ = lv_obj_create(content);
    style_plain(body_);
    lv_obj_set_size(body_, kW, kBodyH);
    lv_obj_set_pos(body_, 0, kHeaderH);

    hint_ = make_label(content, f_small_, kMuted, "");
    lv_obj_set_width(hint_, kW - 16);
    lv_label_set_long_mode(hint_, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(hint_, 8, 170 - kHintH + 1);

    ensure_loaded();
    refresh();
}

void BrowseScreen::build_header(lv_obj_t* parent) {
    header_ = lv_obj_create(parent);
    style_plain(header_);
    lv_obj_set_size(header_, kW, kHeaderH);
    lv_obj_set_pos(header_, 0, 0);

    // Logo: the red rounded play button and the wordmark.
    logo_ = lv_obj_create(header_);
    style_plain(logo_);
    lv_obj_set_size(logo_, 86, kHeaderH);
    lv_obj_set_pos(logo_, 6, 0);
    auto* badge = lv_obj_create(logo_);
    style_plain(badge);
    lv_obj_set_size(badge, 21, 15);
    lv_obj_align(badge, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(badge, 4, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(kRed), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    auto* tri = make_label(badge, f_icon_, 0xffffff, view::ICON_PLAY);
    lv_obj_set_style_text_font(tri, font("Phosphor-Fill.ttf", 9, &lv_font_montserrat_12), 0);
    lv_obj_center(tri);
    auto* word = make_label(logo_, f_logo_, kText, APP_LAUNCHER_NAME);
    lv_obj_set_style_text_letter_space(word, -1, 0);
    lv_obj_align(word, LV_ALIGN_LEFT_MID, 24, -1);

    // Tabs as chips, right of the logo.
    chips_ = lv_obj_create(header_);
    style_plain(chips_);
    lv_obj_set_size(chips_, LV_SIZE_CONTENT, kHeaderH);
    lv_obj_set_flex_flow(chips_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chips_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(chips_, 4, 0);
    lv_obj_align(chips_, LV_ALIGN_RIGHT_MID, -26, 0);
    static const char* const kNames[3] = {"For you", "Subs", "Account"};
    for (int i = 0; i < 3; ++i) {
        auto* chip = lv_obj_create(chips_);
        style_plain(chip);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, 20);
        lv_obj_set_style_radius(chip, 7, 0);
        lv_obj_set_style_pad_hor(chip, 8, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        auto* label = make_label(chip, f_chip_, kText, kNames[i]);
        lv_obj_center(label);
        chip_[i] = chip;
    }

    search_icon_ = make_label(header_, font("Phosphor-Fill.ttf", 15, &lv_font_montserrat_14), kText,
                              view::ICON_MAGNIFYING_GLASS);
    lv_obj_align(search_icon_, LV_ALIGN_RIGHT_MID, -7, 0);

    // The search field, shown instead of the logo and chips while typing.
    search_box_ = lv_obj_create(header_);
    style_plain(search_box_);
    lv_obj_set_size(search_box_, kW - 12, 22);
    lv_obj_align(search_box_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(search_box_, 11, 0);
    lv_obj_set_style_bg_color(search_box_, lv_color_hex(kSearchBg), 0);
    lv_obj_set_style_bg_opa(search_box_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(search_box_, lv_color_hex(kSearchBorder), 0);
    lv_obj_set_style_border_width(search_box_, 1, 0);
    auto* glass = make_label(search_box_, f_icon_, kMuted, view::ICON_MAGNIFYING_GLASS);
    lv_obj_align(glass, LV_ALIGN_LEFT_MID, 9, 0);
    search_text_ = make_label(search_box_, f_title_, kText, "");
    lv_obj_set_width(search_text_, kW - 50);
    lv_label_set_long_mode(search_text_, LV_LABEL_LONG_CLIP);
    lv_obj_align(search_text_, LV_ALIGN_LEFT_MID, 27, 0);
}

void BrowseScreen::update_header() {
    auto& s = S();
    const bool typing = s.typing;
    view::set_visible_if_changed(logo_, !typing);
    view::set_visible_if_changed(chips_, !typing);
    view::set_visible_if_changed(search_icon_, !typing);
    view::set_visible_if_changed(search_box_, typing);
    for (int i = 0; i < 3; ++i) {
        // In search results no tab is lit: the list is not any of them.
        const bool active = !s.in_search && s.tab == i;
        lv_obj_set_style_bg_color(chip_[i], lv_color_hex(active ? kChipActive : kChip), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(chip_[i], 0), lv_color_hex(active ? 0x0f0f0f : kText), 0);
    }
    if (typing) set_query_text();
}

void BrowseScreen::set_query_text() {
    const auto& d = S().draft;
    if (d.empty()) {
        lv_obj_set_style_text_color(search_text_, lv_color_hex(0x717171), 0);
        view::set_label_text_if_changed(search_text_, "Search YouTube");
    } else {
        lv_obj_set_style_text_color(search_text_, lv_color_hex(kText), 0);
        // Keep the end (where you type) in view.
        std::string shown = d + "|";
        view::set_label_text_if_changed(search_text_, shown.c_str());
    }
}

void BrowseScreen::refresh() {
    if (!body_) return;
    update_header();
    rebuild_body();
    update_hint();
}

void BrowseScreen::update_hint() {
    auto& s = S();
    const char* text;
    if (s.typing) {
        text = "Type to search   OK search   Esc cancel";
    } else if (s.signin) {
        text = "Esc stop signing in";
    } else if (current_list() >= 0 && !s.lists[current_list()].items.empty()) {
        text = s.in_search ? "OK play   S search   Esc back" : "OK play   S search   R refresh   1 2 3 tabs";
        if (s.tab == 2 && !s.in_search) text = "OK play   R refresh   Esc back";
    } else if (s.tab == 2) {
        const auto& yt = net::YouTube::instance();
        static char buf[96];
        if (yt.engine_installed()) {
            std::snprintf(buf, sizeof(buf), "OK open   U update yt-dlp %s", yt.engine_version().c_str());
        } else {
            std::snprintf(buf, sizeof(buf), "OK install");
        }
        text = buf;
    } else {
        text = "S search   1 2 3 tabs";
    }
    view::set_label_text_if_changed(hint_, text);
}

void BrowseScreen::build_message(const char* icon, const char* title, const char* detail, bool spinner) {
    auto* box = lv_obj_create(body_);
    style_plain(box);
    lv_obj_set_size(box, kW - 24, kBodyH);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 4, 0);
    if (spinner) {
        auto* sp = lv_spinner_create(box);
        lv_obj_set_size(sp, 26, 26);
        lv_obj_set_style_arc_width(sp, 3, LV_PART_MAIN);
        lv_obj_set_style_arc_width(sp, 3, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(sp, lv_color_hex(0x303030), LV_PART_MAIN);
        lv_obj_set_style_arc_color(sp, lv_color_hex(kRed), LV_PART_INDICATOR);
    } else if (icon) {
        make_label(box, f_icon_big_, kMuted, icon);
    }
    if (title && *title) {
        auto* t = make_label(box, f_big_, kText, title);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    }
    if (detail && *detail) {
        auto* d = make_label(box, f_small_, kMuted, detail);
        lv_obj_set_width(d, kW - 40);
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
    }
}

void BrowseScreen::rebuild_body() {
    lv_obj_clean(body_);
    rows_.clear();
    menu_rows_.clear();
    shown_list_ = -1;
    auto& s = S();
    auto& yt = net::YouTube::instance();

    if (!yt.ready()) {
        build_message(nullptr, "Starting", "", true);
        return;
    }
    if (!yt.engine_installed()) {
        if (yt.engine_busy()) {
            build_message(nullptr, "Setting up YouTube", s.engine_step.empty() ? "Getting yt-dlp" : s.engine_step.c_str(),
                          true);
        } else {
            build_message(view::ICON_DOWNLOAD, "YouTube engine needed",
                          s.engine_failed ? "Could not download yt-dlp. Check Wi-Fi, then press OK to try again."
                                          : "Press OK to download yt-dlp (about 15 MB).");
        }
        return;
    }
    if (s.signin) {
        build_signin();
        return;
    }
    const int list = current_list();
    if (list < 0) {
        build_account();
        return;
    }
    auto& l = s.lists[list];
    if (l.loading && l.items.empty()) {
        build_message(nullptr, "", "", true);
        return;
    }
    if (!l.error.empty() && l.items.empty()) {
        build_message(view::ICON_WARNING, "Couldn't load", (l.error + "\nR to try again").c_str());
        return;
    }
    if (l.needs_signin) {
        build_message(view::ICON_USER_CIRCLE, list == kHome ? "Videos picked for you" : "Your subscriptions",
                      "Sign in on the Account tab (3) to see them. S searches without signing in.");
        return;
    }
    if (l.loaded && l.items.empty()) {
        build_message(view::ICON_MAGNIFYING_GLASS, list == kSearch ? "No results" : "Nothing here yet",
                      list == kSearch ? "Try other words." : "");
        return;
    }
    if (!l.loaded) {
        build_message(view::ICON_MAGNIFYING_GLASS, "Search YouTube", "Press S and type.");
        return;
    }
    build_list(list);
}

void BrowseScreen::build_list(int list) {
    auto& l = S().lists[list];
    shown_list_ = list;
    auto* scroller = lv_obj_create(body_);
    style_plain(scroller);
    lv_obj_set_size(scroller, kW, kBodyH);
    lv_obj_add_flag(scroller, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scroller, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(scroller, LV_DIR_VER);

    auto& yt = net::YouTube::instance();
    int y = 0;
    for (const auto& v : l.items) {
        Row row;
        row.vid = v.id;
        auto* r = lv_obj_create(scroller);
        style_plain(r);
        lv_obj_set_size(r, kW - 4, kRowH);
        lv_obj_set_pos(r, 2, y);
        lv_obj_set_style_radius(r, 8, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(kSelected), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        row.obj = r;

        auto* thumb = lv_image_create(r);
        lv_obj_set_pos(thumb, 4, 3);
        lv_obj_set_size(thumb, kThumbW, kThumbH);
        lv_obj_set_style_radius(thumb, 4, 0);
        lv_obj_set_style_bg_color(thumb, lv_color_hex(kPlaceholder), 0);
        lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
        if (const auto* dsc = yt.thumbnail(v.id)) {
            lv_image_set_src(thumb, dsc);
            lv_obj_set_style_bg_opa(thumb, LV_OPA_TRANSP, 0);
        }
        row.thumb = thumb;

        std::string badge = v.live ? "LIVE" : format_duration(v.duration);
        if (!badge.empty()) {
            auto* b = make_label(r, f_small_, 0xffffff, badge.c_str());
            lv_obj_set_style_bg_color(b, lv_color_hex(v.live ? 0xcc0000 : 0x000000), 0);
            lv_obj_set_style_bg_opa(b, v.live ? LV_OPA_COVER : LV_OPA_80, 0);
            lv_obj_set_style_radius(b, 2, 0);
            lv_obj_set_style_pad_hor(b, 2, 0);
            lv_obj_align_to(b, thumb, LV_ALIGN_BOTTOM_RIGHT, -1, -1);
        }

        auto* title = make_label(r, f_title_, kText, v.title.c_str());
        lv_obj_set_pos(title, 74, 1);
        lv_obj_set_size(title, kW - 4 - 74 - 4, 29);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_line_space(title, -2, 0);

        std::string meta = v.channel;
        const std::string views = format_views(v.views);
        if (!views.empty()) meta += (meta.empty() ? "" : " \xC2\xB7 ") + views;
        auto* channel = make_label(r, f_small_, kMuted, meta.c_str());
        lv_obj_set_pos(channel, 74, 29);
        lv_obj_set_height(channel, 13);
        lv_obj_set_width(channel, kW - 4 - 74 - 4);
        lv_label_set_long_mode(channel, LV_LABEL_LONG_DOT);

        rows_.push_back(row);
        y += kRowH;
    }
    l.selected = std::clamp(l.selected, 0, static_cast<int>(rows_.size()) - 1);
    select_row(l.selected, true);
}

void BrowseScreen::select_row(int index, bool scroll) {
    if (rows_.empty() || shown_list_ < 0) return;
    auto& l = S().lists[shown_list_];
    if (l.selected >= 0 && l.selected < static_cast<int>(rows_.size())) {
        lv_obj_set_style_bg_opa(rows_[l.selected].obj, LV_OPA_TRANSP, 0);
    }
    l.selected = std::clamp(index, 0, static_cast<int>(rows_.size()) - 1);
    auto* row = rows_[l.selected].obj;
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    if (scroll) lv_obj_scroll_to_view(row, LV_ANIM_OFF);
}

void BrowseScreen::thumbnail_ready(const std::string& vid) {
    auto& yt = net::YouTube::instance();
    for (auto& row : rows_) {
        if (row.vid != vid) continue;
        if (const auto* dsc = yt.thumbnail(vid)) {
            lv_image_set_src(row.thumb, dsc);
            lv_obj_set_style_bg_opa(row.thumb, LV_OPA_TRANSP, 0);
        }
    }
}

void BrowseScreen::build_account() {
    auto& s = S();
    auto& yt = net::YouTube::instance();

    // Profile card.
    auto* card = lv_obj_create(body_);
    style_plain(card);
    lv_obj_set_size(card, kW, 44);
    lv_obj_set_pos(card, 0, 0);
    auto* avatar = lv_image_create(card);
    lv_obj_set_size(avatar, 32, 32);
    lv_obj_set_pos(avatar, 10, 6);
    if (yt.signed_in() && yt.avatar()) {
        lv_image_set_src(avatar, yt.avatar());
    } else {
        lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(avatar, lv_color_hex(yt.signed_in() ? 0x5a3fbf : kChip), 0);
        lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
        auto* glyph = make_label(avatar, font("Phosphor-Fill.ttf", 20, &lv_font_montserrat_20), kMuted,
                                 view::ICON_USER_CIRCLE);
        lv_obj_center(glyph);
    }
    std::string name, sub;
    if (!yt.signed_in()) {
        name = "Not signed in";
        sub = "Search works without an account";
    } else if (s.account_loading && !s.account_loaded) {
        name = "Signed in";
        sub = "Loading your account...";
    } else if (s.expired) {
        name = "Sign-in expired";
        sub = "YouTube no longer accepts these cookies";
    } else {
        name = s.name.empty() ? "Signed in" : s.name;
        sub = !s.handle.empty() ? s.handle : s.account_error;
    }
    auto* n = make_label(card, f_big_, kText, name.c_str());
    lv_obj_set_pos(n, 52, 7);
    lv_obj_set_width(n, kW - 60);
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    auto* h = make_label(card, f_small_, kMuted, sub.c_str());
    lv_obj_set_pos(h, 52, 26);
    lv_obj_set_width(h, kW - 60);
    lv_label_set_long_mode(h, LV_LABEL_LONG_DOT);

    // Menu.
    const auto actions = menu_actions();
    s.menu_selected = std::clamp(s.menu_selected, 0, static_cast<int>(actions.size()) - 1);
    int y = 46;
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const auto a = actions[i];
        const char* icon = view::ICON_ARROW_CLOCKWISE;
        std::string label;
        uint32_t color = kText;
        switch (a) {
            case MenuAction::Feed0:
            case MenuAction::Feed1:
            case MenuAction::Feed2: {
                const auto& f = kAccountFeeds[static_cast<int>(a)];
                icon = f.icon;
                label = f.label;
                break;
            }
            case MenuAction::SignIn:
                icon = view::ICON_USER_CIRCLE;
                label = "Sign in with your phone or computer";
                color = kLink;
                break;
            case MenuAction::SignOut:
                icon = view::ICON_SIGN_OUT;
                label = "Sign out";
                break;
            case MenuAction::Engine:
                icon = view::ICON_DOWNLOAD;
                label = "Install the YouTube engine";
                break;
        }
        auto* r = lv_obj_create(body_);
        style_plain(r);
        lv_obj_set_size(r, kW - 4, 20);
        lv_obj_set_pos(r, 2, y);
        lv_obj_set_style_radius(r, 6, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(kSelected), 0);
        lv_obj_set_style_bg_opa(r, static_cast<int>(i) == s.menu_selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        auto* ic = make_label(r, f_icon_, kText, icon);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 16, 0);
        auto* t = make_label(r, f_title_, color, label.c_str());
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 38, 0);
        menu_rows_.push_back(r);
        y += 20;
    }
}

void BrowseScreen::build_signin() {
    auto& s = S();
    if (s.signin_loading || s.signin_urls.empty()) {
        build_message(nullptr, "", "", true);
        return;
    }
    const std::string& url = s.signin_urls.front();
    auto* qr = lv_qrcode_create(body_);
    lv_qrcode_set_size(qr, 100);
    lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(qr, lv_color_hex(0xffffff));
    lv_qrcode_update(qr, url.data(), static_cast<uint32_t>(url.size()));
    lv_obj_set_style_border_color(qr, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_width(qr, 4, 0);
    lv_obj_set_pos(qr, 10, 6);

    auto* t = make_label(body_, f_big_, kText, "Sign in");
    lv_obj_set_pos(t, 124, 4);
    std::string text = "Scan or open (same network or Tailscale):";
    for (const auto& u : s.signin_urls) text += "\n" + u;
    text += "\nthen send your YouTube cookies.txt";
    auto* d = make_label(body_, f_small_, kMuted, text.c_str());
    lv_obj_set_pos(d, 124, 24);
    lv_obj_set_width(d, kW - 130);
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
}

// ---- keys -------------------------------------------------------------------

bool BrowseScreen::key_handler(uint32_t key, void* user_data) {
    return static_cast<BrowseScreen*>(user_data)->on_key(key);
}

void BrowseScreen::start_search() {
    auto& s = S();
    s.typing = true;
    s.draft = s.in_search ? s.query : "";
    platform::set_text_input_mode(true);
    refresh();
}

void BrowseScreen::submit_search() {
    auto& s = S();
    s.typing = false;
    platform::set_text_input_mode(false);
    if (s.draft.empty()) {
        refresh();
        return;
    }
    s.query = s.draft;
    s.in_search = true;
    s.lists[kSearch].items.clear();
    s.lists[kSearch].loaded = false;
    load_list(kSearch, s.query);
}

bool BrowseScreen::on_search_key(uint32_t key) {
    auto& s = S();
    if (key == LV_KEY_ESC) {
        s.typing = false;
        platform::set_text_input_mode(false);
        refresh();
        return true;
    }
    if (key == LV_KEY_ENTER || key == '\r' || key == '\n') {
        submit_search();
        return true;
    }
    if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
        // Drop one UTF-8 character.
        while (!s.draft.empty()) {
            const unsigned char c = static_cast<unsigned char>(s.draft.back());
            s.draft.pop_back();
            if ((c & 0xC0) != 0x80) break;
        }
        set_query_text();
        return true;
    }
    if (key >= 32 && key < 127 && s.draft.size() < 80) {
        s.draft.push_back(static_cast<char>(key));
        set_query_text();
        return true;
    }
    return true;  // everything else is swallowed while typing
}

bool BrowseScreen::on_account_key(uint32_t key) {
    auto& s = S();
    auto& yt = net::YouTube::instance();
    const auto actions = menu_actions();
    switch (key) {
        case LV_KEY_UP:
            if (s.menu_selected > 0) {
                --s.menu_selected;
                rebuild_body();
            }
            return true;
        case LV_KEY_DOWN:
            if (s.menu_selected + 1 < static_cast<int>(actions.size())) {
                ++s.menu_selected;
                rebuild_body();
            }
            return true;
        case 'u':
        case 'U':
            if (!yt.engine_busy()) {
                yt.setup_engine();
                s.engine_step = "Updating yt-dlp";
                refresh();
            }
            return true;
        case LV_KEY_ENTER: {
            if (actions.empty()) return true;
            const auto a = actions[static_cast<std::size_t>(s.menu_selected)];
            switch (a) {
                case MenuAction::Feed0:
                case MenuAction::Feed1:
                case MenuAction::Feed2: {
                    const int f = static_cast<int>(a);
                    if (s.account_feed != f) {
                        s.lists[kAccountList] = ListState{};
                    }
                    s.account_feed = f;
                    load_list(kAccountList, kAccountFeeds[f].feed);
                    return true;
                }
                case MenuAction::SignIn:
                    s.signin = true;
                    s.signin_loading = true;
                    yt.request({{"cmd", "signin_start"}}, s.owner, [](const net::YouTube::Json& reply) {
                        auto& s = S();
                        s.signin_loading = false;
                        s.signin_urls.clear();
                        if (reply.contains("urls")) {
                            for (const auto& u : reply["urls"]) s.signin_urls.push_back(u.get<std::string>());
                        }
                        if (s.signin_urls.empty()) s.signin = false;
                        notify();
                    });
                    refresh();
                    return true;
                case MenuAction::SignOut:
                    yt.request({{"cmd", "signout"}}, s.owner, [](const net::YouTube::Json&) {
                        auto& s = S();
                        net::YouTube::instance().set_signed_in(false);
                        net::YouTube::instance().load_avatar("");
                        S().last_signed_in = false;
                        s.account_loaded = false;
                        s.name.clear();
                        s.handle.clear();
                        for (auto& l : s.lists) l = ListState{};
                        s.menu_selected = 0;
                        notify();
                    });
                    return true;
                case MenuAction::Engine:
                    s.engine_failed = false;
                    yt.setup_engine();
                    refresh();
                    return true;
            }
            return true;
        }
        default:
            return false;
    }
}

bool BrowseScreen::on_key(uint32_t key) {
    auto& s = S();
    auto& yt = net::YouTube::instance();
    if (s.typing) return on_search_key(key);

    if (s.signin) {
        if (key == LV_KEY_ESC) {
            s.signin = false;
            yt.request({{"cmd", "signin_stop"}}, s.owner, nullptr);
            refresh();
        }
        return true;
    }

    switch (key) {
        case '1':
        case '2':
        case '3':
            s.in_search = false;
            s.tab = static_cast<int>(key - '1');
            ensure_loaded();
            refresh();
            return true;
        case LV_KEY_LEFT:
        case LV_KEY_RIGHT:
            if (s.in_search) {
                s.in_search = false;
            } else {
                s.tab = (s.tab + (key == LV_KEY_RIGHT ? 1 : 2)) % 3;
            }
            ensure_loaded();
            refresh();
            return true;
        case 's':
        case 'S':
        case '/':
            start_search();
            return true;
        default:
            break;
    }

    if (!yt.engine_installed()) {
        if (key == LV_KEY_ENTER && !yt.engine_busy() && yt.ready()) {
            s.engine_failed = false;
            yt.setup_engine();
            refresh();
        }
        return key == LV_KEY_ENTER;
    }

    const int list = current_list();
    if (list < 0) return on_account_key(key);

    auto& l = s.lists[list];
    switch (key) {
        case LV_KEY_UP:
            select_row(l.selected - 1, true);
            return true;
        case LV_KEY_DOWN:
            select_row(l.selected + 1, true);
            return true;
        case LV_KEY_ENTER:
            open_selected();
            return true;
        case 'r':
        case 'R':
            if (!l.loading) {
                if (list == kSearch) load_list(kSearch, s.query);
                else load_list(list, feed_for_list(list));
            }
            return true;
        case LV_KEY_ESC:
            if (s.in_search) {
                s.in_search = false;
                refresh();
                return true;
            }
            if (list == kAccountList) {
                s.account_feed = -1;
                refresh();
                return true;
            }
            return false;
        default:
            return false;
    }
}

void BrowseScreen::open_selected() {
    if (shown_list_ < 0) return;
    auto& l = S().lists[shown_list_];
    if (l.selected < 0 || l.selected >= static_cast<int>(l.items.size())) return;
    const auto& v = l.items[static_cast<std::size_t>(l.selected)];
    view_model().play({v.id, v.title, v.channel, v.duration});
}

}  // namespace screen
