#include "engine/core/parse.hpp"

#include <cmath>
#include <cstring>

#include "cJSON.h"
#include "cat_eyes.h"

namespace engine {
namespace {


struct Doc {
    cJSON* root;
    explicit Doc(std::string_view s) : root(cJSON_ParseWithLength(s.data(), s.size())) {}
    ~Doc() { cJSON_Delete(root); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

bool known(const cJSON* o, std::initializer_list<std::string_view> keys, std::string_view& bad) {
    for (const cJSON* c = o->child; c; c = c->next) {
        bool ok = false;
        for (auto k : keys) ok |= (c->string && k == c->string);
        if (!ok) { bad = c->string ? c->string : "?"; return false; }
    }
    return true;
}

// ---- typed field readers: return nullopt if absent, error message via `err` ----
struct Ctx {
    std::string_view err;  // non-empty on failure
    void bad(std::string_view m) { if (err.empty()) err = m; }
};

std::optional<bool> get_bool(Ctx& c, const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!v) return std::nullopt;
    if (!cJSON_IsBool(v)) { c.bad("expected boolean"); return std::nullopt; }
    return cJSON_IsTrue(v);
}
}  // namespace

std::expected<EyeCmd, ParseError> parse_eye(std::string_view json) noexcept {
    auto E = [](Err c, std::string_view m) { return std::unexpected(ParseError{c, StaticString<63>(m)}); };
    if (json.size() > kMaxBody) return E(Err::TooLarge, "body over 1 KB");
    Doc d(json);
    if (!d.root || !cJSON_IsObject(d.root)) return E(Err::BadJson, "invalid JSON object");
    std::string_view bad;
    if (!known(d.root, {"style", "iris", "play", "auto", "look", "save"}, bad)) return E(Err::BadJson, "unknown field");
    Ctx c;
    EyeCmd cmd;
    auto name_field = [&](const char* k, int (*lookup)(const char*), const char* what) -> std::optional<std::uint8_t> {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(d.root, k);
        if (!v) return std::nullopt;
        int id = cJSON_IsString(v) ? lookup(v->valuestring) : -1;
        if (id < 0) { c.bad(what); return std::nullopt; }
        return static_cast<std::uint8_t>(id);
    };
    auto style_lookup = [](const char* n) { return std::string_view(n) == "line_art" ? cat_eyes_style_from_name("line") : cat_eyes_style_from_name(n); };
    cmd.style = name_field("style", +style_lookup, "unknown style");
    cmd.iris = name_field("iris", cat_eyes_iris_from_name, "unknown iris");
    if (const cJSON* v = cJSON_GetObjectItemCaseSensitive(d.root, "play")) {
        const char* n = cJSON_IsString(v) ? v->valuestring : nullptr;
        if (n && std::string_view(n) == "blink") cmd.play = kPlayBlink;
        else if (n && std::string_view(n) == "random") cmd.play = kPlayRandom;
        else if (n && cat_eyes_anim_from_name(n) >= 0) cmd.play = static_cast<std::uint8_t>(cat_eyes_anim_from_name(n));
        else if (n && clip_name_valid(n)) cmd.fx.assign(n);  // a custom effect; the web layer checks that it exists
        else c.bad("unknown animation");
    }
    cmd.auto_idle = get_bool(c, d.root, "auto");
    if (auto v = get_bool(c, d.root, "save")) cmd.save = *v;
    if (const cJSON* l = cJSON_GetObjectItemCaseSensitive(d.root, "look")) {
        const int n = cJSON_IsArray(l) ? cJSON_GetArraySize(l) : 0;
        if (n < 2 || n > 3) c.bad("look must be [gx, gy] or [gx, gy, hold_ms]");
        else {
            std::array<std::int16_t, 2> q{};
            for (int i = 0; i < 2; ++i) {
                const cJSON* x = cJSON_GetArrayItem(l, i);
                if (!cJSON_IsNumber(x) || x->valuedouble < -1 || x->valuedouble > 1) { c.bad("look values must be -1..1"); break; }
                q[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(std::lround(x->valuedouble * 256));
            }
            if (n == 3) {
                const cJSON* h = cJSON_GetArrayItem(l, 2);
                if (!cJSON_IsNumber(h) || h->valuedouble < 0 || h->valuedouble > 60000) c.bad("hold_ms must be 0..60000");
                else cmd.hold_ms = static_cast<std::uint16_t>(h->valuedouble);
            }
            if (c.err.empty()) cmd.look_q8 = q;
        }
    }
    if (!c.err.empty()) return E(Err::BadJson, c.err);
    if (!cmd.style && !cmd.iris && !cmd.play && cmd.fx.empty() && !cmd.auto_idle && !cmd.look_q8) return E(Err::BadJson, "nothing to do");
    return cmd;
}

std::expected<PlayCmd, ParseError> parse_play(std::string_view json) noexcept {
    auto E = [](Err c, std::string_view m) { return std::unexpected(ParseError{c, StaticString<63>(m)}); };
    if (json.size() > kMaxBody) return E(Err::TooLarge, "body over 1 KB");
    Doc d(json);
    if (!d.root || !cJSON_IsObject(d.root)) return E(Err::BadJson, "invalid JSON object");
    std::string_view bad;
    if (!known(d.root, {"clip", "left", "right", "loop", "then"}, bad)) return E(Err::BadJson, "unknown field");
    PlayCmd p;
    Ctx c;
    auto name = [&](const char* k, std::initializer_list<std::size_t> targets) -> bool {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(d.root, k);
        if (!v) return false;
        if (!cJSON_IsString(v)) { c.bad("clip name must be a string"); return false; }
        if (!clip_name_valid(v->valuestring)) { c.bad("invalid clip name"); return false; }
        for (auto t : targets) p.clip[t].assign(v->valuestring);
        return true;
    };
    const bool both = name("clip", {0, 1});
    const bool l = name("left", {0}), r = name("right", {1});
    if (both && (l || r)) c.bad("use clip or left/right, not both");
    if (!both && !l && !r && c.err.empty()) c.bad("clip, left or right required");
    if (auto v = get_bool(c, d.root, "loop")) p.loop = *v;
    if (const cJSON* t = cJSON_GetObjectItemCaseSensitive(d.root, "then")) {
        if (cJSON_IsString(t) && std::string_view(t->valuestring) == "eyes") p.then_eyes = true;
        else if (cJSON_IsString(t) && std::string_view(t->valuestring) == "hold") p.then_eyes = false;
        else c.bad("then must be eyes or hold");
    }
    if (!c.err.empty()) {
        return E(c.err == "invalid clip name" ? Err::BadName : Err::BadJson, c.err);
    }
    return p;
}

}  // namespace engine
