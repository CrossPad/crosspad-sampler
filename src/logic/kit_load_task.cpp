// SPDX-License-Identifier: MIT

#include <crosspad-sampler/logic/kit_load_task.hpp>
#include <crosspad-sampler/SamplerPlatformCallbacks.hpp>
#include <crosspad-sampler/SamplerConfig.hpp>

#include <crosspad-sampler/ui/sampler_panel.hpp>   // getKitOrchestrator()

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

// Why the load failed, for the status bar. Written before s_outcome is stored
// with release ordering and read after it is loaded with acquire, so that pair
// publishes it — no extra lock needed.
static char s_error[48] = {0};

static void set_error(const char* msg) {
    snprintf(s_error, sizeof(s_error), "%s",
             (msg && *msg) ? msg : "Kit load failed");
}

static void progress_poll_timer_cb(lv_timer_t* timer) {
    int outcome = s_outcome.load(std::memory_order_acquire);
    if (outcome == 0) {
        crosspad_gui::statusbar_set_loading(s_progress.load(std::memory_order_relaxed));
        return;
    }
    if (outcome > 0) {
        crosspad_gui::statusbar_process_loader_status(100, true, false, "Kit loaded");
    } else {
        crosspad_gui::statusbar_process_loader_status(100, true, true, s_error);
    }
    s_loading = false;
    lv_timer_delete(timer);

    // Last, and on this thread: the platform's follow-up may touch UI or
    // storage state the load task must not. Ordered after s_loading is cleared
    // so a handler is free to start another load.
    auto& cb = sampler_get_platform_callbacks();
    if (cb.audio.onKitLoadFinished) {
        cb.audio.onKitLoadFinished(outcome > 0);
    }
}

// ── Background task ─────────────────────────────────────────────────────────

struct LoadParams {
    bool onlyModified;
    int  kitId;      // < 0 = a kit is already current, skip the change
};

static void load_fn(void* param) {
    auto* p = static_cast<LoadParams*>(param);
    bool onlyModified = p->onlyModified;
    int  kitId        = p->kitId;
    delete p;

    // The kit change belongs here, not on the caller's thread: it parses the
    // kit JSON off storage, which is what made the UI thread miss the task
    // watchdog when the selector drove it directly.
    if (kitId >= 0) {
        auto result = getKitOrchestrator().changeKit(kitId);
        if (!result.success) {
            set_error(result.error.c_str());
            s_outcome.store(-1, std::memory_order_release);
            vTaskDelete(nullptr);
            return;
        }
    }

    auto* kitMgr = crosspad::getKitManager();
    if (!kitMgr || !kitMgr->getCurrentKit()) {
        set_error("No kit loaded");
        s_outcome.store(-1, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }

    auto* kit = kitMgr->getCurrentKit();
    auto& cb = sampler_get_platform_callbacks();

    // Let the audio/streaming background settle so it releases the storage
    // bus and its file handles before the reload reopens files.
    vTaskDelay(pdMS_TO_TICKS(150));

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

void kit_load_task_start(bool onlyModified, int kitId) {
    // Must be called from the LVGL thread (creates an lv_timer).
    if (s_loading) return;
    s_loading = true;
    s_progress.store(0, std::memory_order_relaxed);
    set_error(nullptr);
    s_outcome.store(0, std::memory_order_release);

    crosspad_gui::statusbar_set_loading(0);

    auto* p = new LoadParams{onlyModified, kitId};

    // Allocate task stack from PSRAM to avoid exhausting scarce internal SRAM.
    // xTaskCreatePinnedToCoreWithCaps is ESP-IDF v5 only — the Arduino build
    // (IDF v4.x) uses a static BSS stack instead: its internal heap's largest
    // free block (~7 kB at runtime) is smaller than LOAD_TASK_STACK, so a
    // dynamically allocated stack fails outright. Buffers are reusable across
    // loads — s_loading serializes them and load_fn self-deletes.
#if defined(ARDUINO)
    // 4 kB matches the stack the legacy Arduino loader task ran the same
    // LoadPad work on; internal RAM is too scarce for the PSRAM-sized figure.
    constexpr size_t kArduinoLoadStack = 4096;
    static StaticTask_t s_loadTcb;
    static StackType_t  s_loadStack[kArduinoLoadStack / sizeof(StackType_t)];
    TaskHandle_t th = xTaskCreateStaticPinnedToCore(load_fn, "kit_load", kArduinoLoadStack,
                                                    p, 1, s_loadStack, &s_loadTcb, 0);
    BaseType_t ret = (th != nullptr) ? pdPASS : pdFAIL;
#else
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(load_fn, "kit_load", LOAD_TASK_STACK,
                                                     p, 1, nullptr, 0, MALLOC_CAP_SPIRAM);
#endif
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
