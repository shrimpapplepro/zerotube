/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "asset_manager.h"

#include "logger.h"

#if LV_USE_FREETYPE
#include "src/libs/freetype/lv_freetype.h"
#endif

#include <algorithm>
#include <utility>

#ifndef APP_NAME
#define APP_NAME "zerotube"
#endif

#ifndef APP_CMAKE_ASSETS_ROOT
#define APP_CMAKE_ASSETS_ROOT ""
#endif

#ifndef APP_SOURCE_ASSETS_ROOT
#define APP_SOURCE_ASSETS_ROOT "assets"
#endif

namespace app {
namespace {

bool path_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

// Inter has no CJK. Song tags, file names and chat text in Japanese or Chinese
// fall back to a system face (the CP0 image ships fonts-noto-cjk; index 0 of the
// .ttc is the Japanese variant). The first file that exists wins.
std::filesystem::path cjk_fallback_path(bool heavy) {
    const char* const candidates[] = {
        heavy ? "/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc"
              : "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",  // the macOS simulator
    };
    for (const char* candidate : candidates) {
        if (path_exists(candidate)) return candidate;
    }
    return {};
}

std::filesystem::path weakly_canonical_path(const std::filesystem::path& path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canonical;
}

} // namespace

struct AssetManager::LoadedFont {
    std::filesystem::path path;
    uint32_t size;
    lv_font_t* font;
};

AssetManager::AssetManager(std::string app_name, std::filesystem::path cmake_assets_root) {
    add_root_if_valid(std::move(cmake_assets_root));
    add_root_if_valid(default_cmake_assets_root());
    add_root_if_valid(APP_SOURCE_ASSETS_ROOT);
    add_root_if_valid(std::filesystem::path{"/usr/share"} / app_name);
}

AssetManager::~AssetManager() {
#if LV_USE_FREETYPE
    for (auto& loaded_font : loaded_fonts_) {
        if (loaded_font && loaded_font->font) {
            lv_freetype_font_delete(loaded_font->font);
        }
    }

#endif
}

std::string AssetManager::default_app_name() {
    return APP_NAME;
}

std::filesystem::path AssetManager::default_cmake_assets_root() {
    return APP_CMAKE_ASSETS_ROOT;
}

const std::vector<std::filesystem::path>& AssetManager::roots() const {
    return roots_;
}

std::filesystem::path AssetManager::resolve(const std::filesystem::path& relative_path) const {
    if (relative_path.is_absolute() && path_exists(relative_path)) {
        return weakly_canonical_path(relative_path);
    }

    for (const auto& root : roots_) {
        auto candidate = root / relative_path;
        if (path_exists(candidate)) {
            return weakly_canonical_path(candidate);
        }
    }

    return {};
}

std::filesystem::path AssetManager::resolve_font(const std::filesystem::path& file_name) const {
    return resolve(std::filesystem::path{"fonts"} / file_name);
}

lv_font_t* AssetManager::load_font(const std::filesystem::path& file_name, uint32_t size) {
#if LV_USE_FREETYPE
    auto path = resolve_font(file_name);
    if (path.empty()) {
        LOG_WARN("font asset not found: {}", file_name.string());
        return nullptr;
    }
    lv_font_t* font = load_font_file(path, size);
    // Text faces get the CJK fallback; icon fonts (Phosphor) do not need it.
    const std::string name = file_name.filename().string();
    if (font && !font->fallback && name.rfind("Phosphor", 0) != 0) {
        const bool heavy = name.find("medium") != std::string::npos || name.find("bold") != std::string::npos;
        const auto cjk = cjk_fallback_path(heavy);
        if (!cjk.empty()) font->fallback = load_font_file(cjk, size);
    }
    return font;
#else
    LV_UNUSED(file_name);
    LV_UNUSED(size);
    LOG_WARN("FreeType font loading requested but LV_USE_FREETYPE is disabled");
    return nullptr;
#endif
}

lv_font_t* AssetManager::load_font_file(const std::filesystem::path& path, uint32_t size) {
#if LV_USE_FREETYPE
    auto existing = std::find_if(loaded_fonts_.begin(), loaded_fonts_.end(), [&](const auto& loaded_font) {
        return loaded_font && loaded_font->path == path && loaded_font->size == size;
    });
    if (existing != loaded_fonts_.end()) {
        return (*existing)->font;
    }

    lv_font_t* font = lv_freetype_font_create(path.string().c_str(),
                                              LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                              size,
                                              LV_FREETYPE_FONT_STYLE_NORMAL);
    if (!font) {
        LOG_WARN("failed to load freetype font: {}", path.string());
        return nullptr;
    }

    loaded_fonts_.push_back(std::make_unique<LoadedFont>(LoadedFont{path, size, font}));
    LOG_INFO("loaded freetype font: {} ({})", path.string(), size);
    return font;
#else
    LV_UNUSED(path);
    LV_UNUSED(size);
    LOG_WARN("FreeType font loading requested but LV_USE_FREETYPE is disabled");
    return nullptr;
#endif
}

void AssetManager::add_root_if_valid(std::filesystem::path path) {
    if (path.empty()) {
        return;
    }

    path = weakly_canonical_path(path);
    if (!path_exists(path)) {
        return;
    }

    auto it = std::find(roots_.begin(), roots_.end(), path);
    if (it == roots_.end()) {
        roots_.push_back(std::move(path));
    }
}

} // namespace app
