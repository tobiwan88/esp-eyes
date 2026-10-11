#include "engine/idf/render_task.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "clip/idf/player.hpp"
#include "config.hpp"
#include "display/idf/display.hpp"
#include "engine/core/scene.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "settings/idf/settings.hpp"
#include "storage/idf/storage.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace engine {
namespace {

const char* const kTag = "render";
QueueHandle_t g_queue = nullptr;
Scene<clip::ClipPlayer>* g_scene = nullptr;
portMUX_TYPE g_stats_mux = portMUX_INITIALIZER_UNLOCKED;
Stats g_stats{};
portMUX_TYPE g_pool_mux = portMUX_INITIALIZER_UNLOCKED;
Pool g_pool;
std::uint32_t g_pool_version = 1;
UBaseType_t g_stack_hwm = 0;
std::uint64_t g_fill_us = 0;  // render task only

portMUX_TYPE g_info_mux = portMUX_INITIALIZER_UNLOCKED;
eyes::CatInfo g_info{};
void publish_eye_info(const eyes::CatInfo& i) {
    taskENTER_CRITICAL(&g_info_mux);
    g_info = i;
    taskEXIT_CRITICAL(&g_info_mux);
}

struct LcdReq { bool pending = false; LcdOp op = LcdOp::Swap; int arg = 0; };
portMUX_TYPE g_lcd_mux = portMUX_INITIALIZER_UNLOCKED;
LcdReq g_lcd_req{};
std::uint32_t g_test_until = 0;  // render task only; 0 = no test pattern

// Custom effects: two buffers, read the file into the idle one and hand it to cat_eyes only if it loaded, so the
// effect that is running (it points into the other buffer) is never overwritten.
std::span<const std::uint8_t> load_fx(std::string_view name) noexcept {
    static std::uint8_t* bufs[2] = {nullptr, nullptr};
    static int cur = 1;
    const int next = 1 - cur;
    if (!bufs[next]) {
        bufs[next] = static_cast<std::uint8_t*>(heap_caps_malloc(storage::kMaxEfxBytes, MALLOC_CAP_SPIRAM));
        if (!bufs[next]) bufs[next] = static_cast<std::uint8_t*>(heap_caps_malloc(storage::kMaxEfxBytes, MALLOC_CAP_DEFAULT));
        if (!bufs[next]) return {};
    }
    auto n = storage::efx_read(name, {bufs[next], storage::kMaxEfxBytes});
    if (!n) return {};
    cur = next;
    return {bufs[next], *n};
}

std::uint32_t now_ms() { return static_cast<std::uint32_t>(esp_timer_get_time() / 1000); }

// Renders one display's plan; returns the first error.
std::expected<void, Err> draw(Scene<clip::ClipPlayer>& scene, Eye eye, const FramePlan& plan) {
    auto rects = plan.dirty;
    if (plan.full_redraw) { rects.clear(); (void)rects.push_back(gfx::Rect{0, 0, gfx::kScreen, gfx::kScreen}); }
    for (const gfx::Rect& r : rects) {
        for (const gfx::Rect s : gfx::Strips(r, static_cast<int>(cfg::kStripLines), plan.trim)) {
            auto buf = display::acquire(100);
            if (!buf) return std::unexpected(buf.error());
            const std::int64_t f0 = esp_timer_get_time();
            auto f = scene.fill(eye, s, buf->px.first(static_cast<std::size_t>(s.area())));
            g_fill_us += static_cast<std::uint64_t>(esp_timer_get_time() - f0);
            if (!f) {
                // buffer was acquired but never pushed: push black so the pool stays consistent
                std::fill(buf->px.begin(), buf->px.begin() + s.area(), gfx::Rgb565be{0});
            }
            if (auto p = display::push(eye, std::move(*buf), s); !p) return p;
        }
    }
    return {};
}

// Runs a pending display diagnostic; returns true while the test pattern owns the panels.
bool service_lcd(Scene<clip::ClipPlayer>& scene, std::uint32_t now) {
    taskENTER_CRITICAL(&g_lcd_mux);
    const LcdReq req = g_lcd_req;
    g_lcd_req.pending = false;
    taskEXIT_CRITICAL(&g_lcd_mux);
    if (req.pending) {
        if (req.op == LcdOp::Swap) {
            (void)display::wait_idle(100);
            display::set_swap(req.arg != 0);
        } else if (req.op == LcdOp::Init) {
            if (!display::reinit(req.arg)) ESP_LOGE(kTag, "lcd re-init failed");
        } else {
            g_test_until = req.arg > 0 ? now + static_cast<std::uint32_t>(req.arg) * 1000 : 0;
        }
        ESP_LOGI(kTag, "lcd: swap=%d spi=%d Hz test=%s", display::swapped(), display::spi_hz(), g_test_until ? "on" : "off");
        scene.invalidate();
    }
    if (g_test_until == 0) return false;
    if (static_cast<std::int32_t>(now - g_test_until) >= 0) {
        g_test_until = 0;
        scene.invalidate();
        return false;
    }
    for (std::size_t i = 0; i < kEyes; ++i) (void)display::pattern(static_cast<Eye>(i));
    return true;
}

Pool load_pool() {  // saved pool (NVS) on top of the default pool
    Pool p;
    if (auto j = settings::get("pool")) {
        if (auto patch = parse_pool(*j)) apply(p, *patch);
    }
    return p;
}

void task(void*) {
    esp_task_wdt_add(nullptr);
    eyes::set_fx_loader(&load_fx);
    auto num = [](const char* k, int d) { auto v = settings::get(k); return v ? std::atoi(v->c_str()) : d; };
    Scene<clip::ClipPlayer> scene(esp_timer_get_time() & 0xFFFFFFFF, static_cast<std::uint8_t>(num("style", 0)),
                                  static_cast<std::uint8_t>(num("iris", 0)), num("auto", 1) != 0);
    g_scene = &scene;
    g_pool = load_pool();
    std::uint32_t pool_seen = 0;
    scene.clip(Eye::Left).set_slot(0);
    scene.clip(Eye::Right).set_slot(1);
    std::uint32_t frame_n = 0;
    std::uint64_t sum_us = 0;
    std::uint32_t max_us = 0, window_start = now_ms();
    int stalls = 0;
    TickType_t last_wake = xTaskGetTickCount();
    std::uint32_t delay_ms = 33;

    for (;;) {
        const std::int64_t t0 = esp_timer_get_time();
        const std::uint32_t now = now_ms();
        Command cmd;
        while (xQueueReceive(g_queue, &cmd, 0) == pdTRUE) scene.apply(cmd, now);
        if (pool_seen != g_pool_version) {  // the HTTP side changed the pool
            Pool copy;
            taskENTER_CRITICAL(&g_pool_mux);
            copy = g_pool;
            pool_seen = g_pool_version;
            taskEXIT_CRITICAL(&g_pool_mux);
            scene.set_pool(copy);
        }
        scene.tick(now);
        if (scene.save_pending()) {  // "save": persist style, iris and random-idle
            const auto i = scene.eye_info();
            settings::set("style", std::to_string(i.style));
            settings::set("iris", std::to_string(i.iris));
            settings::set("auto", i.auto_idle ? "1" : "0");
            scene.saved();
        }

        std::uint32_t next_delay = 100;
        bool failed = false;
        const bool testing = service_lcd(scene, now);
        if (testing) next_delay = 33;
        for (std::size_t i = 0; i < kEyes && !testing; ++i) {
            const Eye eye = static_cast<Eye>(i);
            const FramePlan plan = scene.next(eye, now);
            next_delay = std::min(next_delay, plan.delay_ms);
            if (auto r = draw(scene, eye, plan); !r) { failed = true; break; }
            if (auto w = display::wait_idle(100); !w) { failed = true; break; }  // keep FIFO across displays
        }
        stalls = failed ? stalls + 1 : 0;
        if (stalls >= 3) {
            ESP_LOGE(kTag, "display stalled, rebooting");
            esp_restart();
        }
        esp_task_wdt_reset();

        const auto us = static_cast<std::uint32_t>(esp_timer_get_time() - t0);
        sum_us += us; max_us = std::max(max_us, us); ++frame_n;
        const std::uint32_t nowm = now_ms();
        if (nowm - window_start >= 1000) {
            taskENTER_CRITICAL(&g_stats_mux);
            g_stats.fps_x10 = static_cast<std::uint16_t>(frame_n * 10000 / (nowm - window_start));
            g_stats.frame_us_avg = static_cast<std::uint32_t>(sum_us / frame_n);
            g_stats.frame_us_max = max_us;
            g_stats.fill_us_avg = static_cast<std::uint32_t>(g_fill_us / frame_n);
            g_fill_us = 0;
            taskEXIT_CRITICAL(&g_stats_mux);
            frame_n = 0; sum_us = 0; max_us = 0; window_start = nowm;
            g_stack_hwm = uxTaskGetStackHighWaterMark(nullptr);
        }
        g_stats.source = {scene.current(Eye::Left), scene.current(Eye::Right)};
        publish_eye_info(scene.eye_info());
        delay_ms = next_delay;
        const TickType_t before = last_wake;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(delay_ms));
        (void)before;
        if (xTaskGetTickCount() - last_wake > pdMS_TO_TICKS(200)) last_wake = xTaskGetTickCount();  // overrun: reset reference
    }
}

}  // namespace

std::expected<void, Err> start_render_task() noexcept {
    g_queue = xQueueCreate(8, sizeof(Command));
    if (!g_queue) return std::unexpected(Err::Io);
    if (xTaskCreate(task, "render", 4096, nullptr, 8, nullptr) != pdPASS) return std::unexpected(Err::Io);
    return {};
}

std::expected<void, Err> post(const Command& c, std::uint32_t timeout_ms) noexcept {
    if (!g_queue) return std::unexpected(Err::Busy);
    return xQueueSend(g_queue, &c, pdMS_TO_TICKS(timeout_ms)) == pdTRUE ? std::expected<void, Err>{} : std::unexpected(Err::Busy);
}

bool lcd_request(LcdOp op, int arg) noexcept {
    taskENTER_CRITICAL(&g_lcd_mux);
    const bool busy = g_lcd_req.pending;
    if (!busy) g_lcd_req = LcdReq{true, op, arg};
    taskEXIT_CRITICAL(&g_lcd_mux);
    return !busy;
}

Pool pool_get() noexcept {
    taskENTER_CRITICAL(&g_pool_mux);
    Pool p = g_pool;
    taskEXIT_CRITICAL(&g_pool_mux);
    return p;
}

void pool_set(const Pool& p) noexcept {
    taskENTER_CRITICAL(&g_pool_mux);
    g_pool = p;
    ++g_pool_version;
    taskEXIT_CRITICAL(&g_pool_mux);
}

eyes::CatInfo eye_info() noexcept {
    taskENTER_CRITICAL(&g_info_mux);
    eyes::CatInfo i = g_info;
    taskEXIT_CRITICAL(&g_info_mux);
    return i;
}

unsigned render_stack_free_bytes() noexcept { return static_cast<unsigned>(g_stack_hwm) * sizeof(StackType_t); }

Stats stats() noexcept {
    taskENTER_CRITICAL(&g_stats_mux);
    Stats s = g_stats;
    taskEXIT_CRITICAL(&g_stats_mux);
    return s;
}

}  // namespace engine
