/*
 * SPDX-FileCopyrightText: 2026 ZeroTube contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "thumb_decoder.h"

#include <algorithm>
#include <cmath>

#if ZT_FLAG_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
#endif

namespace platform {

namespace {

void round_corners(std::vector<std::uint8_t>& px, int w, int h, int radius) {
    radius = std::min(radius, std::min(w, h) / 2);
    if (radius <= 0) return;
    const double r = radius;
    for (int y = 0; y < radius; ++y) {
        for (int x = 0; x < radius; ++x) {
            // Distance from the corner circle's centre, sampled at the pixel
            // centre; a one-pixel ramp keeps the edge smooth.
            const double dx = r - (x + 0.5);
            const double dy = r - (y + 0.5);
            const double d = std::sqrt(dx * dx + dy * dy);
            const double cover = std::clamp(r - d + 0.5, 0.0, 1.0);
            if (cover >= 1.0) continue;
            const auto a = static_cast<std::uint8_t>(std::lround(cover * 255));
            const int xs[2] = {x, w - 1 - x};
            const int ys[2] = {y, h - 1 - y};
            for (int yy : ys) {
                for (int xx : xs) {
                    auto& alpha = px[(static_cast<std::size_t>(yy) * w + xx) * 4 + 3];
                    alpha = std::min(alpha, a);
                }
            }
        }
    }
}

}  // namespace

#if !ZT_FLAG_HAVE_FFMPEG

bool decode_thumbnail(const std::string&, int, int, int, bool, std::vector<std::uint8_t>&) {
    return false;
}

#else

bool decode_thumbnail(const std::string& path, int width, int height, int radius, bool letterboxed,
                      std::vector<std::uint8_t>& out) {
    AVFormatContext* ic = nullptr;
    if (avformat_open_input(&ic, path.c_str(), nullptr, nullptr) < 0) return false;
    bool ok = false;
    AVCodecContext* dec = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    do {
        if (avformat_find_stream_info(ic, nullptr) < 0) break;
        const int index = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (index < 0) break;
        const AVCodec* codec = avcodec_find_decoder(ic->streams[index]->codecpar->codec_id);
        if (!codec) break;
        dec = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(dec, ic->streams[index]->codecpar);
        dec->thread_count = 1;
        if (avcodec_open2(dec, codec, nullptr) < 0) break;
        bool got = false;
        while (!got && av_read_frame(ic, pkt) >= 0) {
            if (pkt->stream_index == index && avcodec_send_packet(dec, pkt) >= 0) {
                got = avcodec_receive_frame(dec, frame) >= 0;
            }
            av_packet_unref(pkt);
        }
        if (!got) {
            avcodec_send_packet(dec, nullptr);
            got = avcodec_receive_frame(dec, frame) >= 0;
        }
        if (!got) break;

        // Source window: drop letterbox bars, then crop to the target aspect.
        int sx = 0, sy = 0, sw = frame->width, sh = frame->height;
        if (letterboxed && sw * 3 == sh * 4) {
            const int content = sw * 9 / 16;
            sy = (sh - content) / 2;
            sh = content;
        }
        const double target = static_cast<double>(width) / height;
        if (static_cast<double>(sw) / sh > target) {
            const int nw = static_cast<int>(std::lround(sh * target));
            sx += (sw - nw) / 2;
            sw = nw;
        } else {
            const int nh = static_cast<int>(std::lround(sw / target));
            sy += (sh - nh) / 2;
            sh = nh;
        }
        // Chroma planes are subsampled: keep the window on even lines/columns.
        sx &= ~1;
        sy &= ~1;
        sw &= ~1;
        sh &= ~1;
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
        if (!desc) break;
        const std::uint8_t* src[4] = {};
        int src_stride[4] = {};
        for (int p = 0; p < 4 && frame->data[p]; ++p) {
            const bool chroma = (p == 1 || p == 2) && !(desc->flags & AV_PIX_FMT_FLAG_RGB);
            const int x = chroma ? sx >> desc->log2_chroma_w : sx;
            const int y = chroma ? sy >> desc->log2_chroma_h : sy;
            const int step = (desc->flags & AV_PIX_FMT_FLAG_PLANAR) || p > 0 ? 1 : av_get_bits_per_pixel(desc) / 8;
            src[p] = frame->data[p] + static_cast<std::ptrdiff_t>(y) * frame->linesize[p] + x * std::max(1, step);
            src_stride[p] = frame->linesize[p];
        }
        SwsContext* sws = sws_getContext(sw, sh, static_cast<AVPixelFormat>(frame->format), width, height,
                                         AV_PIX_FMT_BGRA, SWS_AREA, nullptr, nullptr, nullptr);
        if (!sws) break;
        out.assign(static_cast<std::size_t>(width) * height * 4, 0xff);
        std::uint8_t* dst[4] = {out.data(), nullptr, nullptr, nullptr};
        int dst_stride[4] = {width * 4, 0, 0, 0};
        sws_scale(sws, src, src_stride, 0, sh, dst, dst_stride);
        sws_freeContext(sws);
        round_corners(out, width, height, radius);
        ok = true;
    } while (false);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    if (dec) avcodec_free_context(&dec);
    avformat_close_input(&ic);
    return ok;
}

#endif

}  // namespace platform
