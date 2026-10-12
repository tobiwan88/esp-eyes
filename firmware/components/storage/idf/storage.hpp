// storage: LittleFS clip store with atomic uploads (plan §3.7).
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include "engine/core/static.hpp"
#include "engine/core/types.hpp"

namespace storage {

[[nodiscard]] std::expected<void, engine::Err> init() noexcept;  // mount /fs, create /fs/clips, delete stale .tmp-*

struct ClipInfo { engine::StaticString<31> name; std::uint32_t size; std::uint16_t frames, fps_x10; };
[[nodiscard]] std::expected<std::vector<ClipInfo>, engine::Err> list() noexcept;  // heap result: keeps stacks small
[[nodiscard]] std::expected<ClipInfo, engine::Err> info(std::string_view name) noexcept;
[[nodiscard]] std::expected<void, engine::Err> remove(std::string_view name) noexcept;  // Conflict if playing
[[nodiscard]] std::uint32_t free_bytes() noexcept;
[[nodiscard]] std::uint32_t max_clip_bytes() noexcept;
[[nodiscard]] bool path_for(std::string_view name, char* out, std::size_t cap) noexcept;  // /fs/clips/<name>.clip

// Custom effects (.efx files, see docs/efx-format.md): at most kMaxEfx slots in /fs/efx, each <= kMaxEfxBytes.
// Uploading under an existing name replaces it; when all slots are taken a new name is refused (NoSpace).
inline constexpr int kMaxEfx = 16;
inline constexpr std::size_t kMaxEfxBytes = 4096;
struct EfxInfo { engine::StaticString<31> name; std::uint32_t size; std::uint16_t duration_ms; std::uint8_t shapes; };
[[nodiscard]] std::expected<std::vector<EfxInfo>, engine::Err> efx_list() noexcept;
[[nodiscard]] bool efx_exists(std::string_view name) noexcept;
// Validates (BadClip), refuses built-in animation names (BadName) and clip names (Conflict), then writes atomically.
[[nodiscard]] std::expected<EfxInfo, engine::Err> efx_put(std::string_view name, std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] std::expected<void, engine::Err> efx_remove(std::string_view name) noexcept;
[[nodiscard]] std::expected<std::size_t, engine::Err> efx_read(std::string_view name, std::span<std::uint8_t> out) noexcept;

// "Playing" registry: players mark the clip they hold open so it cannot be replaced or deleted.
void mark_playing(int slot, std::string_view name) noexcept;
void clear_playing(int slot) noexcept;
bool is_playing(std::string_view name) noexcept;

// RAII upload: writes /fs/clips/.tmp-<name>, unlinks it unless committed. One at a time.
class Upload {
public:
    static std::expected<Upload, engine::Err> begin(std::string_view name, std::uint32_t total) noexcept;
    Upload(Upload&&) noexcept;
    Upload& operator=(Upload&&) = delete;
    Upload(const Upload&) = delete;
    ~Upload();
    [[nodiscard]] std::expected<void, engine::Err> write(std::span<const std::byte>) noexcept;
    [[nodiscard]] std::expected<ClipInfo, engine::Err> commit() noexcept;  // length check + validate + CRC + atomic rename

private:
    Upload() = default;
    int fd_ = -1;
    std::uint32_t total_ = 0, written_ = 0;
    engine::StaticString<31> name_;
    bool active_ = false;
};

}  // namespace storage
