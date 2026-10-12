// engine: commands, FramePlan, FrameSource concept (frozen contract, plan §3.2).
#pragma once

#include <array>
#include <concepts>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

#include "engine/core/static.hpp"
#include "engine/core/types.hpp"
#include "gfx/core/gfx.hpp"

namespace engine {

struct FramePlan {
    StaticVec<gfx::Rect, 16> dirty;
    bool full_redraw = false;
    bool trim = true;
    std::uint32_t delay_ms = 33;
    bool finished = false;
};

template <class S>
concept FrameSource = requires(S s, std::uint32_t now, gfx::Rect r, std::span<gfx::Rgb565be> buf) {
    { s.next(now) } noexcept -> std::same_as<FramePlan>;
    { s.fill(r, buf) } noexcept -> std::same_as<std::expected<void, Err>>;
    { s.stop() } noexcept;
    { S::kName } -> std::convertible_to<std::string_view>;
};

// Play targets besides the 12 animation ids (0..11) of cat_eyes.
inline constexpr std::uint8_t kPlayBlink = 200, kPlayRandom = 201;

struct EyeCmd {
    std::optional<std::uint8_t> style;                    // cat_style_t
    std::optional<std::uint8_t> iris;                     // cat_iris_t
    std::optional<std::uint8_t> play;                     // animation id | kPlayBlink | kPlayRandom
    std::optional<bool> auto_idle;                        // random idle animations on/off
    std::optional<std::array<std::int16_t, 2>> look_q8;   // gaze -256..256 (= -1..1)
    std::uint16_t hold_ms = 1000;                         // gaze hold time for look
    StaticString<31> fx;                                  // custom effect (.efx) to play; the caller checks it exists
    bool save = false;                                    // persist style + iris + auto
};

struct PlayCmd {
    std::array<StaticString<31>, kEyes> clip;  // empty = keep eyes on that display
    bool loop = false;
    bool then_eyes = true;
};
struct StopCmd {};
using Command = std::variant<EyeCmd, PlayCmd, StopCmd>;
static_assert(std::is_trivially_copyable_v<Command>);

[[nodiscard]] constexpr bool clip_name_valid(std::string_view n) noexcept {
    if (n.empty() || n.size() > 31) return false;
    for (char c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

}  // namespace engine
