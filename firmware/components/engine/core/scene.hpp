// engine: per-display source selection (eyes vs clip) on top of the shared eye model (plan §3.2).
// Two fixed source kinds instead of a variant: no heap, no visit, same behaviour.
#pragma once

#include <algorithm>
#include <array>
#include <expected>
#include <span>
#include <string_view>

#include "engine/core/command.hpp"
#include "engine/core/pool.hpp"
#include "eyes/core/cat.hpp"

namespace engine {

template <class Clip>
concept ClipSource = FrameSource<Clip> && requires(Clip c, std::string_view n, bool loop) {
    { c.start(n, loop) } noexcept -> std::same_as<std::expected<void, Err>>;
};

// Source that never plays anything (before clips exist / for tests).
struct NoClip {
    static constexpr std::string_view kName = "clip";
    std::expected<void, Err> start(std::string_view, bool) noexcept { return std::unexpected(Err::NotFound); }
    FramePlan next(std::uint32_t) noexcept { FramePlan p; p.finished = true; return p; }
    std::expected<void, Err> fill(gfx::Rect, std::span<gfx::Rgb565be>) noexcept { return {}; }
    void stop() noexcept {}
};

template <ClipSource Clip>
class Scene {
public:
    // style/iris default to realistic/amber (cat_eyes enums 0/0)
    explicit Scene(std::uint32_t seed = 1, std::uint8_t style = 0, std::uint8_t iris = 0, bool auto_idle = true) noexcept
        : views_{eyes::CatView(Eye::Left), eyes::CatView(Eye::Right)} {
        model_.start(seed, style, iris, auto_idle);
    }

    void apply(const Command& cmd, std::uint32_t now) noexcept {
        if (const auto* e = std::get_if<EyeCmd>(&cmd)) {
            if (e->play && *e->play == kPlayRandom) {  // one pick from the pool, right now
                EyeCmd rest = *e;
                rest.play.reset();
                model_.apply(rest, now);
                pick_now(now);
            } else {
                model_.apply(*e, now);
            }
        } else if (const auto* p = std::get_if<PlayCmd>(&cmd)) {
            for (std::size_t i = 0; i < kEyes; ++i) {
                if (p->clip[i].empty()) continue;
                if (clips_[i].start(p->clip[i].view(), p->loop)) {
                    active_[i] = Src::Playing;
                    then_eyes_[i] = p->then_eyes;
                }
            }
        } else {
            stop_all();
        }
    }

    void set_pool(const Pool& p) noexcept { pool_ = p; }
    const Pool& pool() const noexcept { return pool_; }

    void tick(std::uint32_t now) noexcept {  // once per frame, before next()
        model_.tick(now);
        schedule(now);
    }

    FramePlan next(Eye e, std::uint32_t now) noexcept {
        const auto i = idx(e);
        if (active_[i] == Src::Playing) {
            FramePlan p = clips_[i].next(now);
            if (!p.finished) return p;
            clips_[i].stop();
            if (then_eyes_[i]) {
                active_[i] = Src::Eyes;
                views_[i].invalidate();
                return views_[i].next(now);
            }
            FramePlan hold;  // keep the last clip frame on screen
            hold.delay_ms = 100;
            return hold;
        }
        return views_[i].next(now);
    }

    std::expected<void, Err> fill(Eye e, gfx::Rect r, std::span<gfx::Rgb565be> buf) noexcept {
        const auto i = idx(e);
        return active_[i] == Src::Playing ? clips_[i].fill(r, buf) : views_[i].fill(r, buf);
    }

    // next() reports the whole eye on both displays (after a display re-init or test pattern)
    void invalidate() noexcept { for (auto& v : views_) v.invalidate(); }

    std::string_view current(Eye e) const noexcept {
        return active_[idx(e)] == Src::Playing ? Clip::kName : eyes::CatView::kName;
    }
    bool save_pending() const noexcept { return model_.save_pending(); }
    void saved() noexcept { model_.saved(); }
    static eyes::CatInfo eye_info() noexcept { return eyes::CatModel::info(); }
    Clip& clip(Eye e) noexcept { return clips_[idx(e)]; }

private:
    // Random mode: when nothing is playing, wait a random pause, then pick one item from the pool.
    void schedule(std::uint32_t now) noexcept {
        if (!eye_info().auto_idle) { next_pick_ = 0; return; }
        bool busy = eyes::CatModel::animating();
        for (const auto a : active_) busy |= (a == Src::Playing);
        if (busy) { next_pick_ = 0; return; }
        if (next_pick_ == 0) {
            const std::uint32_t lo = pool_.min_ds * 100u, hi = std::max<std::uint32_t>(pool_.max_ds * 100u, lo);
            rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
            next_pick_ = now + lo + rng_ % (hi - lo + 1);
            if (next_pick_ == 0) next_pick_ = 1;
            return;
        }
        if (static_cast<std::int32_t>(now - next_pick_) >= 0) { pick_now(now); next_pick_ = 0; }
    }
    void pick_now(std::uint32_t now) noexcept {
        const Pick p = pick(pool_, rng_, last_key_, eyes::CatModel::style_has_special());
        if (p.kind == Pick::Kind::Anim) {
            eyes::CatModel::play_anim(p.index);
            last_key_ = pick_key(p);
        } else if (p.kind == Pick::Kind::Clip) {
            if (eyes::CatModel::play_custom(pool_.clips[static_cast<std::size_t>(p.index)].view())) {  // an .efx effect
                last_key_ = pick_key(p);
                return;
            }
            PlayCmd c;
            c.clip[0] = c.clip[1] = pool_.clips[static_cast<std::size_t>(p.index)];
            c.then_eyes = true;
            apply(Command{c}, now);  // a missing clip simply does not start
            last_key_ = pick_key(p);
        }
    }

    enum class Src : std::uint8_t { Eyes, Playing };
    static constexpr std::size_t idx(Eye e) noexcept { return static_cast<std::size_t>(e); }
    void stop_all() noexcept {
        for (std::size_t i = 0; i < kEyes; ++i) {
            if (active_[i] == Src::Playing) { clips_[i].stop(); active_[i] = Src::Eyes; views_[i].invalidate(); }
        }
    }

    Pool pool_;
    std::uint32_t rng_ = 0x2545F491u, next_pick_ = 0;
    int last_key_ = -1;
    eyes::CatModel model_;
    std::array<eyes::CatView, kEyes> views_;
    std::array<Clip, kEyes> clips_{};
    std::array<Src, kEyes> active_{Src::Eyes, Src::Eyes};
    std::array<bool, kEyes> then_eyes_{true, true};
};

}  // namespace engine
