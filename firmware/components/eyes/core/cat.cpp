#include "eyes/core/cat.hpp"

#include "cat_eyes.h"

namespace eyes {
namespace {
FxLoader g_fx_loader = nullptr;
bool g_auto = true;  // random idle on/off; the animation choice is made by engine::Scene, not by cat_eyes
}

void set_fx_loader(FxLoader l) noexcept { g_fx_loader = l; }

bool CatModel::play_custom(std::string_view name) noexcept {
    if (!g_fx_loader || !engine::clip_name_valid(name)) return false;
    const auto bytes = g_fx_loader(name);
    if (bytes.empty()) return false;
    const engine::StaticString<31> z(name);  // NUL-terminated copy for the C API
    return cat_eyes_efx_play(z.c_str(), bytes.data(), bytes.size());
}

void CatModel::start(std::uint32_t seed, std::uint8_t style, std::uint8_t iris, bool auto_idle) noexcept {
    rng_ = seed | 1u;
    cat_eyes_init(seed);
    cat_eyes_set_swap_bytes(true);  // GC9A01 wants big-endian RGB565
    cat_eyes_set_style(static_cast<cat_style_t>(style < CAT_STYLE_COUNT ? static_cast<int>(style) : 0));
    cat_eyes_set_iris(static_cast<cat_iris_t>(iris < CAT_IRIS_COUNT ? static_cast<int>(iris) : 0));
    g_auto = auto_idle;
    cat_eyes_set_auto(false);  // saccades and blinks still run; random animation picks are done by the Scene
}

void CatModel::apply(const engine::EyeCmd& c, std::uint32_t) noexcept {
    if (c.style && *c.style < CAT_STYLE_COUNT) cat_eyes_set_style(static_cast<cat_style_t>(*c.style));
    if (c.iris && *c.iris < CAT_IRIS_COUNT) cat_eyes_set_iris(static_cast<cat_iris_t>(*c.iris));
    if (c.auto_idle) g_auto = *c.auto_idle;
    if (c.play) {
        if (*c.play == engine::kPlayBlink) {
            cat_eyes_blink();
        } else if (*c.play == engine::kPlayRandom) {
            rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
            const bool has_special = cat_eyes_get_style() >= CAT_STYLE_ANIME;
            const std::uint32_t n = has_special ? CAT_ANIM_COUNT : CAT_ANIM_COUNT - 1;
            cat_eyes_play(static_cast<cat_anim_t>(rng_ % n));
        } else if (*c.play < CAT_ANIM_COUNT) {
            cat_eyes_play(static_cast<cat_anim_t>(*c.play));
        }
    }
    if (!c.fx.empty()) play_custom(c.fx.view());
    if (c.look_q8) cat_eyes_look((*c.look_q8)[0] / 256.f, (*c.look_q8)[1] / 256.f, c.hold_ms);
    if (c.save) save_pending_ = true;
}

bool CatModel::animating() noexcept { return cat_eyes_current_anim() != CAT_ANIM_NONE; }
bool CatModel::style_has_special() noexcept { return cat_eyes_get_style() >= CAT_STYLE_ANIME; }
void CatModel::play_anim(int id) noexcept {
    if (id >= 0 && id < CAT_ANIM_COUNT) cat_eyes_play(static_cast<cat_anim_t>(id));
}

void CatModel::tick(std::uint32_t now) noexcept { cat_eyes_tick(now); }

CatInfo CatModel::info() noexcept {
    const auto style = cat_eyes_get_style();
    CatInfo i{};
    i.style = static_cast<std::uint8_t>(style);
    i.iris = static_cast<std::uint8_t>(cat_eyes_get_iris());
    i.auto_idle = g_auto;
    i.anim = cat_eyes_anim_name(cat_eyes_current_anim());
    i.style_name = kStyleNames[i.style < kStyleNames.size() ? i.style : 0];
    i.iris_name = kIrisNames[i.iris < kIrisNames.size() ? i.iris : 0];
    return i;
}

void CatView::invalidate() noexcept { cat_eyes_force_full(static_cast<int>(eye_)); }

engine::FramePlan CatView::next(std::uint32_t) noexcept {
    engine::FramePlan p;
    p.trim = false;  // cat_eyes renders full 240-px rows
    p.delay_ms = 33;
    int y0 = 0, y1 = 0;
    if (cat_eyes_dirty_rows(static_cast<int>(eye_), &y0, &y1)) {
        (void)p.dirty.push_back(gfx::Rect{0, static_cast<std::int16_t>(y0), gfx::kScreen, static_cast<std::int16_t>(y1 - y0)});
        p.full_redraw = (y0 == 0 && y1 == gfx::kScreen);
    } else {
        p.delay_ms = 50;
    }
    return p;
}

std::expected<void, engine::Err> CatView::fill(gfx::Rect r, std::span<gfx::Rgb565be> buf) noexcept {
    if (r.x != 0 || r.w != gfx::kScreen || buf.size() < static_cast<std::size_t>(r.area())) return std::unexpected(engine::Err::Io);
    cat_eyes_render_strip(static_cast<int>(eye_), r.y, r.h, buf.data());
    return {};
}

}  // namespace eyes
