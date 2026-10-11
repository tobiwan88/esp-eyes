#include "doctest.h"
#include "cat_eyes.h"
#include "engine/core/parse.hpp"

using namespace engine;

#define OK(x) CHECK(static_cast<bool>(x))
#define BAD(x, c) do { auto r_ = (x); CHECK(!r_); if (!r_) CHECK(r_.error().code == (c)); } while (0)

TEST_CASE("parse_eye basics") {
    auto r = parse_eye(R"({"style":"anime","iris":"green","play":"wink","auto":false,"look":[0.5,-1,800],"save":true})");
    OK(r); if (!r) return;
    CHECK(*r->style == CAT_STYLE_ANIME); CHECK(*r->iris == CAT_IRIS_GREEN); CHECK(*r->play == CAT_ANIM_WINK);
    CHECK(*r->auto_idle == false); CHECK((*r->look_q8)[0] == 128); CHECK((*r->look_q8)[1] == -256);
    CHECK(r->hold_ms == 800); CHECK(r->save);
}
TEST_CASE("parse_eye play targets and aliases") {
    auto a = parse_eye(R"({"play":"blink"})"); OK(a); if (a) CHECK(*a->play == kPlayBlink);
    auto b = parse_eye(R"({"play":"random"})"); OK(b); if (b) CHECK(*b->play == kPlayRandom);
    auto c = parse_eye(R"({"play":"heart_eyes"})"); OK(c); if (c) CHECK(*c->play == CAT_ANIM_SPECIAL);
    auto d = parse_eye(R"({"style":"line_art"})"); OK(d); if (d) CHECK(*d->style == CAT_STYLE_LINE_ART);
    auto e = parse_eye(R"({"look":[0,0]})"); OK(e); if (e) CHECK(e->hold_ms == 1000);
}
TEST_CASE("parse_eye rejects bad input") {
    BAD(parse_eye("not json"), Err::BadJson);
    BAD(parse_eye("[]"), Err::BadJson);
    BAD(parse_eye(R"({})"), Err::BadJson);
    BAD(parse_eye(R"({"foo":1})"), Err::BadJson);
    BAD(parse_eye(R"({"mood":"happy"})"), Err::BadJson);
    BAD(parse_eye(R"({"style":"neon"})"), Err::BadJson);
    BAD(parse_eye(R"({"iris":3})"), Err::BadJson);
    BAD(parse_eye(R"({"play":"Dance!"})"), Err::BadJson);
    auto fx = parse_eye(R"({"play":"dance"})");  // not built in: a custom effect, checked by the web layer
    OK(fx); if (fx) { CHECK(!fx->play); CHECK(fx->fx.view() == "dance"); }
    BAD(parse_eye(R"({"auto":"yes"})"), Err::BadJson);
    BAD(parse_eye(R"({"look":[1]})"), Err::BadJson);
    BAD(parse_eye(R"({"look":[2,0]})"), Err::BadJson);
    BAD(parse_eye(R"({"look":[0,0,70000]})"), Err::BadJson);
    BAD(parse_eye(std::string(2000, ' ')), Err::TooLarge);
}
TEST_CASE("parse_eye error message") {
    auto r = parse_eye(R"({"look":[1]})");
    CHECK(!r);
    if (!r) CHECK(r.error().message.view() == "look must be [gx, gy] or [gx, gy, hold_ms]");
}
TEST_CASE("parse_play") {
    auto a = parse_play(R"({"clip":"wink"})"); OK(a);
    if (a) { CHECK(a->clip[0].view() == "wink"); CHECK(a->clip[1].view() == "wink"); CHECK(a->then_eyes); CHECK(!a->loop); }
    auto b = parse_play(R"({"left":"a","loop":true,"then":"hold"})"); OK(b);
    if (b) { CHECK(b->clip[0].view() == "a"); CHECK(b->clip[1].empty()); CHECK(b->loop); CHECK(!b->then_eyes); }
    BAD(parse_play(R"({})"), Err::BadJson);
    BAD(parse_play(R"({"clip":"A B"})"), Err::BadName);
    BAD(parse_play(R"({"clip":"a","left":"b"})"), Err::BadJson);
    BAD(parse_play(R"({"clip":"a","then":"x"})"), Err::BadJson);
    BAD(parse_play(R"({"clip":3})"), Err::BadJson);
}
TEST_CASE("clip_name_valid") {
    CHECK(clip_name_valid("a-b_9")); CHECK(!clip_name_valid("")); CHECK(!clip_name_valid("A"));
    CHECK(!clip_name_valid("../x")); CHECK(!clip_name_valid(std::string(32, 'a'))); CHECK(clip_name_valid(std::string(31, 'a')));
}

#include "engine/core/errors.hpp"
TEST_CASE("errors and auth helpers") {
    CHECK(describe(Err::Conflict).http == 409);
    CHECK(describe(Err::BadJson).code == "E_BAD_JSON");
    static_assert(ct_equal("abc", "abc") && !ct_equal("abc", "abd") && !ct_equal("abc", "ab"));
    CHECK(bearer_token("Bearer xyz") == "xyz");
    CHECK(bearer_token("Basic xyz").empty());
    CHECK(bearer_token("Bearer ").empty());
}
