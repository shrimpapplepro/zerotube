/*
 * SPDX-FileCopyrightText: 2026 ZeroTube contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace platform {

// Decodes a small JPEG (a YouTube thumbnail or avatar) into premultiplied-free
// ARGB8888 (B,G,R,A in memory, LVGL's LV_COLOR_FORMAT_ARGB8888) at exactly
// width x height, cropping to fill. `radius` rounds the corners in the alpha
// channel (radius >= min(w,h)/2 gives a circle), so LVGL draws it with no
// clip-corner pass. `letterboxed` drops the black bars of YouTube's 4:3
// default.jpg (a 16:9 picture in 120x90). Blocking: call off the LVGL thread.
bool decode_thumbnail(const std::string& path, int width, int height, int radius, bool letterboxed,
                      std::vector<std::uint8_t>& out);

}  // namespace platform
