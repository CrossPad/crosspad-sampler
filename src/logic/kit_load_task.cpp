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
#include <freertos/semphr.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>

#include <atomic>
#include <cstring>
#include <cstdio>

namespace crosspad_sampler {

static volatile bool s_loading = false;

static constexpr size_t LOAD_TASK_STACK = 8096;

// Above the housekeeping that shares core 0, below the audio tasks.
// At priority 1 the loader sat level with the main CDC loop, which
// hex-dumps every command it receives and blocks on the console UART
// while doing it: two prio-1 tasks round-robining, one of them holding
// the CPU in printf. Under a heavy pad stream that stretched a ~3 s kit
// load past 20 s. The loader spends nearly all its time blocked on SD
// reads, so it cannot hog the CPU at this level.
static constexpr UBaseType_t LOAD_TASK_PRIORITY = 5;

// ── The worker is created once and never dies ───────────────────────────────
// It used to be spawned per load and ended with vTaskDelete(). That leaks:
// xTaskCreatePinnedToCoreWithCaps() builds a *static* task — the stack comes
// from heap_caps_malloc(MALLOC_CAP_SPIRAM) and the TCB from pvPortMalloc(),
// which is internal RAM — and FreeRTOS never frees a static task's buffers.
// Only vTaskDeleteWithCaps() does, and calling that on yourself spawns yet
// another temporary task to do the freeing. Measured cost of the old shape:
// 8196 B of PSRAM and 372 B of internal RAM gone per kit load, for good. Ten
// swaps took internal RAM from 18.7 kB to 5.7 kB free, at which point
// sdmmc_cmd could no longer allocate a DMA buffer, every SD read failed, and
// the load that was in flight never finished.
//
// A worker that parks on a semaphore between loads costs those buffers once.
// It is also what makes back-to-back swaps cheap: no allocation stands
// between the request and the work, so a swap cannot fail for want of memory.
static TaskHandle_t      s_worker      = nullptr;
static SemaphoreHandle_t s_workPending = nullptr;

// The request the worker picks up. Written before the semaphore is given and
// read after it is taken; a second load cannot start while one is running (it
// waits in the pending slot below instead), so this one needs no lock.
static bool s_reqOnlyModified = false;
static int  s_reqKitId        = -1;

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

// A kit change asked for while one is already running. It used to be dropped
// on the floor — kit_load_task_start() simply returned if s_loading was set —
// so picking a kit while the previous one was still loading did nothing at
// all: no sound change, no error, and a status bar that kept showing the load
// it was already doing. Two taps in the selector inside the ~2.5 s a load
// takes was enough, and driving swaps from a script hit it every time.
//
// The latest request wins rather than queueing them all: someone spinning
// through the selector wants the kit they stopped on, not to sit through every
// kit they passed over on the way there.
static bool s_pendingValid        = false;
static bool s_pendingOnlyModified = false;
static int  s_pendingKitId        = -1;

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

    // Then, and on this thread: the platform's follow-up may touch UI or
    // storage state the load task must not. Ordered after s_loading is cleared
    // so a handler is free to start another load.
    auto& cb = sampler_get_platform_callbacks();
    if (cb.audio.onKitLoadFinished) {
        cb.audio.onKitLoadFinished(outcome > 0);
    }

    // Last: whatever the user asked for while this one was busy. This runs on
    // the LVGL thread, which is where kit_load_task_start() must be called
    // from, and s_loading is already clear so the request is accepted.
    if (s_pendingValid) {
        s_pendingValid = false;
        kit_load_task_start(s_pendingOnlyModified, s_pendingKitId);
    }
}

// ── Background task ─────────────────────────────────────────────────────────

// One kit load, start to finish. Returns with s_outcome published; the LVGL
// timer picks it up from there.
static void run_one_load(bool onlyModified, int kitId) {
    // The kit change belongs here, not on the caller's thread: it parses the
    // kit JSON off storage, which is what made the UI thread miss the task
    // watchdog when the selector drove it directly.
    if (kitId >= 0) {
        auto result = getKitOrchestrator().changeKit(kitId);
        if (!result.success) {
            set_error(result.error.c_str());
            s_outcome.store(-1, std::memory_order_release);
            return;
        }
    }

    auto* kitMgr = crosspad::getKitManager();
    if (!kitMgr || !kitMgr->getCurrentKit()) {
        set_error("No kit loaded");
        s_outcome.store(-1, std::memory_order_release);
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
}

// Parks between loads instead of dying after one, so its stack and TCB are
// allocated exactly once for the life of the firmware.
static void load_fn(void* /*param*/) {
    for (;;) {
        if (xSemaphoreTake(s_workPending, portMAX_DELAY) != pdTRUE) continue;
        run_one_load(s_reqOnlyModified, s_reqKitId);
    }
}

// ── Public API ──────────────────────────────────────────────────────────────

void kit_load_task_init() {
    // No-op — lv_async_call requires no pre-initialization.
}

void kit_load_task_poll() {
    // No-op — lv_async_call is drained by LVGL's own task handler.
}

// Bring up the worker and its doorbell. Runs on the first load and never
// again — everything here is a once-per-boot cost.
static bool ensure_worker() {
    if (s_worker) return true;

    if (!s_workPending) {
        s_workPending = xSemaphoreCreateBinary();
        if (!s_workPending) return false;
    }

    // Allocate the task stack from PSRAM to avoid exhausting scarce internal
    // SRAM. xTaskCreatePinnedToCoreWithCaps is ESP-IDF v5 only — the Arduino
    // build (IDF v4.x) uses a static BSS stack instead: its internal heap's
    // largest free block (~7 kB at runtime) is smaller than LOAD_TASK_STACK,
    // so a dynamically allocated stack fails outright.
#if defined(ARDUINO)
    // 4 kB matches the stack the legacy Arduino loader task ran the same
    // LoadPad work on; internal RAM is too scarce for the PSRAM-sized figure.
    constexpr size_t kArduinoLoadStack = 4096;
    static StaticTask_t s_loadTcb;
    static StackType_t  s_loadStack[kArduinoLoadStack / sizeof(StackType_t)];
    s_worker = xTaskCreateStaticPinnedToCore(load_fn, "kit_load", kArduinoLoadStack,
                                             nullptr, LOAD_TASK_PRIORITY, s_loadStack, &s_loadTcb, 0);
    return s_worker != nullptr;
#else
    if (xTaskCreatePinnedToCoreWithCaps(load_fn, "kit_load", LOAD_TASK_STACK,
                                        nullptr, LOAD_TASK_PRIORITY, &s_worker, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_worker = nullptr;
        return false;
    }
    return true;
#endif
}

void kit_load_task_start(bool onlyModified, int kitId) {
    // Must be called from the LVGL thread (creates an lv_timer).
    if (s_loading) {
        // Don't drop it — hand it to the timer that is watching the running
        // load, which will start it as soon as that one reports.
        s_pendingOnlyModified = onlyModified;
        s_pendingKitId        = kitId;
        s_pendingValid        = true;
        return;
    }

    if (!ensure_worker()) {
        printf("kit_load_task: worker task creation failed\n");
        crosspad_gui::statusbar_process_loader_status(100, true, true, "Kit load failed");
        return;
    }

    s_loading = true;
    s_progress.store(0, std::memory_order_relaxed);
    set_error(nullptr);
    s_outcome.store(0, std::memory_order_release);

    crosspad_gui::statusbar_set_loading(0);

    s_reqOnlyModified = onlyModified;
    s_reqKitId        = kitId;

    lv_timer_create(progress_poll_timer_cb, 100, nullptr);

    // Last: the worker runs as soon as this is given, and it must not start
    // ahead of the request it is meant to pick up.
    xSemaphoreGive(s_workPending);
}

bool kit_load_task_is_loading() {
    return s_loading;
}

int kit_load_task_pending_kit() {
    return s_pendingValid ? s_pendingKitId : -1;
}

} // namespace crosspad_sampler
