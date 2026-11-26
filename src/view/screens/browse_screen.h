/*
 * SPDX-FileCopyrightText: 2026 ZeroTube contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_screen.h"

#include <memory>
#include <string>
#include <vector>

namespace screen {

struct BrowseState;

// The whole app except the player: a YouTube-style header (logo, the three
// tabs as chips, search), a list of videos with tiny thumbnails, and the
// Account tab. Its state lives outside the screen (BrowseState), so coming
// back from the player lands on the same list, row and tab.
class BrowseScreen : public BaseScreen {
public:
    BrowseScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);
    ~BrowseScreen() override;

    // Called by the state when a list or the account changes.
    void refresh();
    void thumbnail_ready(const std::string& vid);

protected:
    void build_content(lv_obj_t* content) override;

private:
    static bool key_handler(uint32_t key, void* user_data);
    bool on_key(uint32_t key);
    bool on_search_key(uint32_t key);
    bool on_account_key(uint32_t key);

    void build_header(lv_obj_t* parent);
    void update_header();
    void rebuild_body();
    void build_list(int list);
    void build_message(const char* icon, const char* title, const char* detail, bool spinner = false);
    void build_account();
    void build_signin();
    void update_hint();
    void select_row(int index, bool scroll);
    void open_selected();
    void start_search();
    void submit_search();
    void set_query_text();

    const lv_font_t* font(const char* file, uint32_t size, const lv_font_t* fallback);

    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    int listener_ = 0;

    lv_obj_t* header_ = nullptr;
    lv_obj_t* logo_ = nullptr;
    lv_obj_t* chips_ = nullptr;
    lv_obj_t* chip_[3] = {};
    lv_obj_t* search_icon_ = nullptr;
    lv_obj_t* search_box_ = nullptr;
    lv_obj_t* search_text_ = nullptr;
    lv_obj_t* body_ = nullptr;
    lv_obj_t* hint_ = nullptr;

    struct Row {
        lv_obj_t* obj = nullptr;
        lv_obj_t* thumb = nullptr;
        std::string vid;
    };
    std::vector<Row> rows_;
    std::vector<lv_obj_t*> menu_rows_;
    int shown_list_ = -1;

    const lv_font_t* f_logo_ = nullptr;
    const lv_font_t* f_chip_ = nullptr;
    const lv_font_t* f_title_ = nullptr;
    const lv_font_t* f_small_ = nullptr;
    const lv_font_t* f_big_ = nullptr;
    const lv_font_t* f_icon_ = nullptr;
    const lv_font_t* f_icon_big_ = nullptr;
};

}  // namespace screen
