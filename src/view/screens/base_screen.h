/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_viewmodel.h"
#include "lvgl.h"

#include <memory>

namespace app {
class AssetManager;
}

namespace screen {

class BaseScreen {
public:
    BaseScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);
    virtual ~BaseScreen();

    BaseScreen(const BaseScreen&) = delete;
    BaseScreen& operator=(const BaseScreen&) = delete;

    void init();
    lv_obj_t* root() const;

protected:
    virtual void build_content(lv_obj_t* content) = 0;

    viewmodel::BaseViewModel& view_model();
    app::AssetManager& assets();

private:
    viewmodel::BaseViewModel& view_model_;
    app::AssetManager& assets_;
    lv_obj_t* root_{nullptr};
    lv_obj_t* content_{nullptr};
};

} // namespace screen
