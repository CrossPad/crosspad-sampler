// SPDX-License-Identifier: MIT

#include <crosspad-sampler/logic/kit_load_task.hpp>
#include <crosspad-sampler/SamplerPlatformCallbacks.hpp>
#include <crosspad-sampler/SamplerConfig.hpp>

#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include <crosspad-gui/components/status_bar.h>
#include <lvgl.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>

#include <atomic>
#include <cstring>
#include <cstdio>

namespace crosspad_sampler {

static volatile bool s_loading = false;

static constexpr size_t LOAD_TASK_STACK = 8096;

// ── Cross-thread progress hand-off ──────────────────────────────────────────
// The load task must NOT touch LVGL: lv_async_call() itself mutates the timer
// list and is not thread-safe without LV_USE_OS — concurrent calls from the
// load task corrupted the list and crashed the LVGL task on a garbage
// callback pointer (intermittent InstrFetchProhibited/LoadProhibited).
// The task only writes these atomics; an LVGL timer created on the LVGL
// thread in kit_load_task_start() polls them and drives the status bar.

static std::atomic<int> s_progress{0};        // 0..100
static std::atomic<int> s_outcome{0};         // 0 = running, 1 = done, -1 = error

static void progress_poll_timer_cb(lv_timer_t* timer) {
    int outcome = s_outcome.load(std::memory_order_acquire);
    if (outcome == 0) {
        crosspad_gui::statusbar_set_loading(s_progress.load(std::memory_order_relaxed));
        return;
    }
    if (outcome > 0) {
        crosspad_gui::statusbar_process_loader_status(100, true, false, "Kit loaded");
    } else {
        crosspad_gui::statusbar_process_loader_status(100, true, true, "Kit load failed");
    }
    s_loading = false;
    lv_timer_delete(timer);
}

// ── Background task ─────────────────────────────────────────────────────────

struct LoadParams {
    bool onlyModified;
};

static void load_fn(void* param) {
    auto* p = static_cast<LoadParams*>(param);
    bool onlyModified = p->onlyModified;
    delete p;

    auto* kitMgr = crosspad::getKitManager();
    if (!kitMgr || !kitMgr->getCurrentKit()) {
        s_outcome.store(-1, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }

    auto* kit = kitMgr->getCurrentKit();
    auto& cb = sampler_get_platform_callbacks();

    for (uint8_t padIdx = 0; padIdx < PAD_COUNT; ++padIdx) {
        if (onlyModified && !kit->pads[padIdx].IsModified()) continue;

        if (cb.audio.reloadPad) {
            cb.audio.reloadPad(padIdx);
        }

        s_progress.store(((padIdx + 1) * 100) / PAD_COUNT,
                         std::memory_order_relaxed);
    }

    s_outcome.store(1, std::memory_order_release);
    vTaskDelete(nullptr);
}

// ── Public API ──────────────────────────────────────────────────────────────

void kit_load_task_init() {
    // No-op — lv_async_call requires no pre-initialization.
}

void kit_load_task_poll() {
    // No-op — lv_async_call is drained by LVGL's own task handler.
}

void kit_load_task_start(bool onlyModified) {
    // Must be called from the LVGL thread (creates an lv_timer).
    if (s_loading) return;
    s_loading = true;
    s_progress.store(0, std::memory_order_relaxed);
    s_outcome.store(0, std::memory_order_release);

    crosspad_gui::statusbar_set_loading(0);

    auto* p = new LoadParams{onlyModified};

    // Allocate task stack from PSRAM to avoid exhausting scarce internal SRAM.
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(load_fn, "kit_load", LOAD_TASK_STACK,
                                                     p, 1, nullptr, 0, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        printf("kit_load_task: xTaskCreate failed\n");
        delete p;
        crosspad_gui::statusbar_process_loader_status(100, true, true, "Kit load failed");
        s_loading = false;
        return;
    }
    lv_timer_create(progress_poll_timer_cb, 100, nullptr);
}

bool kit_load_task_is_loading() {
    return s_loading;
}

} // namespace crosspad_sampler
