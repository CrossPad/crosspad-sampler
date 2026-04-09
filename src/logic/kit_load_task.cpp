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

#include <cstring>
#include <cstdio>

namespace crosspad_sampler {

static volatile bool s_loading = false;

static constexpr size_t LOAD_TASK_STACK = 8096;

// ── lv_async_call helpers (thread-safe LVGL updates) ────────────────────────

static void async_set_progress(void* data) {
    int progress = reinterpret_cast<intptr_t>(data);
    crosspad_gui::statusbar_set_loading(progress);
}

static void async_done(void* /*data*/) {
    crosspad_gui::statusbar_process_loader_status(100, true, false, "Kit loaded");
}

static void async_error(void* /*data*/) {
    crosspad_gui::statusbar_process_loader_status(100, true, true, "Kit load failed");
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
        lv_async_call(async_error, nullptr);
        s_loading = false;
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

        int progress = ((padIdx + 1) * 100) / PAD_COUNT;
        lv_async_call(async_set_progress, reinterpret_cast<void*>(progress));
    }

    lv_async_call(async_done, nullptr);
    s_loading = false;
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
    if (s_loading) return;
    s_loading = true;

    crosspad_gui::statusbar_set_loading(0);

    auto* p = new LoadParams{onlyModified};

    // Allocate task stack from PSRAM to avoid exhausting scarce internal SRAM.
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(load_fn, "kit_load", LOAD_TASK_STACK,
                                                     p, 1, nullptr, 0, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        printf("kit_load_task: xTaskCreate failed\n");
        delete p;
        lv_async_call(async_error, nullptr);
        s_loading = false;
    }
}

bool kit_load_task_is_loading() {
    return s_loading;
}

} // namespace crosspad_sampler
