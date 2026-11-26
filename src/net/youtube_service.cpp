/*
 * SPDX-FileCopyrightText: 2026 ZeroTube contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "youtube_service.h"

#include "linux_input.h"
#include "logger.h"
#include "thumb_decoder.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

#ifndef ZT_HELPER_SOURCE
#define ZT_HELPER_SOURCE "helper"
#endif
#ifndef ZT_HELPER_INSTALLED
#define ZT_HELPER_INSTALLED "/usr/lib/zerotube"
#endif

namespace net {

namespace fs = std::filesystem;

namespace {

constexpr int kThumbW = 64;
constexpr int kThumbH = 36;
constexpr int kAvatar = 32;
constexpr std::size_t kThumbCap = 240;

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v && *v ? v : fallback;
}

std::string data_dir() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/zerotube";
    return env_or("HOME", "/tmp") + "/.local/share/zerotube";
}

std::string helper_dir() {
    const std::string forced = env_or("ZEROTUBE_HELPER_DIR", "");
    if (!forced.empty()) return forced;
    std::error_code ec;
    if (fs::exists(std::string(ZT_HELPER_INSTALLED) + "/zerotube-helper.py", ec)) return ZT_HELPER_INSTALLED;
    return ZT_HELPER_SOURCE;
}

std::string python_path() {
    const std::string forced = env_or("ZEROTUBE_PYTHON", "");
    if (!forced.empty()) return forced;
    const std::string venv = data_dir() + "/venv/bin/python";
    std::error_code ec;
    if (fs::exists(venv, ec)) return venv;
    return "python3";  // the helper still starts and reports the engine missing
}

// Runs argv with stdin/stdout piped (either may be null to skip). Returns pid.
pid_t spawn(const std::vector<std::string>& args, int* to_child, int* from_child) {
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    if (to_child && pipe(in_pipe) != 0) return -1;
    if (from_child && pipe(out_pipe) != 0) return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (to_child) {
        posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
    }
    if (from_child) {
        posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    }
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (to_child) {
        close(in_pipe[0]);
        if (rc == 0) *to_child = in_pipe[1];
        else close(in_pipe[1]);
    }
    if (from_child) {
        close(out_pipe[1]);
        if (rc == 0) *from_child = out_pipe[0];
        else close(out_pipe[0]);
    }
    return rc == 0 ? pid : -1;
}

struct Thumb {
    std::vector<std::uint8_t> pixels;
    lv_image_dsc_t dsc{};
    std::uint64_t used = 0;
};

void fill_dsc(Thumb& t, int w, int h) {
    t.dsc = lv_image_dsc_t{};
    t.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    t.dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    t.dsc.header.w = static_cast<uint32_t>(w);
    t.dsc.header.h = static_cast<uint32_t>(h);
    t.dsc.header.stride = static_cast<uint32_t>(w * 4);
    t.dsc.data = t.pixels.data();
    t.dsc.data_size = static_cast<uint32_t>(t.pixels.size());
}

}  // namespace

std::vector<Video> parse_videos(const nlohmann::json& reply) {
    std::vector<Video> out;
    const auto it = reply.find("items");
    if (it == reply.end() || !it->is_array()) return out;
    for (const auto& j : *it) {
        Video v;
        v.id = j.value("id", "");
        v.title = j.value("title", "");
        v.channel = j.value("channel", "");
        v.duration = j.value("duration", 0);
        v.views = j.value("views", 0LL);
        v.live = j.value("live", false);
        if (!v.id.empty()) out.push_back(std::move(v));
    }
    return out;
}

struct YouTube::Impl {
    // helper process
    pid_t pid = -1;
    int to_helper = -1;
    std::atomic<bool> stopping{false};

    // from worker threads to the LVGL thread
    std::mutex inbox_mutex;
    std::deque<Json> inbox;

    // LVGL-thread state
    int next_id = 1;
    std::map<int, std::pair<Owner, Reply>> pending;
    std::vector<Json> unsent;  // before the helper is ready
    int next_listener = 1;
    std::map<int, std::pair<Owner, Reply>> listeners;
    std::map<std::string, std::unique_ptr<Thumb>> thumbs;
    std::set<std::string> thumbs_wanted;  // asked from the helper, not decoded yet
    std::uint64_t use_counter = 0;
    std::unique_ptr<Thumb> avatar;
    lv_timer_t* timer = nullptr;
    int failed_starts = 0;  // helper exits before "ready", in a row

    // decoder thread
    std::mutex decode_mutex;
    std::condition_variable decode_cv;
    std::deque<std::pair<std::string, std::string>> decode_queue;  // vid, path
    std::thread decoder;

    void post(Json j) {
        {
            std::lock_guard<std::mutex> lock(inbox_mutex);
            inbox.push_back(std::move(j));
        }
        platform::wake_main_loop();
    }

    void write_line(const Json& j) {
        if (to_helper < 0) return;
        const std::string line = j.dump() + "\n";
        const char* p = line.data();
        std::size_t left = line.size();
        while (left > 0) {
            const ssize_t n = ::write(to_helper, p, left);
            if (n <= 0) return;
            p += n;
            left -= static_cast<std::size_t>(n);
        }
    }

    void run_decoder() {
        while (!stopping) {
            std::pair<std::string, std::string> job;
            {
                std::unique_lock<std::mutex> lock(decode_mutex);
                decode_cv.wait(lock, [&] { return stopping || !decode_queue.empty(); });
                if (stopping) return;
                job = decode_queue.front();
                decode_queue.pop_front();
            }
            const bool avatar_job = job.first == "@avatar";
            std::vector<std::uint8_t> px;
            const int w = avatar_job ? kAvatar : kThumbW;
            const int h = avatar_job ? kAvatar : kThumbH;
            const bool ok = platform::decode_thumbnail(job.second, w, h, avatar_job ? kAvatar : 4, !avatar_job, px);
            Json done{{"ev", "_decoded"}, {"vid", job.first}, {"ok", ok}};
            // The pixels travel beside the JSON: hand them over by pointer.
            if (ok) done["px"] = reinterpret_cast<std::uintptr_t>(new std::vector<std::uint8_t>(std::move(px)));
            post(std::move(done));
        }
    }
};

YouTube& YouTube::instance() {
    static YouTube yt;
    return yt;
}

YouTube::YouTube() : impl_(std::make_shared<Impl>()) {}

void YouTube::wake_cb(void* self) {
    auto* yt = static_cast<YouTube*>(self);
    if (yt->impl_->timer) lv_timer_ready(yt->impl_->timer);
}

void YouTube::timer_cb(lv_timer_t* timer) {
    static_cast<YouTube*>(lv_timer_get_user_data(timer))->pump();
}

void YouTube::start() {
    auto& d = *impl_;
    if (d.pid > 0) return;
    d.stopping = false;
    if (!d.timer) {
#if USE_DESKTOP
        // The simulator has no wake channel: poll the inbox.
        d.timer = lv_timer_create(timer_cb, 40, this);
#else
        d.timer = lv_timer_create(timer_cb, 1000, this);  // safety net; wakes do the work
        platform::add_wake_handler(wake_cb, this);
#endif
    }
    if (!d.decoder.joinable()) {
        auto impl = impl_;
        d.decoder = std::thread([impl] { impl->run_decoder(); });
    }

    const std::string script = helper_dir() + "/zerotube-helper.py";
    int from_helper = -1;
    d.pid = spawn({python_path(), "-u", script}, &d.to_helper, &from_helper);
    if (d.pid < 0) {
        LOG_ERROR("cannot start the helper ({})", script);
        Json fail{{"ev", "ready"}, {"engine", ""}, {"import_error", "cannot start python3"}, {"signed_in", false}};
        d.post(fail);
        return;
    }
    LOG_INFO("helper started: pid {} ({})", d.pid, script);
    auto impl = impl_;
    const pid_t pid = d.pid;
    std::thread([impl, from_helper, pid] {
        FILE* f = fdopen(from_helper, "r");
        std::string line;
        char buf[8192];
        while (f && std::fgets(buf, sizeof(buf), f)) {
            line += buf;
            if (line.empty() || line.back() != '\n') continue;
            Json j = Json::parse(line, nullptr, false);
            line.clear();
            if (!j.is_discarded()) impl->post(std::move(j));
        }
        if (f) std::fclose(f);
        int status = 0;
        waitpid(pid, &status, 0);
        if (!impl->stopping) impl->post(Json{{"ev", "_helper_exit"}, {"pid", pid}});
    }).detach();
}

void YouTube::stop() {
    auto& d = *impl_;
    d.stopping = true;
    if (d.to_helper >= 0) {
        close(d.to_helper);  // EOF on stdin ends the helper's loop
        d.to_helper = -1;
    }
    if (d.pid > 0) {
        kill(d.pid, SIGTERM);
        d.pid = -1;
    }
    d.decode_cv.notify_all();
    if (d.decoder.joinable()) d.decoder.join();
#if !USE_DESKTOP
    platform::remove_wake_handler(this);
#endif
    if (d.timer) {
        lv_timer_delete(d.timer);
        d.timer = nullptr;
    }
    ready_ = false;
}

void YouTube::request(Json req, Owner owner, Reply reply) {
    auto& d = *impl_;
    const int id = d.next_id++;
    req["id"] = id;
    d.pending[id] = {std::move(owner), std::move(reply)};
    if (!ready_) {
        d.unsent.push_back(std::move(req));
        return;
    }
    d.write_line(req);
}

int YouTube::add_listener(Owner owner, Reply fn) {
    const int id = impl_->next_listener++;
    impl_->listeners[id] = {std::move(owner), std::move(fn)};
    return id;
}

void YouTube::remove_listener(int id) {
    impl_->listeners.erase(id);
}

void YouTube::emit(const Json& event) {
    // Copy first: a listener may add or remove listeners.
    std::vector<std::pair<Owner, Reply>> targets;
    for (auto it = impl_->listeners.begin(); it != impl_->listeners.end();) {
        if (it->second.first.expired()) {
            it = impl_->listeners.erase(it);
            continue;
        }
        targets.push_back(it->second);
        ++it;
    }
    for (auto& t : targets) {
        if (!t.first.expired()) t.second(event);
    }
}

void YouTube::pump() {
    auto& d = *impl_;
    std::deque<Json> batch;
    {
        std::lock_guard<std::mutex> lock(d.inbox_mutex);
        batch.swap(d.inbox);
    }
    for (auto& j : batch) handle(j);
}

void YouTube::handle(const Json& j) {
    auto& d = *impl_;
    if (j.contains("id") && j["id"].is_number_integer()) {
        const int id = j["id"].get<int>();
        auto it = d.pending.find(id);
        if (it == d.pending.end()) return;
        auto entry = std::move(it->second);
        d.pending.erase(it);
        if (!entry.first.expired() && entry.second) entry.second(j);
        return;
    }
    const std::string ev = j.value("ev", "");
    if (ev == "ready") {
        ready_ = true;
        d.failed_starts = 0;
        engine_ = j.value("engine", "");
        import_error_ = j.value("import_error", "");
        signed_in_ = j.value("signed_in", false);
        LOG_INFO("helper ready: yt-dlp '{}' {}", engine_, import_error_);
        for (auto& req : d.unsent) d.write_line(req);
        d.unsent.clear();
        // Thumbnails asked for before the helper was up.
        if (!d.thumbs_wanted.empty()) {
            Json ids = Json::array();
            for (const auto& v : d.thumbs_wanted) ids.push_back(v);
            d.write_line(Json{{"cmd", "thumbs"}, {"ids", ids}, {"id", 0}});
        }
        emit(j);
    } else if (ev == "thumb") {
        std::lock_guard<std::mutex> lock(d.decode_mutex);
        d.decode_queue.emplace_back(j.value("vid", ""), j.value("path", ""));
        d.decode_cv.notify_one();
    } else if (ev == "_decoded") {
        const std::string vid = j.value("vid", "");
        if (!j.value("ok", false)) {
            d.thumbs_wanted.erase(vid);
            return;
        }
        std::unique_ptr<std::vector<std::uint8_t>> px(
            reinterpret_cast<std::vector<std::uint8_t>*>(j.at("px").get<std::uintptr_t>()));
        auto t = std::make_unique<Thumb>();
        t->pixels = std::move(*px);
        if (vid == "@avatar") {
            fill_dsc(*t, kAvatar, kAvatar);
            d.avatar = std::move(t);
        } else {
            fill_dsc(*t, kThumbW, kThumbH);
            t->used = ++d.use_counter;
            d.thumbs[vid] = std::move(t);
            d.thumbs_wanted.erase(vid);
            // Bound memory: drop the least recently shown, never the last 120.
            while (d.thumbs.size() > kThumbCap) {
                auto oldest = d.thumbs.begin();
                for (auto it = d.thumbs.begin(); it != d.thumbs.end(); ++it) {
                    if (it->second->used < oldest->second->used) oldest = it;
                }
                if (d.use_counter - oldest->second->used < 120) break;
                d.thumbs.erase(oldest);
            }
        }
        emit(Json{{"ev", "thumb_ready"}, {"vid", vid}});
    } else if (ev == "_helper_exit") {
        if (j.value("pid", -1) != d.pid) return;  // an old helper we replaced
        LOG_WARN("helper exited; restarting");
        d.pid = -1;
        if (d.to_helper >= 0) {
            close(d.to_helper);
            d.to_helper = -1;
        }
        const bool ready_was = ready_;
        ready_ = false;
        // Fail what was in flight rather than leave spinners up.
        auto pending = std::move(d.pending);
        d.pending.clear();
        for (auto& [id, entry] : pending) {
            if (!entry.first.expired() && entry.second) {
                entry.second(Json{{"id", id}, {"ok", false}, {"error", "YouTube helper stopped"}});
            }
        }
        // A helper that dies before it is even ready (missing script, broken
        // Python) would otherwise respawn in a tight loop: back off, and after
        // three tries tell the UI instead.
        if (!ready_was) {
            if (++d.failed_starts >= 3) {
                ready_ = true;
                import_error_ = "The YouTube helper does not start";
                emit(Json{{"ev", "ready"}, {"engine", ""}, {"import_error", import_error_}});
                return;
            }
        }
        lv_timer_t* retry = lv_timer_create(
            [](lv_timer_t* t) {
                // repeat count 1: LVGL deletes the timer after this call.
                static_cast<YouTube*>(lv_timer_get_user_data(t))->start();
            },
            ready_was ? 500 : 2000, this);
        lv_timer_set_repeat_count(retry, 1);
    } else if (ev == "engine_step" || ev == "engine_done") {
        if (ev == "engine_done") {
            engine_busy_ = false;
            // Restart the helper so it imports the new yt-dlp.
            if (d.pid > 0) {
                const pid_t old = d.pid;
                d.pid = -1;
                if (d.to_helper >= 0) {
                    close(d.to_helper);
                    d.to_helper = -1;
                }
                kill(old, SIGTERM);
            }
            ready_ = false;
            start();
        }
        emit(j);
    } else {
        emit(j);
    }
}

void YouTube::setup_engine() {
    if (engine_busy_) return;
    engine_busy_ = true;
    auto impl = impl_;
    const std::string script = helper_dir() + "/setup-engine.sh";
    std::thread([impl, script] {
        int from = -1;
        const pid_t pid = spawn({"/bin/sh", script}, nullptr, &from);
        if (pid < 0) {
            impl->post(Json{{"ev", "engine_done"}, {"ok", false}, {"text", "Cannot run the setup script"}});
            return;
        }
        FILE* f = fdopen(from, "r");
        char buf[1024];
        std::string last;
        while (f && std::fgets(buf, sizeof(buf), f)) {
            std::string line(buf);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (line.rfind("STEP ", 0) == 0) {
                last = line.substr(5);
                impl->post(Json{{"ev", "engine_step"}, {"text", last}});
            }
        }
        if (f) std::fclose(f);
        int status = 0;
        waitpid(pid, &status, 0);
        const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        impl->post(Json{{"ev", "engine_done"}, {"ok", ok}, {"text", ok ? last : "Setup failed (no internet?)"}});
    }).detach();
}

const lv_image_dsc_t* YouTube::thumbnail(const std::string& vid) {
    auto& d = *impl_;
    auto it = d.thumbs.find(vid);
    if (it != d.thumbs.end()) {
        it->second->used = ++d.use_counter;
        return &it->second->dsc;
    }
    prefetch_thumbnails({vid});
    return nullptr;
}

void YouTube::prefetch_thumbnails(const std::vector<std::string>& vids) {
    auto& d = *impl_;
    Json ids = Json::array();
    for (const auto& v : vids) {
        if (d.thumbs.count(v) || d.thumbs_wanted.count(v)) continue;
        d.thumbs_wanted.insert(v);
        ids.push_back(v);
    }
    if (ids.empty() || !ready_) return;
    // id 0: nobody waits for this reply; the thumbs come as events.
    d.write_line(Json{{"cmd", "thumbs"}, {"ids", ids}, {"id", 0}});
}

const lv_image_dsc_t* YouTube::avatar() const {
    return impl_->avatar ? &impl_->avatar->dsc : nullptr;
}

void YouTube::load_avatar(const std::string& path) {
    if (path.empty()) {
        impl_->avatar.reset();
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->decode_mutex);
    impl_->decode_queue.emplace_front("@avatar", path);
    impl_->decode_cv.notify_one();
}

}  // namespace net
