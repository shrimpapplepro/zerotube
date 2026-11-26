/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 * SPDX-FileCopyrightText: 2026 ZeroTube contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>
#include <utility>

namespace model {

enum class AppPage {
    Browse = 0,  // For you / Subs / Account, search results
    Player = 1,
};

// The video the player screen opens.
struct PlayRequest {
    std::string id;
    std::string title;
    std::string channel;
    int duration = 0;
};

class BaseModel {
public:
    const char* app_title() const;

    bool dark_mode() const;
    void set_dark_mode(bool enabled);
    void toggle_dark_mode();

    AppPage current_page() const;
    void set_current_page(AppPage page);

    const PlayRequest& play_request() const { return play_request_; }
    void set_play_request(PlayRequest request) { play_request_ = std::move(request); }

private:
    PlayRequest play_request_;
    bool dark_mode_ = true;
    AppPage current_page_ = AppPage::Browse;
};

} // namespace model
