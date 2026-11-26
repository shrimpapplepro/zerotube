/*
 * SPDX-FileCopyrightText: 2026 ZeroTube contributors
 * SPDX-FileCopyrightText: 2026 ZeroTube contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "media_player.h"


#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#if ZT_FLAG_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

#if ZT_FLAG_AUDIO_PULSE
#include <pulse/error.h>
#include <pulse/simple.h>
#elif ZT_FLAG_AUDIO_SDL
#include <SDL.h>
#endif

namespace platform {

namespace {

std::string lower_ext(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = name.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

bool in(const std::string& ext, std::initializer_list<const char*> list) {
    return std::any_of(list.begin(), list.end(), [&](const char* e) { return ext == e; });
}

}  // namespace

MediaKind media_kind_for(const std::string& name) {
    const std::string ext = lower_ext(name);
    if (ext.empty()) return MediaKind::None;
    if (in(ext, {"mp4", "m4v", "mkv", "webm", "avi", "mov", "qt", "wmv", "asf", "flv", "f4v", "ts", "m2ts", "mts",
                 "mpg", "mpeg", "m1v", "m2v", "vob", "3gp", "3g2", "ogv", "divx", "xvid", "rm", "rmvb", "mxf",
                 "nut", "y4m", "dv", "h264", "264", "h265", "265", "hevc", "ivf", "mjpeg", "mjpg", "gifv"})) {
        return MediaKind::Video;
    }
    if (in(ext, {"mp3", "m4a", "m4b", "aac", "flac", "ogg", "oga", "opus", "wav", "wave", "aif", "aiff", "aifc",
                 "wma", "ape", "wv", "mka", "alac", "ac3", "eac3", "dts", "mp2", "mpa", "amr", "awb", "au", "snd",
                 "caf", "tta", "tak", "dsf", "dff", "mid", "mod", "xm", "s3m", "it", "spx", "mpc", "ra", "w64",
                 "voc", "gsm", "adts", "weba"})) {
        return MediaKind::Audio;
    }
    if (in(ext, {"png", "jpg", "jpeg", "jpe", "jfif", "webp", "bmp", "gif", "tif", "tiff", "tga", "ppm", "pgm",
                 "pbm", "pnm", "pcx", "sgi", "dds", "exr", "hdr", "qoi", "jxl", "avif", "heic", "heif", "ico",
                 "psd", "xbm", "xpm", "j2k", "jp2"})) {
        return MediaKind::Image;
    }
    return MediaKind::None;
}

#if !ZT_FLAG_HAVE_FFMPEG

// Builds without FFmpeg (a bare simulator) keep the API and report why.
struct MediaPlayer::Impl {
    MediaStatus status;
};

MediaPlayer::MediaPlayer() : impl_(std::make_shared<Impl>()) {}
MediaPlayer::~MediaPlayer() = default;
void MediaPlayer::open(const std::string&, int, int, FrameFormat, const std::string&, double, const std::string&) {
    impl_->status = MediaStatus{};
    impl_->status.state = MediaStatus::State::Error;
    impl_->status.error = "Playback needs FFmpeg, which this build does not have";
}
void MediaPlayer::stop() { impl_->status = MediaStatus{}; }
void MediaPlayer::toggle_pause() {}
void MediaPlayer::set_paused(bool) {}
void MediaPlayer::seek_by(double) {}
void MediaPlayer::seek_to(double) {}
void MediaPlayer::set_volume(int) {}
void MediaPlayer::cycle_audio_track() {}
void MediaPlayer::cycle_subtitle() {}
MediaStatus MediaPlayer::status() const { return impl_->status; }
bool MediaPlayer::take_frame(std::uint32_t&, std::vector<std::uint8_t>&, int&, int&) { return false; }

#else

namespace {

using Clock = std::chrono::steady_clock;

double now_s() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

constexpr int kOutRate = 48000;
constexpr int kOutChannels = 2;
constexpr std::size_t kMaxQueueBytes = 12u * 1024u * 1024u;
constexpr double kQueueAheadS = 3.0;

// ---- audio output -----------------------------------------------------------

class AudioOut {
public:
    virtual ~AudioOut() = default;
    virtual bool open(std::string& error) = 0;
    // Blocks until the device takes it (that is what paces the audio thread).
    virtual bool write(const std::int16_t* samples, int frames) = 0;
    virtual double latency() = 0;  // seconds queued but not yet heard
    virtual void flush() = 0;
    // The rate the next write() takes. Asked before each chunk is resampled.
    virtual int rate() { return kOutRate; }
};

#if ZT_FLAG_AUDIO_PULSE
class PulseOut : public AudioOut {
public:
    ~PulseOut() override {
        if (pa_) pa_simple_free(pa_);
    }
    bool open(std::string& error) override {
        pa_sample_spec spec{};
        spec.format = PA_SAMPLE_S16LE;
        spec.rate = kOutRate;
        spec.channels = kOutChannels;
        // ~150 ms of server buffer: low enough that pause/seek feel immediate,
        // high enough that a busy decoder does not underrun.
        pa_buffer_attr attr{};
        attr.maxlength = static_cast<uint32_t>(-1);
        attr.tlength = static_cast<uint32_t>(pa_usec_to_bytes(150000, &spec));
        attr.prebuf = static_cast<uint32_t>(-1);
        attr.minreq = static_cast<uint32_t>(-1);
        attr.fragsize = static_cast<uint32_t>(-1);
        int err = 0;
        pa_ = pa_simple_new(nullptr, "ZeroTube", PA_STREAM_PLAYBACK, nullptr, "Video", &spec, nullptr, &attr,
                            &err);
        if (!pa_) {
            error = std::string("Audio: ") + pa_strerror(err);
            return false;
        }
        return true;
    }
    bool write(const std::int16_t* samples, int frames) override {
        int err = 0;
        return pa_simple_write(pa_, samples, static_cast<size_t>(frames) * kOutChannels * 2, &err) == 0;
    }
    double latency() override {
        int err = 0;
        const pa_usec_t usec = pa_simple_get_latency(pa_, &err);
        return usec == static_cast<pa_usec_t>(-1) ? 0.0 : static_cast<double>(usec) / 1e6;
    }
    void flush() override {
        int err = 0;
        pa_simple_flush(pa_, &err);
    }

private:
    pa_simple* pa_ = nullptr;
};
#elif ZT_FLAG_AUDIO_SDL
class SdlOut : public AudioOut {
public:
    ~SdlOut() override {
        if (dev_) SDL_CloseAudioDevice(dev_);
    }
    bool open(std::string& error) override {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            error = std::string("Audio: ") + SDL_GetError();
            return false;
        }
        SDL_AudioSpec want{};
        SDL_AudioSpec have{};
        want.freq = kOutRate;
        want.format = AUDIO_S16SYS;
        want.channels = kOutChannels;
        want.samples = 1024;
        dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!dev_) {
            error = std::string("Audio: ") + SDL_GetError();
            return false;
        }
        SDL_PauseAudioDevice(dev_, 0);
        return true;
    }
    bool write(const std::int16_t* samples, int frames) override {
        const Uint32 bytes_per_s = kOutRate * kOutChannels * 2;
        while (SDL_GetQueuedAudioSize(dev_) > bytes_per_s / 6) sleep_ms(5);
        return SDL_QueueAudio(dev_, samples, static_cast<Uint32>(frames) * kOutChannels * 2) == 0;
    }
    double latency() override {
        return static_cast<double>(SDL_GetQueuedAudioSize(dev_)) / (kOutRate * kOutChannels * 2);
    }
    void flush() override { SDL_ClearQueuedAudio(dev_); }

private:
    SDL_AudioDeviceID dev_ = 0;
};
#endif

// Paces like a sound card would, so playback still runs (silently) with no
// audio backend.
class NullOut : public AudioOut {
public:
    bool open(std::string&) override { return true; }
    bool write(const std::int16_t*, int frames) override {
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>(frames) * 1000000 / kOutRate));
        return true;
    }
    double latency() override { return 0; }
    void flush() override {}
};

std::unique_ptr<AudioOut> make_audio_out() {
#if ZT_FLAG_AUDIO_PULSE
    return std::make_unique<PulseOut>();
#elif ZT_FLAG_AUDIO_SDL
    return std::make_unique<SdlOut>();
#else
    return std::make_unique<NullOut>();
#endif
}

// ---- packet queue -----------------------------------------------------------

struct QueuedPacket {
    AVPacket* pkt = nullptr;
    int serial = 0;
};

class PacketQueue {
public:
    ~PacketQueue() { flush(); }

    void put(AVPacket* pkt, int serial) {
        std::lock_guard<std::mutex> lock(mutex_);
        bytes_ += static_cast<std::size_t>(pkt->size);
        queue_.push_back({pkt, serial});
        cv_.notify_one();
    }
    // 1 = got one, 0 = timed out (empty), -1 = aborted.
    int get(QueuedPacket& out, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return abort_ || !queue_.empty(); });
        if (abort_) return -1;
        if (queue_.empty()) return 0;
        out = queue_.front();
        queue_.pop_front();
        bytes_ -= static_cast<std::size_t>(out.pkt->size);
        return 1;
    }
    void flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : queue_) av_packet_free(&p.pkt);
        queue_.clear();
        bytes_ = 0;
        eof_ = false;
    }
    void abort() {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_ = true;
        cv_.notify_all();
    }
    void set_eof(bool eof) {
        std::lock_guard<std::mutex> lock(mutex_);
        eof_ = eof;
        cv_.notify_all();
    }
    bool eof_and_empty() {
        std::lock_guard<std::mutex> lock(mutex_);
        return eof_ && queue_.empty();
    }
    std::size_t bytes() {
        std::lock_guard<std::mutex> lock(mutex_);
        return bytes_;
    }
    std::size_t count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<QueuedPacket> queue_;
    std::size_t bytes_ = 0;
    bool eof_ = false;
    bool abort_ = false;
};

// ---- subtitles ----------------------------------------------------------------

struct Cue {
    double start = 0;
    double end = 0;
    std::string text;
};

// "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text" (FFmpeg's
// decoded ASS form) -> just the text, with {\tags} gone and \N as newlines.
std::string ass_to_text(const char* ass) {
    if (!ass) return "";
    const char* p = ass;
    if (std::strncmp(p, "Dialogue:", 9) == 0) {
        for (int commas = 0; *p && commas < 9; ++p) commas += *p == ',';
    } else {
        for (int commas = 0; *p && commas < 8; ++p) commas += *p == ',';
    }
    std::string out;
    bool in_tag = false;
    for (; *p; ++p) {
        if (*p == '{') { in_tag = true; continue; }
        if (*p == '}') { in_tag = false; continue; }
        if (in_tag) continue;
        if (*p == '\\' && (p[1] == 'N' || p[1] == 'n')) { out += '\n'; ++p; continue; }
        if (*p == '\\' && p[1] == 'h') { out += ' '; ++p; continue; }
        if (*p == '\r') continue;
        out += *p;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

std::string subtitle_text(const AVSubtitle& sub) {
    std::string text;
    for (unsigned i = 0; i < sub.num_rects; ++i) {
        const AVSubtitleRect* rect = sub.rects[i];
        std::string line;
        if (rect->type == SUBTITLE_ASS) line = ass_to_text(rect->ass);
        else if (rect->type == SUBTITLE_TEXT && rect->text) line = rect->text;
        if (line.empty()) continue;
        if (!text.empty()) text += '\n';
        text += line;
    }
    return text;
}

bool is_text_subtitle(const AVCodecParameters* par) {
    const AVCodecDescriptor* desc = avcodec_descriptor_get(par->codec_id);
    return desc && (desc->props & AV_CODEC_PROP_TEXT_SUB);
}

std::string dict(const AVDictionary* d, const char* key) {
    const AVDictionaryEntry* e = av_dict_get(d, key, nullptr, 0);
    return e && e->value ? e->value : "";
}

std::string track_label(const AVStream* st, int ordinal) {
    std::string label;
    const std::string title = dict(st->metadata, "title");
    const std::string lang = dict(st->metadata, "language");
    if (!title.empty()) label = title;
    else if (!lang.empty() && lang != "und") label = lang;
    else label = "Track " + std::to_string(ordinal);
    std::string detail = avcodec_get_name(st->codecpar->codec_id);
    for (auto& c : detail) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        const int ch = st->codecpar->ch_layout.nb_channels;
        if (ch == 1) detail += " mono";
        else if (ch == 6) detail += " 5.1";
        else if (ch == 8) detail += " 7.1";
        else if (ch > 2) detail += " " + std::to_string(ch) + "ch";
    }
    return label + " (" + detail + ")";
}

// One opened input: a local path or a URL (the helper's loopback proxy).
struct Input {
    AVFormatContext* ic = nullptr;

    ~Input() {
        if (ic) avformat_close_input(&ic);
    }

    bool open(const std::string& location, const AVIOInterruptCB& interrupt, std::string& error) {
        ic = avformat_alloc_context();
        ic->interrupt_callback = interrupt;
        AVDictionary* opts = nullptr;
        if (location.rfind("http", 0) == 0) {
            // The proxy is on loopback; a stall means the helper died, so give
            // up instead of hanging the player.
            av_dict_set(&opts, "rw_timeout", "30000000", 0);
            av_dict_set(&opts, "reconnect", "1", 0);
            av_dict_set(&opts, "seekable", "1", 0);
        }
        const int r = avformat_open_input(&ic, location.c_str(), nullptr, &opts);
        av_dict_free(&opts);
        if (r < 0) {
            char text[128];
            av_strerror(r, text, sizeof(text));
            error = std::string("Cannot open: ") + text;
            ic = nullptr;  // avformat_open_input freed it
            return false;
        }
        return true;
    }
};

// Audio packets from the separate audio input carry stream_index + this, so
// one queue and one decoder lookup serve both layouts.
constexpr int kAudioInputBase = 1000;

}  // namespace

// ---- the player -----------------------------------------------------------------

struct MediaPlayer::Impl : std::enable_shared_from_this<MediaPlayer::Impl> {
    // config
    std::string location;
    std::string audio_location;  // YouTube: a second input with the sound
    std::string sub_location;
    int max_w = 320, max_h = 170;
    FrameFormat format = FrameFormat::XRGB8888;
    double start_at = 0;

    std::atomic<bool> abort{false};

    // status
    mutable std::mutex status_mutex;
    MediaStatus st;

    // control
    std::atomic<bool> paused{false};
    std::mutex control_mutex;
    bool seek_requested = false;
    double seek_target = 0;
    int audio_switch_to = -1;  // stream index, -1 none pending
    int subtitle_switch_to = INT_MIN;  // subtitle_tracks index (-1 off), INT_MIN none pending
    std::atomic<int> serial{0};
    std::atomic<double> video_skip_until{-1};
    std::atomic<double> audio_skip_until{-1};
    std::atomic<int> volume{100};

    // clock (seconds of pts, not yet offset by the start time)
    std::mutex clock_mutex;
    double clock_pts = NAN;
    double clock_time = 0;
    double start_offset = 0;

    // FFmpeg
    Input input;
    Input ainput;  // open only with audio_location
    std::atomic<bool> audio_eof{false};
    std::atomic<int> vstream{-1};
    std::atomic<int> astream{-1};
    std::atomic<int> sstream{-1};  // embedded text subtitle stream, -1 none
    AVCodecContext* vdec = nullptr;
    AVCodecContext* sdec = nullptr;
    PacketQueue vq, aq;
    std::atomic<bool> demux_eof{false};
    std::atomic<bool> video_done{true};
    std::atomic<bool> audio_done{true};
    std::atomic<bool> audio_started{false};
    std::atomic<bool> audio_failed{false};

    // frames
    std::mutex frame_mutex;
    std::vector<std::uint8_t> frame;
    int frame_w = 0, frame_h = 0;
    std::uint32_t frame_seq = 0;

    // subtitles
    std::mutex sub_mutex;
    std::vector<Cue> cues;           // embedded, decoded as demuxed
    std::vector<Cue> external_cues;  // the sidecar file, all of it
    std::vector<int> sub_streams;    // per subtitle_tracks entry; -2 = external
    int sub_choice = -1;
    std::vector<int> audio_streams;

    ~Impl() {
        if (vdec) avcodec_free_context(&vdec);
        if (sdec) avcodec_free_context(&sdec);
    }

    // ---- clock ----
    double clock_get() {
        std::lock_guard<std::mutex> lock(clock_mutex);
        if (std::isnan(clock_pts)) return NAN;
        if (paused) return clock_pts;
        return clock_pts + (now_s() - clock_time);
    }
    void clock_set(double pts, bool force = false) {
        std::lock_guard<std::mutex> lock(clock_mutex);
        if (paused && !force) return;
        clock_pts = pts;
        clock_time = now_s();
    }
    void set_paused(bool pause) {
        std::lock_guard<std::mutex> lock(clock_mutex);
        if (pause == paused) return;
        if (pause) {
            if (!std::isnan(clock_pts)) clock_pts = clock_pts + (now_s() - clock_time);
            paused = true;
        } else {
            clock_time = now_s();
            paused = false;
        }
    }

    void fail(const std::string& message) {
        std::lock_guard<std::mutex> lock(status_mutex);
        st.state = MediaStatus::State::Error;
        st.error = message;
    }

    static int interrupt_cb(void* opaque) {
        return static_cast<Impl*>(opaque)->abort.load() ? 1 : 0;
    }

    // ---- frame output ----
    void fit(int src_w, int src_h, AVRational sar, int& out_w, int& out_h) const {
        double aspect = static_cast<double>(src_w) / std::max(1, src_h);
        if (sar.num > 0 && sar.den > 0) aspect *= av_q2d(sar);
        out_w = max_w;
        out_h = static_cast<int>(std::lround(max_w / aspect));
        if (out_h > max_h) {
            out_h = max_h;
            out_w = static_cast<int>(std::lround(max_h * aspect));
        }
        // Never upscale a picture smaller than the screen past 2x; pixel art
        // stays crisp-ish and tiny images do not turn to mush.
        if (src_w * 2 < out_w && src_h * 2 < out_h) {
            out_w = src_w * 2;
            out_h = src_h * 2;
        }
        out_w = std::max(2, out_w & ~1);
        out_h = std::max(2, out_h & ~1);
    }

    void publish(AVFrame* f, SwsContext*& sws) {
        int out_w = 0, out_h = 0;
        fit(f->width, f->height, f->sample_aspect_ratio, out_w, out_h);
        const AVPixelFormat dst_fmt = format == FrameFormat::RGB565 ? AV_PIX_FMT_RGB565LE : AV_PIX_FMT_BGR0;
        const int bpp = format == FrameFormat::RGB565 ? 2 : 4;
        // Bilinear when shrinking video (cheap, and it is moving); bicubic for
        // a still picture, which is looked at.
        const int flags = st_is_image() ? SWS_BICUBIC : SWS_FAST_BILINEAR;
        sws = sws_getCachedContext(sws, f->width, f->height, static_cast<AVPixelFormat>(f->format), out_w, out_h,
                                   dst_fmt, flags, nullptr, nullptr, nullptr);
        if (!sws) return;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(out_w) * out_h * bpp);
        std::uint8_t* dst[4] = {pixels.data(), nullptr, nullptr, nullptr};
        int dst_stride[4] = {out_w * bpp, 0, 0, 0};
        sws_scale(sws, f->data, f->linesize, 0, f->height, dst, dst_stride);
        std::lock_guard<std::mutex> lock(frame_mutex);
        frame.swap(pixels);
        frame_w = out_w;
        frame_h = out_h;
        ++frame_seq;
    }

    bool st_is_image() const { return is_image_; }
    bool is_image_ = false;

    // ---- demuxer thread ----
    void run_demux() {
        AVIOInterruptCB interrupt{interrupt_cb, this};
        std::string error;
        if (!input.open(location, interrupt, error)) {
            fail(error);
            return;
        }
        AVFormatContext* ic = input.ic;
        if (avformat_find_stream_info(ic, nullptr) < 0) {
            fail("Not a media file FFmpeg can read");
            return;
        }
        if (abort) return;
        AVFormatContext* aic = nullptr;
        if (!audio_location.empty()) {
            if (!ainput.open(audio_location, interrupt, error) || avformat_find_stream_info(ainput.ic, nullptr) < 0) {
                fail(error.empty() ? "Cannot read the audio stream" : error);
                return;
            }
            aic = ainput.ic;
            if (abort) return;
        }

        vstream = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        int best_audio = aic ? -1 : av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, vstream, nullptr, 0);
        if (aic) {
            const int a = av_find_best_stream(aic, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if (a >= 0) best_audio = kAudioInputBase + a;
        }
        bool cover = vstream >= 0 && (ic->streams[vstream]->disposition & AV_DISPOSITION_ATTACHED_PIC);
        start_offset = ic->start_time != AV_NOPTS_VALUE ? static_cast<double>(ic->start_time) / AV_TIME_BASE : 0;

        MediaStatus info;
        info.state = MediaStatus::State::Playing;
        info.duration = ic->duration > 0 ? static_cast<double>(ic->duration) / AV_TIME_BASE : 0;
        if (aic && aic->duration > 0) info.duration = std::max(info.duration, static_cast<double>(aic->duration) / AV_TIME_BASE);
        info.container = ic->iformat && ic->iformat->name ? ic->iformat->name : "";
        info.title = dict(ic->metadata, "title");
        info.artist = dict(ic->metadata, "artist");
        info.album = dict(ic->metadata, "album");
        if (best_audio >= 0) {
            const AVDictionary* md = stream_at(best_audio)->metadata;  // Ogg/Opus keep tags here
            if (info.title.empty()) info.title = dict(md, "title");
            if (info.artist.empty()) info.artist = dict(md, "artist");
            if (info.album.empty()) info.album = dict(md, "album");
        }
        if (info.artist.empty()) info.artist = dict(ic->metadata, "album_artist");

        for (unsigned i = 0; i < ic->nb_streams; ++i) {
            const AVStream* s = ic->streams[i];
            if (s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audio_streams.push_back(static_cast<int>(i));
                info.audio_tracks.push_back({static_cast<int>(i),
                                             track_label(s, static_cast<int>(info.audio_tracks.size()) + 1)});
                if (static_cast<int>(i) == best_audio) info.audio_track = static_cast<int>(info.audio_tracks.size()) - 1;
            } else if (s->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE && is_text_subtitle(s->codecpar)) {
                sub_streams.push_back(static_cast<int>(i));
                info.subtitle_tracks.push_back({static_cast<int>(i),
                                                track_label(s, static_cast<int>(info.subtitle_tracks.size()) + 1)});
            }
        }

        // A one-frame "video" with no duration is a picture.
        if (vstream >= 0 && !cover && best_audio < 0) {
            const AVStream* s = ic->streams[vstream];
            const bool still = s->nb_frames == 1 || (info.duration <= 0.05 && s->nb_frames <= 1) ||
                               (ic->iformat && std::strstr(ic->iformat->name, "_pipe")) ||
                               (ic->iformat && std::strcmp(ic->iformat->name, "image2") == 0);
            const bool animated = s->codecpar->codec_id == AV_CODEC_ID_GIF || s->codecpar->codec_id == AV_CODEC_ID_WEBP;
            is_image_ = still && !(animated && info.duration > 0.05);
        }

        // Video decoder (also used once for cover art).
        if (vstream >= 0) {
            const AVStream* s = ic->streams[vstream];
            const AVCodec* codec = avcodec_find_decoder(s->codecpar->codec_id);
            vdec = codec ? avcodec_alloc_context3(codec) : nullptr;
            if (vdec && avcodec_parameters_to_context(vdec, s->codecpar) >= 0) {
                vdec->pkt_timebase = s->time_base;
                // Leave a core for audio + the UI.
                vdec->thread_count = cover || is_image_ ? 1 : 3;
                vdec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
                // Decode MJPEG/MPEG-4 etc. at a reduced size when the codec
                // can: a 1080p camera clip shown at 320 wide needs 1/4 of it.
                if (codec->max_lowres > 0) {
                    int lowres = 0;
                    while (lowres < codec->max_lowres && (s->codecpar->width >> (lowres + 1)) >= max_w &&
                           (s->codecpar->height >> (lowres + 1)) >= max_h) {
                        ++lowres;
                    }
                    vdec->lowres = lowres;
                }
                if (avcodec_open2(vdec, codec, nullptr) < 0) avcodec_free_context(&vdec);
            } else if (vdec) {
                avcodec_free_context(&vdec);
            }
            if (!vdec) {
                if (cover || best_audio >= 0) {
                    vstream = -1;  // play the sound anyway
                    cover = false;
                } else {
                    fail(std::string("No decoder for ") + avcodec_get_name(s->codecpar->codec_id));
                    return;
                }
            } else {
                info.video_width = s->codecpar->width;
                info.video_height = s->codecpar->height;
                info.video_codec = avcodec_get_name(s->codecpar->codec_id);
                const AVRational fr = av_guess_frame_rate(ic, const_cast<AVStream*>(s), nullptr);
                info.fps = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 0;
            }
        }
        if (best_audio >= 0) info.audio_codec = avcodec_get_name(stream_at(best_audio)->codecpar->codec_id);
        if (aic) {
            audio_streams.push_back(best_audio);
            info.audio_tracks.push_back({best_audio, track_label(stream_at(best_audio), 1)});
            info.audio_track = 0;
        }

        if (cover && vdec) {
            // Artwork: decode the attached picture once and show it.
            AVPacket* pic = av_packet_clone(&ic->streams[vstream]->attached_pic);
            AVFrame* f = av_frame_alloc();
            SwsContext* sws = nullptr;
            if (pic && avcodec_send_packet(vdec, pic) >= 0) {
                avcodec_send_packet(vdec, nullptr);
                if (avcodec_receive_frame(vdec, f) >= 0) {
                    info.has_cover = true;
                    publish(f, sws);
                }
            }
            sws_freeContext(sws);
            av_frame_free(&f);
            av_packet_free(&pic);
            avcodec_free_context(&vdec);
            vstream = -1;
        }
        info.has_video = vstream >= 0;
        info.has_audio = best_audio >= 0;
        if (!info.has_video && !info.has_audio && !info.has_cover) {
            fail("Nothing playable in this file");
            return;
        }

        // Sidecar subtitles, read whole.
        if (!sub_location.empty()) {
            load_external_subs(interrupt);
            if (!external_cues.empty()) {
                sub_streams.push_back(-2);
                info.subtitle_tracks.push_back({-2, "Subtitles"});
                info.subtitle_track = static_cast<int>(info.subtitle_tracks.size()) - 1;
                sub_choice = info.subtitle_track;
            }
        }

        // Like other players: an embedded track flagged default or forced is
        // on from the start (a sidecar file, chosen above, wins).
        if (info.subtitle_track < 0) {
            for (std::size_t i = 0; i < sub_streams.size(); ++i) {
                const int disp = ic->streams[sub_streams[i]]->disposition;
                if (disp & (AV_DISPOSITION_DEFAULT | AV_DISPOSITION_FORCED)) {
                    select_subtitle(static_cast<int>(i));
                    info.subtitle_track = static_cast<int>(i);
                    break;
                }
            }
        }

        info.volume = volume;
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            st = info;
        }
        astream = best_audio;

        if (start_at > 0.5 && info.duration > start_at + 1) {
            std::lock_guard<std::mutex> lock(control_mutex);
            seek_requested = true;
            seek_target = start_at;
        }

        auto self = shared_from_this();
        if (vstream >= 0) {
            video_done = false;
            std::thread([self] { self->run_video(); }).detach();
        }
        if (best_audio >= 0) {
            audio_done = false;
            std::thread([self] { self->run_audio(); }).detach();
        }

        AVPacket* pkt = av_packet_alloc();
        int read_errors = 0;
        while (!abort) {
            handle_requests(ic);

            const bool video_full = vstream < 0 || vq.count() > static_cast<std::size_t>(std::max(30.0, st_fps() * kQueueAheadS));
            const bool audio_full = astream < 0 || aq.count() > 150 || (aic && audio_eof);
            const bool full = vq.bytes() + aq.bytes() > kMaxQueueBytes || (video_full && audio_full) ||
                              (aic && video_full && aq.count() > 150);
            if (full) {
                sleep_ms(10);
                continue;
            }
            if (aic && !audio_eof && aq.count() <= 150) {
                // The audio input keeps its own pace; read it whenever the
                // audio queue has room, ahead of the video.
                const int ar = av_read_frame(aic, pkt);
                if (ar < 0) {
                    if (ar == AVERROR_EOF || avio_feof(aic->pb)) {
                        audio_eof = true;
                        if (demux_eof) aq.set_eof(true);
                    } else {
                        sleep_ms(20);
                    }
                } else {
                    if (kAudioInputBase + pkt->stream_index == astream) {
                        pkt->stream_index += kAudioInputBase;
                        aq.put(av_packet_clone(pkt), serial);
                    }
                    av_packet_unref(pkt);
                    // Video still has room too: fall through only when the
                    // audio queue is comfortable, so neither starves.
                    if (aq.count() < 40) continue;
                }
            }
            if (aic && video_full) {
                sleep_ms(audio_full ? 10 : 2);
                continue;
            }
            if (demux_eof) {
                if (aic && audio_eof) aq.set_eof(true);
                sleep_ms(20);
                continue;
            }
            const int r = av_read_frame(ic, pkt);
            if (r < 0) {
                if (r == AVERROR_EOF || avio_feof(ic->pb) || ++read_errors > 20) {
                    demux_eof = true;
                    vq.set_eof(true);
                    if (!aic || audio_eof) aq.set_eof(true);
                } else {
                    sleep_ms(20);
                }
                continue;
            }
            read_errors = 0;
            const int index = pkt->stream_index;
            const AVStream* s = ic->streams[index];
            if (index == vstream) {
                vq.put(av_packet_clone(pkt), serial);
            } else if (index == astream) {
                aq.put(av_packet_clone(pkt), serial);
            } else if (index == sstream && sdec) {
                decode_subtitle(pkt, s);
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        vq.abort();
        aq.abort();
    }

    const AVStream* stream_at(int index) const {
        if (index >= kAudioInputBase) return ainput.ic->streams[index - kAudioInputBase];
        return input.ic->streams[index];
    }

    double st_fps() {
        std::lock_guard<std::mutex> lock(status_mutex);
        return st.fps > 0 ? st.fps : 25;
    }

    void handle_requests(AVFormatContext* ic) {
        bool do_seek = false;
        double target = 0;
        int switch_to = -1;
        int sub_to = INT_MIN;
        {
            std::lock_guard<std::mutex> lock(control_mutex);
            sub_to = subtitle_switch_to;
            subtitle_switch_to = INT_MIN;
            if (sub_to != INT_MIN) {
                select_subtitle(sub_to);
                // Embedded cues already demuxed are gone: re-read from here.
                if (sstream >= 0 && !seek_requested) {
                    seek_requested = true;
                    seek_target = position_now();
                }
            }
            if (audio_switch_to >= 0) {
                switch_to = audio_switch_to;
                audio_switch_to = -1;
            }
            if (seek_requested || switch_to >= 0) {
                do_seek = true;
                target = seek_requested ? seek_target : position_now();
                seek_requested = false;
            }
        }
        if (switch_to >= 0) astream = switch_to;  // run_audio reopens its decoder on the new serial
        if (!do_seek) return;

        {
            std::lock_guard<std::mutex> lock(status_mutex);
            st.seeking = true;
            if (st.state == MediaStatus::State::Ended) st.state = paused ? MediaStatus::State::Paused : MediaStatus::State::Playing;
        }
        const double pts = target + start_offset;
        const auto ts = static_cast<std::int64_t>(pts * AV_TIME_BASE);
        // Land on the keyframe at or before the target, then decode forward to it.
        if (avformat_seek_file(ic, -1, INT64_MIN, ts, ts, 0) < 0) {
            avformat_seek_file(ic, -1, INT64_MIN, ts, INT64_MAX, AVSEEK_FLAG_ANY);
        }
        if (ainput.ic) {
            avformat_seek_file(ainput.ic, -1, INT64_MIN, ts, ts, 0);
            audio_eof = false;
        }
        ++serial;
        vq.flush();
        aq.flush();
        demux_eof = false;
        if (vstream >= 0) video_done = false;
        if (astream >= 0) audio_done = false;
        video_skip_until = pts;
        audio_skip_until = pts;
        {
            std::lock_guard<std::mutex> lock(sub_mutex);
            cues.clear();
        }
        if (sdec) avcodec_flush_buffers(sdec);
        clock_set(pts, true);
        if (astream >= 0 && !audio_failed) {
            // The audio thread re-anchors the clock on its first write.
            audio_started = false;
        }
    }

    double position_now() {
        const double c = clock_get();
        return std::isnan(c) ? 0 : std::max(0.0, c - start_offset);
    }

    void decode_subtitle(AVPacket* pkt, const AVStream* s) {
        AVSubtitle sub{};
        int got = 0;
        if (avcodec_decode_subtitle2(sdec, &sub, &got, pkt) < 0 || !got) return;
        const double base = pkt->pts != AV_NOPTS_VALUE ? pkt->pts * av_q2d(s->time_base) : 0;
        Cue cue;
        cue.start = base + sub.start_display_time / 1000.0;
        cue.end = sub.end_display_time > sub.start_display_time
                      ? base + sub.end_display_time / 1000.0
                      : base + (pkt->duration > 0 ? pkt->duration * av_q2d(s->time_base) : 4.0);
        cue.text = subtitle_text(sub);
        avsubtitle_free(&sub);
        if (cue.text.empty()) return;
        std::lock_guard<std::mutex> lock(sub_mutex);
        const double now = clock_get();
        if (!std::isnan(now)) {
            cues.erase(std::remove_if(cues.begin(), cues.end(), [&](const Cue& c) { return c.end < now - 5; }),
                       cues.end());
        }
        cues.push_back(std::move(cue));
    }

    void load_external_subs(const AVIOInterruptCB& interrupt) {
        Input sub;
        std::string error;
        if (!sub.open(sub_location, interrupt, error)) return;
        if (avformat_find_stream_info(sub.ic, nullptr) < 0) return;
        const int index = av_find_best_stream(sub.ic, AVMEDIA_TYPE_SUBTITLE, -1, -1, nullptr, 0);
        if (index < 0) return;
        const AVStream* s = sub.ic->streams[index];
        const AVCodec* codec = avcodec_find_decoder(s->codecpar->codec_id);
        if (!codec) return;
        AVCodecContext* dec = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(dec, s->codecpar);
        dec->pkt_timebase = s->time_base;
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "sub_charenc", "UTF-8", 0);
        if (avcodec_open2(dec, codec, &opts) >= 0) {
            AVPacket* pkt = av_packet_alloc();
            while (!abort && av_read_frame(sub.ic, pkt) >= 0) {
                if (pkt->stream_index == index) {
                    AVSubtitle as{};
                    int got = 0;
                    if (avcodec_decode_subtitle2(dec, &as, &got, pkt) >= 0 && got) {
                        const double base = pkt->pts != AV_NOPTS_VALUE ? pkt->pts * av_q2d(s->time_base) : 0;
                        Cue cue;
                        cue.start = base + as.start_display_time / 1000.0;
                        cue.end = pkt->duration > 0 ? base + pkt->duration * av_q2d(s->time_base)
                                                    : base + as.end_display_time / 1000.0;
                        if (cue.end <= cue.start) cue.end = cue.start + 4;
                        cue.text = subtitle_text(as);
                        if (!cue.text.empty()) external_cues.push_back(std::move(cue));
                        avsubtitle_free(&as);
                    }
                }
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        }
        av_dict_free(&opts);
        avcodec_free_context(&dec);
        std::sort(external_cues.begin(), external_cues.end(),
                  [](const Cue& a, const Cue& b) { return a.start < b.start; });
    }

    void select_subtitle(int choice) {
        // choice indexes st.subtitle_tracks; -1 = off.
        if (sdec) avcodec_free_context(&sdec);
        sstream = -1;
        {
            std::lock_guard<std::mutex> lock(sub_mutex);
            cues.clear();
            sub_choice = choice;
        }
        if (choice < 0 || choice >= static_cast<int>(sub_streams.size())) return;
        const int stream = sub_streams[static_cast<std::size_t>(choice)];
        if (stream < 0) return;  // external: cues already loaded
        const AVStream* s = input.ic->streams[stream];
        const AVCodec* codec = avcodec_find_decoder(s->codecpar->codec_id);
        if (!codec) return;
        AVCodecContext* dec = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(dec, s->codecpar);
        dec->pkt_timebase = s->time_base;
        if (avcodec_open2(dec, codec, nullptr) < 0) {
            avcodec_free_context(&dec);
            return;
        }
        sdec = dec;
        sstream = stream;
    }

    std::string current_subtitle(double pts) {
        std::lock_guard<std::mutex> lock(sub_mutex);
        if (sub_choice < 0) return "";
        const bool external = sub_choice < static_cast<int>(sub_streams.size()) &&
                              sub_streams[static_cast<std::size_t>(sub_choice)] == -2;
        const auto& list = external ? external_cues : cues;
        std::string text;
        for (const auto& cue : list) {
            if (cue.start <= pts && pts < cue.end) {
                if (!text.empty()) text += '\n';
                text += cue.text;
            }
            if (external && cue.start > pts) break;
        }
        return text;
    }

    // ---- video thread ----
    void run_video() {
        auto self = shared_from_this();  // keep alive for the detached thread
        AVFrame* f = av_frame_alloc();
        SwsContext* sws = nullptr;
        int current = -1;
        bool drained = false;
        double last_shown_at = 0;
        int window = 0, window_drops = 0;
        const AVRational tb = input.ic->streams[vstream]->time_base;
        const double wait_start = now_s();

        auto handle_frame = [&](AVFrame* frame) {
            const std::int64_t ts = frame->best_effort_timestamp;
            const double pts = ts != AV_NOPTS_VALUE ? ts * av_q2d(tb) : (std::isnan(clock_get()) ? 0 : clock_get());
            const double skip = video_skip_until;
            if (skip >= 0) {
                if (pts < skip - 0.03) return;
                video_skip_until = -1;
            }
            if (is_image_) {
                publish(frame, sws);
                clock_set(pts, true);
                return;
            }
            // No audio (or audio could not start within a second): video runs the clock.
            if (std::isnan(clock_get())) {
                if (astream < 0 || audio_failed || now_s() - wait_start > 1.0) clock_set(pts, true);
            }
            for (;;) {
                if (abort || current != serial) return;
                double c = clock_get();
                if (std::isnan(c)) {
                    if (astream < 0 || audio_failed || now_s() - wait_start > 1.0) {
                        clock_set(pts, true);
                        c = pts;
                    } else {
                        sleep_ms(5);
                        continue;
                    }
                }
                const double ahead = pts - c;
                if (ahead <= 0.004) break;
                sleep_ms(static_cast<int>(std::clamp(ahead * 1000.0, 1.0, 20.0)));
            }
            const double late = clock_get() - pts;
            ++window;
            const bool showed_recently = now_s() - last_shown_at < 0.4;
            if (late > 0.1 && showed_recently) {
                ++window_drops;
                std::lock_guard<std::mutex> lock(status_mutex);
                ++st.dropped_frames;
            } else {
                publish(frame, sws);
                last_shown_at = now_s();
                std::lock_guard<std::mutex> lock(status_mutex);
                st.seeking = false;
            }
            if (window >= 48) {
                // Dropping more than ~1 frame in 6: take a decoder shortcut.
                if (window_drops > 8) {
                    std::lock_guard<std::mutex> lock(status_mutex);
                    if (st.decoder_shortcuts == 0) {
                        vdec->skip_loop_filter = AVDISCARD_ALL;
                        st.decoder_shortcuts = 1;
                    } else if (st.decoder_shortcuts == 1) {
                        vdec->skip_frame = AVDISCARD_NONREF;
                        st.decoder_shortcuts = 2;
                    }
                }
                window = 0;
                window_drops = 0;
            }
        };

        while (!abort) {
            QueuedPacket p;
            const int r = vq.get(p, 30);
            if (r < 0) break;
            if (r == 0) {
                if (!drained && vq.eof_and_empty() && current == serial) {
                    // End of file: flush out the decoder's last frames.
                    avcodec_send_packet(vdec, nullptr);
                    while (!abort && avcodec_receive_frame(vdec, f) == 0) {
                        handle_frame(f);
                        av_frame_unref(f);
                    }
                    drained = true;
                    video_done = true;
                }
                continue;
            }
            if (p.serial != serial) {
                av_packet_free(&p.pkt);
                continue;
            }
            if (p.serial != current || drained) {
                avcodec_flush_buffers(vdec);
                current = p.serial;
                drained = false;
                video_done = false;
                last_shown_at = 0;
            }
            avcodec_send_packet(vdec, p.pkt);
            av_packet_free(&p.pkt);
            while (!abort && avcodec_receive_frame(vdec, f) == 0) {
                handle_frame(f);
                av_frame_unref(f);
            }
        }
        sws_freeContext(sws);
        av_frame_free(&f);
    }

    // ---- audio thread ----
    void run_audio() {
        auto self = shared_from_this();
        auto local = make_audio_out();
        std::string error;
        if (!local->open(error)) {
            local = std::make_unique<NullOut>();  // keep the clock honest, silently
        }
        auto out = std::move(local);
        AVCodecContext* adec = nullptr;
        int adec_stream = -1;
        SwrContext* swr = nullptr;
        AVChannelLayout swr_in_layout{};
        int swr_in_rate = 0, swr_in_fmt = -1, swr_out_rate = 0;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        AVFrame* f = av_frame_alloc();
        std::vector<std::int16_t> pcm;
        int current = -1;
        bool drained = false;

        auto open_decoder = [&](int stream) -> bool {
            if (adec) avcodec_free_context(&adec);
            adec_stream = stream;
            const AVStream* s = stream_at(stream);
            const AVCodec* codec = avcodec_find_decoder(s->codecpar->codec_id);
            if (!codec) return false;
            adec = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(adec, s->codecpar);
            adec->pkt_timebase = s->time_base;
            if (avcodec_open2(adec, codec, nullptr) < 0) {
                avcodec_free_context(&adec);
                return false;
            }
            return true;
        };

        auto handle_frame = [&](AVFrame* frame) {
            const AVRational tb = stream_at(adec_stream)->time_base;
            const double pts = frame->pts != AV_NOPTS_VALUE ? frame->pts * av_q2d(tb) : NAN;
            const double dur = static_cast<double>(frame->nb_samples) / std::max(1, frame->sample_rate);
            const double skip = audio_skip_until;
            if (skip >= 0 && !std::isnan(pts)) {
                if (pts + dur < skip) return;
                audio_skip_until = -1;
            }
            const int out_rate = out->rate();  // changes when a speaker is picked
            if (!swr || swr_in_rate != frame->sample_rate || swr_in_fmt != frame->format ||
                swr_out_rate != out_rate || av_channel_layout_compare(&swr_in_layout, &frame->ch_layout) != 0) {
                swr_free(&swr);
                if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, out_rate, &frame->ch_layout,
                                        static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0,
                                        nullptr) < 0 ||
                    swr_init(swr) < 0) {
                    swr_free(&swr);
                    return;
                }
                av_channel_layout_uninit(&swr_in_layout);
                av_channel_layout_copy(&swr_in_layout, &frame->ch_layout);
                swr_in_rate = frame->sample_rate;
                swr_in_fmt = frame->format;
                swr_out_rate = out_rate;
            }
            const int capacity = swr_get_out_samples(swr, frame->nb_samples) + 32;
            pcm.resize(static_cast<std::size_t>(capacity) * kOutChannels);
            std::uint8_t* dst = reinterpret_cast<std::uint8_t*>(pcm.data());
            const int frames = swr_convert(swr, &dst, capacity, const_cast<const std::uint8_t**>(frame->extended_data),
                                           frame->nb_samples);
            if (frames <= 0) return;
            const int vol = volume;
            if (vol != 100) {
                for (int i = 0; i < frames * kOutChannels; ++i) {
                    const int v = pcm[static_cast<std::size_t>(i)] * vol / 100;
                    pcm[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(std::clamp(v, -32768, 32767));
                }
            }
            // Paused: hold this chunk until resumed (or dropped by a seek).
            while (paused && !abort && current == serial) sleep_ms(10);
            if (abort || current != serial) return;
            out->write(pcm.data(), frames);
            if (!std::isnan(pts) && current == serial) {
                clock_set(pts + dur - out->latency());
                if (!audio_started) {
                    audio_started = true;
                    std::lock_guard<std::mutex> lock(status_mutex);
                    if (vstream < 0) st.seeking = false;
                }
            }
        };

        while (!abort) {
            QueuedPacket p;
            const int r = aq.get(p, 30);
            if (r < 0) break;
            if (r == 0) {
                if (!drained && adec && aq.eof_and_empty() && current == serial) {
                    avcodec_send_packet(adec, nullptr);
                    while (!abort && avcodec_receive_frame(adec, f) == 0) {
                        handle_frame(f);
                        av_frame_unref(f);
                    }
                    drained = true;
                    audio_done = true;
                }
                continue;
            }
            if (p.serial != serial) {
                av_packet_free(&p.pkt);
                continue;
            }
            if (p.pkt->stream_index != adec_stream) {
                if (!open_decoder(p.pkt->stream_index)) {
                    av_packet_free(&p.pkt);
                    if (current == -1) {
                        audio_failed = true;
                        audio_done = true;
                    }
                    continue;
                }
                current = -1;  // force the flush below
            }
            if (p.serial != current || drained) {
                avcodec_flush_buffers(adec);
                out->flush();
                current = p.serial;
                drained = false;
                audio_done = false;
            }
            avcodec_send_packet(adec, p.pkt);
            av_packet_free(&p.pkt);
            while (!abort && avcodec_receive_frame(adec, f) == 0) {
                handle_frame(f);
                av_frame_unref(f);
            }
        }
        av_frame_free(&f);
        swr_free(&swr);
        av_channel_layout_uninit(&swr_in_layout);
        if (adec) avcodec_free_context(&adec);
    }
};

MediaPlayer::MediaPlayer() = default;

MediaPlayer::~MediaPlayer() {
    stop();
}

void MediaPlayer::open(const std::string& location, int max_width, int max_height, FrameFormat format,
                       const std::string& subtitle_location, double start_at, const std::string& audio_location) {
    stop();
    av_log_set_level(AV_LOG_ERROR);
    auto impl = std::make_shared<Impl>();
    impl->location = location;
    impl->audio_location = audio_location;
    impl->sub_location = subtitle_location;
    impl->max_w = max_width;
    impl->max_h = max_height;
    impl->format = format;
    impl->start_at = start_at;
    impl->st.state = MediaStatus::State::Opening;
    impl_ = impl;
    // Detached: an SMB read can block for its whole timeout, and the UI must
    // not wait on that. The thread owns a reference, so whoever finishes last
    // frees the FFmpeg state.
    std::thread([impl] { impl->run_demux(); }).detach();
}

void MediaPlayer::stop() {
    if (!impl_) return;
    impl_->abort = true;
    impl_->vq.abort();
    impl_->aq.abort();
    impl_.reset();
}

void MediaPlayer::toggle_pause() {
    if (!impl_) return;
    set_paused(!impl_->paused);
}

void MediaPlayer::set_paused(bool paused) {
    if (!impl_) return;
    impl_->set_paused(paused);
}

void MediaPlayer::seek_by(double seconds) {
    if (!impl_) return;
    double base;
    {
        std::lock_guard<std::mutex> lock(impl_->control_mutex);
        base = impl_->seek_requested ? impl_->seek_target : impl_->position_now();
    }
    seek_to(base + seconds);
}

void MediaPlayer::seek_to(double seconds) {
    if (!impl_) return;
    double duration;
    {
        std::lock_guard<std::mutex> lock(impl_->status_mutex);
        duration = impl_->st.duration;
        if (impl_->st.state == MediaStatus::State::Opening || impl_->st.state == MediaStatus::State::Error) return;
    }
    if (duration > 0) seconds = std::min(seconds, std::max(0.0, duration - 0.5));
    seconds = std::max(0.0, seconds);
    std::lock_guard<std::mutex> lock(impl_->control_mutex);
    impl_->seek_requested = true;
    impl_->seek_target = seconds;
}

void MediaPlayer::set_volume(int percent) {
    if (!impl_) return;
    impl_->volume = std::clamp(percent, 0, 150);
}

void MediaPlayer::cycle_audio_track() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->status_mutex);
    auto& st = impl_->st;
    if (st.audio_tracks.size() < 2) return;
    st.audio_track = (st.audio_track + 1) % static_cast<int>(st.audio_tracks.size());
    std::lock_guard<std::mutex> control(impl_->control_mutex);
    impl_->audio_switch_to = st.audio_tracks[static_cast<std::size_t>(st.audio_track)].stream;
}

void MediaPlayer::cycle_subtitle() {
    if (!impl_) return;
    int choice;
    {
        std::lock_guard<std::mutex> lock(impl_->status_mutex);
        auto& st = impl_->st;
        if (st.subtitle_tracks.empty()) return;
        st.subtitle_track = st.subtitle_track + 1 >= static_cast<int>(st.subtitle_tracks.size()) ? -1 : st.subtitle_track + 1;
        choice = st.subtitle_track;
    }
    // The demuxer owns the subtitle decoder; it swaps it between reads.
    std::lock_guard<std::mutex> control(impl_->control_mutex);
    impl_->subtitle_switch_to = choice;
}

MediaStatus MediaPlayer::status() const {
    if (!impl_) return MediaStatus{};
    MediaStatus out;
    {
        std::lock_guard<std::mutex> lock(impl_->status_mutex);
        out = impl_->st;
    }
    out.volume = impl_->volume;
    if (out.state == MediaStatus::State::Playing || out.state == MediaStatus::State::Paused ||
        out.state == MediaStatus::State::Ended) {
        const double c = impl_->clock_get();
        out.position = std::isnan(c) ? 0 : std::max(0.0, c - impl_->start_offset);
        if (out.duration > 0) out.position = std::min(out.position, out.duration);
        const bool done = impl_->demux_eof && (impl_->vstream < 0 || impl_->video_done) &&
                          (impl_->astream < 0 || impl_->audio_done);
        if (done && !out.seeking) {
            out.state = MediaStatus::State::Ended;
            if (out.duration > 0 && !impl_->is_image_) out.position = out.duration;
        } else {
            out.state = impl_->paused ? MediaStatus::State::Paused : MediaStatus::State::Playing;
        }
        if (!std::isnan(c)) out.subtitle = impl_->current_subtitle(c);
    }
    return out;
}

bool MediaPlayer::take_frame(std::uint32_t& seq, std::vector<std::uint8_t>& pixels, int& width, int& height) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->frame_mutex);
    if (impl_->frame_seq == seq || impl_->frame.empty()) return false;
    pixels = impl_->frame;
    width = impl_->frame_w;
    height = impl_->frame_h;
    seq = impl_->frame_seq;
    return true;
}

#endif  // ZT_FLAG_HAVE_FFMPEG

}  // namespace platform
