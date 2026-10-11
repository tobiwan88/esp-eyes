#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "cat_eyes.h"
#include "cat_eyes_internal.h"
#include "doctest.h"
#include "engine/core/scene.hpp"

namespace {
std::vector<std::uint8_t> load(const char* name) {  // examples/efx/<name>.efx, built by tools/efxc.py
    std::ifstream f(std::string(EFX_DIR) + "/" + name + ".efx", std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
void set_crc(std::vector<std::uint8_t>& d) {  // CRC-16/CCITT-FALSE over the payload, as the compiler writes it
    std::uint16_t c = 0xFFFF;
    for (std::size_t i = 16; i < d.size(); ++i) {
        c ^= static_cast<std::uint16_t>(d[i] << 8);
        for (int k = 0; k < 8; ++k) c = (c & 0x8000) ? static_cast<std::uint16_t>((c << 1) ^ 0x1021) : static_cast<std::uint16_t>(c << 1);
    }
    d[12] = c & 255; d[13] = c >> 8;
}
}  // namespace

TEST_CASE("efx: the example files validate") {
    for (const char* n : {"love", "hello"}) {
        const auto d = load(n);
        INFO(n);
        CHECK(d.size() > 16);
        CHECK(cat_eyes_efx_check(d.data(), d.size()) == 0);
    }
}

TEST_CASE("efx: damaged files are rejected with a reason") {
    const auto good = load("love");
    auto d = good;
    CHECK(cat_eyes_efx_check(nullptr, 0) == 1);
    d[0] = 'X';  CHECK(cat_eyes_efx_check(d.data(), d.size()) == 1);
    d = good; d[4] = 2;  CHECK(cat_eyes_efx_check(d.data(), d.size()) == 2);
    d = good; d.pop_back();  CHECK(cat_eyes_efx_check(d.data(), d.size()) == 3);
    d = good; d.back() ^= 1;  CHECK(cat_eyes_efx_check(d.data(), d.size()) == 4);
    d = good; d[8] = 0; CHECK(cat_eyes_efx_check(d.data(), d.size()) == 5);      // zero shapes
    d = good; d[8] = 17; CHECK(cat_eyes_efx_check(d.data(), d.size()) == 5);     // too many shapes
    d = good; d[16 + 4 * 6] = 99; set_crc(d);                                    // first shape: unknown kind
    CHECK(cat_eyes_efx_check(d.data(), d.size()) == 7);
    d = good; d[16 + 4 * 6 + 5 + 10] = 0; d[16 + 4 * 6 + 5 + 11] = 0; set_crc(d);  // key 2 time == key 1 time
    CHECK(cat_eyes_efx_check(d.data(), d.size()) == 8);
    CHECK(std::string_view(cat_eyes_efx_error(4)) == "checksum mismatch");
    // no byte of the file may crash the checker, whatever it is changed to
    for (std::size_t i = 0; i < good.size(); ++i)
        for (int v : {0, 1, 0x7f, 0x80, 0xff}) {
            d = good; d[i] = static_cast<std::uint8_t>(v); set_crc(d);
            (void)cat_eyes_efx_check(d.data(), d.size());
        }
    for (std::size_t n = 0; n < good.size(); ++n) (void)cat_eyes_efx_check(good.data(), n);
}

TEST_CASE("efx: playing draws shapes in time, then stops by itself") {
    const auto d = load("love");
    cat_eyes_init(1);
    cat_eyes_set_auto(false);
    cat_eyes_tick(1000);
    CHECK(cat_eyes_efx_play("love", d.data(), d.size()));
    CHECK(std::string_view(cat_eyes_anim_name(cat_eyes_current_anim())) == "love");
    int max_fx = 0;
    bool left_before = false;
    for (std::uint32_t t = 1000; t < 1000 + 3400; t += 33) {
        cat_eyes_tick(t);
        if (t == 1000) left_before = g_ce.f[0].n_fx > 0;
        max_fx = std::max<int>(max_fx, g_ce.f[0].n_fx);
    }
    CHECK(max_fx >= 3);                       // heart (3 shapes) + more
    CHECK(cat_eyes_current_anim() == CAT_ANIM_NONE);
    cat_eyes_tick(1000 + 3500);
    CHECK(g_ce.f[0].n_fx == 0);
    (void)left_before;
}

TEST_CASE("efx: a rejected file leaves a running effect alone") {
    const auto d = load("love");
    cat_eyes_init(1);
    cat_eyes_tick(0);
    CHECK(cat_eyes_efx_play("love", d.data(), d.size()));
    auto bad = d; bad.back() ^= 1;
    CHECK_FALSE(cat_eyes_efx_play("bad", bad.data(), bad.size()));
    CHECK(std::string_view(cat_eyes_anim_name(cat_eyes_current_anim())) == "love");
}

TEST_CASE("efx: world shapes cross from the left eye to the right eye") {
    const auto d = load("hello");
    cat_eyes_init(1);
    cat_eyes_set_auto(false);
    cat_eyes_tick(0);
    CHECK(cat_eyes_efx_play("hello", d.data(), d.size()));
    int on_left_first = -1, on_right_first = -1;
    for (std::uint32_t t = 0; t < 3300; t += 33) {
        cat_eyes_tick(t);
        auto visible = [](const ce_frame_t& f) {
            for (int i = 0; i < f.n_fx; ++i) if (f.fx[i].bx1 >= 0 && f.fx[i].bx0 < CAT_EYES_W) return true;
            return false;
        };
        const bool l = visible(g_ce.f[0]), r = visible(g_ce.f[1]);
        if (l && on_left_first < 0) on_left_first = static_cast<int>(t);
        if (r && on_right_first < 0) on_right_first = static_cast<int>(t);
    }
    CHECK(on_left_first >= 0);
    CHECK(on_right_first > on_left_first);    // the ball reaches the right display later
}

TEST_CASE("efx: scene plays a custom effect by name and from the pool") {
    static std::vector<std::uint8_t> data;
    data = load("love");
    eyes::set_fx_loader([](std::string_view n) noexcept -> std::span<const std::uint8_t> {
        return n == "love" ? std::span<const std::uint8_t>(data) : std::span<const std::uint8_t>{};
    });
    engine::Scene<engine::NoClip> s(1);
    s.tick(0);
    engine::EyeCmd c; c.fx.assign("nope");
    s.apply(engine::Command{c}, 0);
    CHECK_FALSE(eyes::CatModel::animating());
    c.fx.assign("love");
    s.apply(engine::Command{c}, 0);
    CHECK(eyes::CatModel::animating());
    CHECK(eyes::CatModel::info().anim == "love");
    // a pool with only that item picks it
    engine::Pool p; p.anim_mask = 0; (void)p.clips.push_back(engine::StaticString<31>("love")); p.min_ds = p.max_ds = 10;
    s.set_pool(p);
    for (std::uint32_t t = 100; t < 8000 && eyes::CatModel::animating(); t += 33) s.tick(t);   // let it end
    CHECK_FALSE(eyes::CatModel::animating());
    bool again = false;
    for (std::uint32_t t = 8000; t < 30000 && !again; t += 33) { s.tick(t); again = eyes::CatModel::animating(); }
    CHECK(again);
    eyes::set_fx_loader(nullptr);
}
