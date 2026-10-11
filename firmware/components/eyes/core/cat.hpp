// eyes: cat_eyes (C component) wrapped as a model + per-display FrameSource.
// cat_eyes keeps global state for both eyes: exactly one CatModel may exist, used from the render task only.
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include "engine/core/command.hpp"

namespace eyes {

// Catalogue, in the enum order of cat_eyes.h (checked against the component's name helpers in tests).
inline constexpr std::array<std::string_view, 7> kStyleNames = {"realistic", "flat", "line", "anime", "cute", "halloween", "christmas"};
inline constexpr std::array<std::string_view, 6> kIrisNames = {"amber", "green", "blue", "copper", "violet", "odd"};
inline constexpr std::array<std::string_view, 31> kAnimNames = {
    "wink", "slow_blink", "roll", "side_eye", "hunt", "startle", "nictitate", "sleepy", "cross_eyed", "happy", "glance",
    // effects (cat_eyes_fx.c)
    "food", "fly", "paper_plane", "bubble", "spiral", "slots", "euro", "fish", "rain", "snow",
    "leaf", "day_night", "paw", "spider", "loading", "pacman", "matrix", "disco", "pumpkin",
    "special"};
// special animation name per style ("" = none)
inline constexpr std::array<std::string_view, 7> kSpecialNames = {"", "", "", "sparkle", "heart_eyes", "demon_flare", "twinkle"};

// Custom effects (.efx): the loader returns the validated file bytes for a name (empty span = unknown). The span
// must stay valid until the next call that returns a non-empty span (the loader alternates two buffers, so an
// effect that is still running is never overwritten by a failed load).
using FxLoader = std::span<const std::uint8_t> (*)(std::string_view name) noexcept;
void set_fx_loader(FxLoader) noexcept;

struct CatInfo {
    std::uint8_t style, iris;
    bool auto_idle;
    std::string_view anim;  // "idle" or the running animation's name
    std::string_view style_name, iris_name;
};

class CatModel {
public:
    void start(std::uint32_t seed, std::uint8_t style, std::uint8_t iris, bool auto_idle) noexcept;
    void apply(const engine::EyeCmd&, std::uint32_t now) noexcept;
    void tick(std::uint32_t now) noexcept;  // once per frame, before any CatView::next()
    static CatInfo info() noexcept;
    static bool animating() noexcept;                  // a built-in animation is running
    static void play_anim(int id) noexcept;            // start a built-in animation (ignores unusable ids)
    static bool play_custom(std::string_view name) noexcept;  // start a custom effect; false if unknown / no loader
    static bool style_has_special() noexcept;
    bool save_pending() const noexcept { return save_pending_; }
    void saved() noexcept { save_pending_ = false; }

private:
    std::uint32_t rng_ = 1;
    bool save_pending_ = false;
};

class CatView {
public:
    static constexpr std::string_view kName = "eyes";
    explicit CatView(engine::Eye e) noexcept : eye_(e) {}
    engine::FramePlan next(std::uint32_t now) noexcept;
    std::expected<void, engine::Err> fill(gfx::Rect, std::span<gfx::Rgb565be>) noexcept;
    void stop() noexcept {}
    void invalidate() noexcept;  // next() reports the whole eye

private:
    engine::Eye eye_;
};

static_assert(engine::FrameSource<CatView>);

}  // namespace eyes
