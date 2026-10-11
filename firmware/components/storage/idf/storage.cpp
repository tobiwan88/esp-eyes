#include "storage/idf/storage.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "cat_eyes.h"
#include "clip/core/clip.hpp"
#include "config.hpp"
#include "engine/core/command.hpp"
#include "esp_littlefs.h"
#include "eyes/core/cat.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace storage {
namespace {

const char* const kTag = "storage";
constexpr const char* kDir = "/fs/clips";
constexpr const char* kEfxDir = "/fs/efx";
std::array<engine::StaticString<31>, 2> g_playing;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> g_upload_active{false};

bool rd(void* ctx, std::uint32_t off, std::span<std::byte> out) noexcept {
    const int fd = *static_cast<int*>(ctx);
    return lseek(fd, static_cast<off_t>(off), SEEK_SET) == static_cast<off_t>(off) &&
           read(fd, out.data(), out.size()) == static_cast<ssize_t>(out.size());
}

bool tmp_path(std::string_view name, char* out, std::size_t cap) {
    return std::snprintf(out, cap, "%s/.tmp-%.*s", kDir, static_cast<int>(name.size()), name.data()) < static_cast<int>(cap);
}

std::expected<ClipInfo, engine::Err> info_of(std::string_view name, const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return std::unexpected(engine::Err::NotFound);
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return std::unexpected(engine::Err::Io);
    std::array<std::byte, sizeof(clip::Header)> hb{};
    const bool ok = read(fd, hb.data(), hb.size()) == static_cast<ssize_t>(hb.size());
    std::uint32_t total_ms = 0;
    auto h = clip::parse_header(hb);
    if (ok && h) {
        for (std::uint16_t i = 0; i < h->frame_count; ++i) {
            clip::IndexEntry e{};
            if (lseek(fd, static_cast<off_t>(h->index_offset + i * sizeof e), SEEK_SET) < 0 || read(fd, &e, sizeof e) != sizeof e) break;
            total_ms += e.duration_ms;
        }
    }
    close(fd);
    if (!ok || !h) return std::unexpected(engine::Err::BadClip);
    ClipInfo ci{};
    ci.name.assign(name);
    ci.size = static_cast<std::uint32_t>(st.st_size);
    ci.frames = h->frame_count;
    ci.fps_x10 = total_ms ? static_cast<std::uint16_t>(h->frame_count * 10000ULL / total_ms) : 0;
    return ci;
}

}  // namespace

std::expected<void, engine::Err> init() noexcept {
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/fs";
    conf.partition_label = "storage";
    conf.format_if_mount_failed = true;
    conf.dont_mount = false;
    if (esp_vfs_littlefs_register(&conf) != ESP_OK) return std::unexpected(engine::Err::Io);
    mkdir(kDir, 0775);
    mkdir(kEfxDir, 0775);
    for (const char* dir : {kDir, kEfxDir}) {
        if (DIR* d = opendir(dir)) {  // remove stale uploads
            while (dirent* e = readdir(d)) {
                if (std::strncmp(e->d_name, ".tmp-", 5) == 0) {
                    char p[320];
                    std::snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
                    unlink(p);
                }
            }
            closedir(d);
        }
    }
    ESP_LOGI(kTag, "mounted, free=%lu", static_cast<unsigned long>(free_bytes()));
    return {};
}

bool path_for(std::string_view name, char* out, std::size_t cap) noexcept {
    return engine::clip_name_valid(name) &&
           std::snprintf(out, cap, "%s/%.*s.clip", kDir, static_cast<int>(name.size()), name.data()) < static_cast<int>(cap);
}

std::expected<std::vector<ClipInfo>, engine::Err> list() noexcept {
    std::vector<ClipInfo> out;
    DIR* d = opendir(kDir);
    if (!d) return std::unexpected(engine::Err::Io);
    while (dirent* e = readdir(d)) {
        std::string_view n = e->d_name;
        if (n.size() < 6 || n.substr(n.size() - 5) != ".clip" || n[0] == '.') continue;
        const std::string_view base = n.substr(0, n.size() - 5);
        char p[96];
        if (!path_for(base, p, sizeof p)) continue;
        if (auto i = info_of(base, p); i && out.size() < 64) out.push_back(*i);
    }
    closedir(d);
    return out;
}

std::expected<ClipInfo, engine::Err> info(std::string_view name) noexcept {
    char p[96];
    if (!path_for(name, p, sizeof p)) return std::unexpected(engine::Err::BadName);
    return info_of(name, p);
}

std::expected<void, engine::Err> remove(std::string_view name) noexcept {
    char p[96];
    if (!path_for(name, p, sizeof p)) return std::unexpected(engine::Err::BadName);
    if (is_playing(name)) return std::unexpected(engine::Err::Conflict);
    return unlink(p) == 0 ? std::expected<void, engine::Err>{} : std::unexpected(engine::Err::NotFound);
}

std::uint32_t free_bytes() noexcept {
    std::size_t total = 0, used = 0;
    if (esp_littlefs_info("storage", &total, &used) != ESP_OK) return 0;
    return static_cast<std::uint32_t>(total - used);
}

std::uint32_t max_clip_bytes() noexcept { return CONFIG_EYES_MAX_CLIP_BYTES; }

void mark_playing(int slot, std::string_view name) noexcept {
    taskENTER_CRITICAL(&g_mux);
    g_playing[static_cast<std::size_t>(slot)].assign(name);
    taskEXIT_CRITICAL(&g_mux);
}
void clear_playing(int slot) noexcept { mark_playing(slot, {}); }
bool is_playing(std::string_view name) noexcept {
    taskENTER_CRITICAL(&g_mux);
    bool p = false;
    for (const auto& n : g_playing) p |= (!n.empty() && n.view() == name);
    taskEXIT_CRITICAL(&g_mux);
    return p;
}

// ---- custom effects ----
namespace {
bool efx_path(std::string_view name, char* out, std::size_t cap, bool tmp = false) {
    return engine::clip_name_valid(name) &&
           std::snprintf(out, cap, "%s/%s%.*s.efx", kEfxDir, tmp ? ".tmp-" : "", static_cast<int>(name.size()), name.data()) <
               static_cast<int>(cap);
}

std::expected<std::size_t, engine::Err> read_file(const char* path, std::span<std::uint8_t> out) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return std::unexpected(engine::Err::NotFound);
    const ssize_t n = read(fd, out.data(), out.size());
    struct stat st;
    const bool whole = fstat(fd, &st) == 0 && st.st_size == n;
    close(fd);
    if (n <= 0 || !whole) return std::unexpected(engine::Err::Io);
    return static_cast<std::size_t>(n);
}

std::expected<EfxInfo, engine::Err> efx_info_of(std::string_view name, std::span<const std::uint8_t> d) {
    EfxInfo i{};
    i.name.assign(name);
    i.size = static_cast<std::uint32_t>(d.size());
    i.duration_ms = static_cast<std::uint16_t>(d[6] | (d[7] << 8));
    i.shapes = d[8];
    return i;
}
}  // namespace

bool efx_exists(std::string_view name) noexcept {
    char p[96];
    struct stat st;
    return efx_path(name, p, sizeof p) && stat(p, &st) == 0;
}

std::expected<std::vector<EfxInfo>, engine::Err> efx_list() noexcept {
    std::vector<EfxInfo> out;
    DIR* d = opendir(kEfxDir);
    if (!d) return std::unexpected(engine::Err::Io);
    std::array<std::uint8_t, 16> hdr;
    while (dirent* e = readdir(d)) {
        std::string_view n = e->d_name;
        if (n.size() < 5 || n.substr(n.size() - 4) != ".efx" || n[0] == '.' || out.size() >= static_cast<std::size_t>(kMaxEfx)) continue;
        const std::string_view base = n.substr(0, n.size() - 4);
        char p[96];
        if (!efx_path(base, p, sizeof p)) continue;
        struct stat st;
        const int fd = open(p, O_RDONLY);
        if (fd < 0) continue;
        const bool ok = read(fd, hdr.data(), hdr.size()) == static_cast<ssize_t>(hdr.size()) && fstat(fd, &st) == 0;
        close(fd);
        if (!ok) continue;
        auto i = efx_info_of(base, hdr);
        i->size = static_cast<std::uint32_t>(st.st_size);
        out.push_back(*i);
    }
    closedir(d);
    return out;
}

std::expected<EfxInfo, engine::Err> efx_put(std::string_view name, std::span<const std::uint8_t> data) noexcept {
    if (!engine::clip_name_valid(name)) return std::unexpected(engine::Err::BadName);
    for (const auto a : eyes::kAnimNames)
        if (a == name) return std::unexpected(engine::Err::BadName);
    if (name == "blink" || name == "random" || name == "idle") return std::unexpected(engine::Err::BadName);
    if (data.empty() || data.size() > kMaxEfxBytes) return std::unexpected(engine::Err::TooLarge);
    if (cat_eyes_efx_check(data.data(), data.size()) != 0) return std::unexpected(engine::Err::BadClip);
    if (info(name)) return std::unexpected(engine::Err::Conflict);  // a clip already uses this name
    if (!efx_exists(name)) {
        auto l = efx_list();
        if (!l) return std::unexpected(l.error());
        if (l->size() >= static_cast<std::size_t>(kMaxEfx)) return std::unexpected(engine::Err::NoSpace);
    }
    char tp[96], fp[96];
    if (!efx_path(name, tp, sizeof tp, true) || !efx_path(name, fp, sizeof fp)) return std::unexpected(engine::Err::BadName);
    const int fd = open(tp, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd < 0) return std::unexpected(engine::Err::Io);
    const bool wrote = ::write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size());
    close(fd);
    if (!wrote) { unlink(tp); return std::unexpected(engine::Err::NoSpace); }
    if (rename(tp, fp) != 0) { unlink(tp); return std::unexpected(engine::Err::Io); }
    return efx_info_of(name, data);
}

std::expected<void, engine::Err> efx_remove(std::string_view name) noexcept {
    char p[96];
    if (!efx_path(name, p, sizeof p)) return std::unexpected(engine::Err::BadName);
    return unlink(p) == 0 ? std::expected<void, engine::Err>{} : std::unexpected(engine::Err::NotFound);
}

std::expected<std::size_t, engine::Err> efx_read(std::string_view name, std::span<std::uint8_t> out) noexcept {
    char p[96];
    if (!efx_path(name, p, sizeof p)) return std::unexpected(engine::Err::BadName);
    return read_file(p, out);
}

// ---- Upload ----
std::expected<Upload, engine::Err> Upload::begin(std::string_view name, std::uint32_t total) noexcept {
    if (!engine::clip_name_valid(name)) return std::unexpected(engine::Err::BadName);
    if (total == 0 || total > max_clip_bytes()) return std::unexpected(engine::Err::TooLarge);
    if (total + 65536 > free_bytes() + (info(name) ? info(name)->size : 0)) return std::unexpected(engine::Err::NoSpace);
    bool expected = false;
    if (!g_upload_active.compare_exchange_strong(expected, true)) return std::unexpected(engine::Err::Busy);
    char p[96];
    tmp_path(name, p, sizeof p);
    Upload u;
    u.fd_ = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (u.fd_ < 0) { g_upload_active = false; return std::unexpected(engine::Err::Io); }
    u.total_ = total;
    u.name_.assign(name);
    u.active_ = true;
    return u;
}

Upload::Upload(Upload&& o) noexcept
    : fd_(o.fd_), total_(o.total_), written_(o.written_), name_(o.name_), active_(o.active_) {
    o.active_ = false;
    o.fd_ = -1;
}

Upload::~Upload() {
    if (!active_) return;
    if (fd_ >= 0) close(fd_);
    char p[96];
    tmp_path(name_.view(), p, sizeof p);
    unlink(p);
    g_upload_active = false;
}

std::expected<void, engine::Err> Upload::write(std::span<const std::byte> data) noexcept {
    if (written_ + data.size() > total_) return std::unexpected(engine::Err::TooLarge);
    if (::write(fd_, data.data(), data.size()) != static_cast<ssize_t>(data.size())) return std::unexpected(engine::Err::NoSpace);
    written_ += static_cast<std::uint32_t>(data.size());
    return {};
}

std::expected<ClipInfo, engine::Err> Upload::commit() noexcept {
    if (written_ != total_) return std::unexpected(engine::Err::BadClip);
    close(fd_);
    fd_ = -1;
    char tp[96], fp[96];
    tmp_path(name_.view(), tp, sizeof tp);
    if (!path_for(name_.view(), fp, sizeof fp)) return std::unexpected(engine::Err::BadName);
    int rfd = open(tp, O_RDONLY);
    if (rfd < 0) return std::unexpected(engine::Err::Io);
    auto v = clip::validate(clip::Reader{rd, &rfd}, total_);
    close(rfd);
    if (!v) return std::unexpected(v.error());
    if (is_playing(name_.view())) return std::unexpected(engine::Err::Conflict);
    if (rename(tp, fp) != 0) return std::unexpected(engine::Err::Io);
    active_ = false;  // renamed: nothing to unlink
    g_upload_active = false;
    return info_of(name_.view(), fp);
}

}  // namespace storage
