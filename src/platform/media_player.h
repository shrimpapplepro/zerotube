/*
 * SPDX-FileCopyrightText: 2026 ZeroTube contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Video playback on FFmpeg.
// `audio_location` adds a second input carrying the sound, the way YouTube
// serves video and audio as separate streams.
//
// Anything libavformat can demux and libavcodec can decode plays: on the CP0's
// Debian FFmpeg 7.1 that is H.264/HEVC/VP8/VP9/AV1/MPEG-1/2/4/MJPEG/Theora...
// in MP4/MKV/WebM/AVI/MOV/TS/FLV..., and MP3/AAC/FLAC/ALAC/Opus/Vorbis/WAV/
// APE/WMA/DSD... audio. Pictures (PNG/JPEG/WebP/BMP/GIF/TIFF) go through the
// same path as one-frame videos; animated GIF/WebP play.
//
// Files are read through platform::vfs, so an SMB share streams directly with
// no mount and no root. Video is decoded in software (on the CM0, 720p H.264 is
// ~66 fps on the CPU -- faster than the V4L2 M2M path once its frames are
// copied back) and scaled straight to the output size; when the decoder falls
// behind it first drops late frames, then turns off the deblocking filter, then
// skips non-reference frames. Audio is resampled to 48 kHz stereo S16 and is
// the master clock; video follows it.
//
// Threads: one demuxer, one video decoder, one audio decoder+output. Nothing
// here touches LVGL: the UI polls take_frame() / snapshot() from a timer.
namespace platform {

enum class MediaKind { None, Video, Audio, Image };

// By extension; None for anything the Files tool should not try to play.
MediaKind media_kind_for(const std::string& name);

// The pixel layout take_frame() fills: LVGL's native 32-bit XRGB8888 (BGRX in
// memory) or RGB565.
enum class FrameFormat { XRGB8888, RGB565 };

struct MediaTrack {
    int stream = -1;
    std::string label;  // "English (AAC 5.1)"
};

struct MediaStatus {
    enum class State { Idle, Opening, Playing, Paused, Ended, Error } state = State::Idle;
    std::string error;
    double position = 0;  // seconds
    double duration = 0;  // 0 when unknown (a live stream)
    bool seeking = false;
    int volume = 100;     // percent, 0..150

    // Filled once open succeeds.
    bool has_video = false;  // a real video track, not cover art
    bool has_audio = false;
    bool has_cover = false;  // audio file with embedded artwork
    int video_width = 0, video_height = 0;
    double fps = 0;
    std::string container, video_codec, audio_codec;
    std::string title, artist, album;
    std::vector<MediaTrack> audio_tracks;
    int audio_track = -1;  // index into audio_tracks
    std::vector<MediaTrack> subtitle_tracks;  // includes an external .srt
    int subtitle_track = -1;  // -1 = off
    int dropped_frames = 0;
    int decoder_shortcuts = 0;  // 0 none, 1 no deblocking, 2 + skip non-ref
    std::string subtitle;  // the line on screen now, empty for none
};

class MediaPlayer {
public:
    MediaPlayer();
    ~MediaPlayer();
    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    // Starts playing `location` (a local path or smb:// URL) scaled to fit
    // max_width x max_height. Returns at once; status().state goes Opening ->
    // Playing or Error. `subtitle_location`, when set, is a sidecar .srt/.ass/
    // .vtt loaded as an extra subtitle track (and selected).
    void open(const std::string& location, int max_width, int max_height, FrameFormat format,
              const std::string& subtitle_location = "", double start_at = 0,
              const std::string& audio_location = "");
    void stop();

    void toggle_pause();
    void set_paused(bool paused);
    void seek_by(double seconds);
    void seek_to(double seconds);
    void set_volume(int percent);
    void cycle_audio_track();
    void cycle_subtitle();

    MediaStatus status() const;

    // Copies the newest frame into `pixels` when its sequence number differs
    // from `seq` (and updates seq). Returns false when nothing new.
    bool take_frame(std::uint32_t& seq, std::vector<std::uint8_t>& pixels, int& width, int& height);

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace platform
