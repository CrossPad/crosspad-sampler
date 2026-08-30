// SPDX-License-Identifier: MIT
//
// Kit loading, off the LVGL thread. Four things about this file are
// load-bearing, and each replaced a defect:
//
//  * One long-lived worker, not a task per load. Tasks made with
//    xTaskCreatePinnedToCoreWithCaps() are *static*, and only
//    vTaskDeleteWithCaps() frees their buffers — so a task per load leaked
//    8196 B of PSRAM and 372 B of internal RAM every time. Ten swaps left too
//    little internal RAM for an SD DMA buffer, and reads simply stopped.
//  * The worker never touches LVGL. lv_async_call() mutates the timer list and
//    is not thread-safe without LV_USE_OS. The task writes atomics; an LVGL
//    timer polls them.
//  * A request made mid-load is held, not dropped, and only the newest one —
//    so a second tap does something, and spinning the selector lands on the
//    kit you stopped at.
//  * The outgoing kit is released on the LVGL thread between timer callbacks:
//    the panel reads getCurrentKit()->pads across a waveform draw that blocks
//    on the SD card.

#include <crosspad-sampler/logic/kit_load_task.hpp>
#include <crosspad-sampler/SamplerPlatformCallbacks.hpp>
#include <crosspad-sampler/SamplerConfig.hpp>

#include <crosspad-sampler/ui/sampler_panel.hpp>   // getKitOrchestrator()

#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include <crosspad-gui/components/status_bar.h>
#include <lvgl.h>

#include <crosspad-sampler/port/SamplerRtos.hpp>

#if defined(ARDUINO)
  #include <freertos/FreeRTOS.h>
  #include <freertos/task.h>
#endif

#include <atomic>
#include <cstring>
#include <cstdio>

namespace crosspad_sampler {

static volatile bool s_loading = false;

static constexpr size_t LOAD_TASK_STACK = 8096;

// Above core 0's housekeeping, below the audio tasks. At priority 1 the loader
// round-robined with the console-blocking CDC loop and a ~3 s load stretched
// past 20 s; it is blocked on SD reads anyway, so it cannot hog the CPU here.
static constexpr int LOAD_TASK_PRIORITY = 5;

// ── The worker, created once ────────────────────────────────────────────────
// A task per load leaks: xTaskCreatePinnedToCoreWithCaps() builds a *static*
// task and only vTaskDeleteWithCaps() frees its buffers, so vTaskDelete() left
// 8196 B of PSRAM and 372 B of internal RAM behind on every swap. SamplerWorker
// / SamplerSignal hide the ESP/Arduino/PC-specific calls behind one worker
// that parks on a signal between loads and pays that cost once.
static SamplerWorker  s_worker;
static SamplerSignal  s_workPending;
static bool           s_workerUp = false;

// The request the worker picks up. Written before the semaphore is given,
// read after it is taken; a second load waits in the pending slot below.
static bool s_reqOnlyModified = false;
static int  s_reqKitId        = -1;

// ── Cross-thread progress hand-off ──────────────────────────────────────────
static std::atomic<int> s_progress{0};        // 0..100
static std::atomic<int> s_outcome{0};         // 0 = running, 1 = done, -1 = error

// Why it failed, for the status bar. Published by s_outcome's release/acquire
// pair, so it needs no lock.
static char s_error[48] = {0};

static void set_error(const char* msg) {
    snprintf(s_error, sizeof(s_error), "%s",
             (msg && *msg) ? msg : "Kit load failed");
}

// A kit change asked for while one is running; newest wins.
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

    // On this thread, and after s_loading is cleared: the follow-up may touch
    // UI or storage, and may start another load.
    auto& cb = sampler_get_platform_callbacks();
    if (cb.audio.onKitLoadFinished) {
        cb.audio.onKitLoadFinished(outcome > 0);
    }

    // Release the outgoing kit — here, where no panel reader is in flight.
    if (crosspad::getKitManager()) {
        crosspad::getKitManager()->releaseRetiredKit();
    }

    // Last: whatever was asked for while this one was busy. This is the LVGL
    // thread and s_loading is clear, so the request is accepted.
    if (s_pendingValid) {
        s_pendingValid = false;
        kit_load_task_start(s_pendingOnlyModified, s_pendingKitId);
    }
}

// ── Background task ─────────────────────────────────────────────────────────

// One kit load, start to finish. Returns with s_outcome published; the LVGL
// timer picks it up from there.
static void run_one_load(bool onlyModified, int kitId) {
    // Here, not on the caller's thread: parsing the kit JSON off storage is
    // what made the UI thread miss the task watchdog.
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

    // Let the streaming background release the storage bus and its file
    // handles before the reload reopens them.
    sampler_delay_ms(150);

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

// Parks between loads; its stack and TCB are allocated once.
static void load_fn(void* /*param*/) {
    for (;;) {
        if (!s_workPending.take(kWaitForever)) continue;
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

// Worker and doorbell, on the first load only.
static bool ensure_worker() {
    if (s_workerUp) return true;
    if (!s_workPending.create()) return false;

#if defined(ARDUINO)
    // The Arduino build has no xTaskCreatePinnedToCoreWithCaps and cannot
    // allocate this stack dynamically either: its internal heap's largest
    // free block (~7 kB at runtime) is smaller than LOAD_TASK_STACK. A static
    // BSS stack is the only shape that works there, so this one case stays
    // out of SamplerRtos. 4 kB matches the legacy Arduino loader task's stack
    // for the same work.
    constexpr size_t kArduinoLoadStack = 4096;
    static StaticTask_t s_loadTcb;
    static StackType_t  s_loadStack[kArduinoLoadStack / sizeof(StackType_t)];
    s_workerUp = xTaskCreateStaticPinnedToCore(load_fn, "kit_load", kArduinoLoadStack,
                                               nullptr, LOAD_TASK_PRIORITY,
                                               s_loadStack, &s_loadTcb, 0) != nullptr;
#else
    s_workerUp = s_worker.start(load_fn, nullptr, "kit_load",
                                LOAD_TASK_STACK, LOAD_TASK_PRIORITY, /*core=*/0);
#endif
    return s_workerUp;
}

void kit_load_task_start(bool onlyModified, int kitId) {
    // Must be called from the LVGL thread (creates an lv_timer).
    if (s_loading) {
        // Hand it to the timer watching the running load, to start once that
        // one reports.
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
    s_workPending.give();
}

bool kit_load_task_is_loading() {
    return s_loading;
}

int kit_load_task_pending_kit() {
    return s_pendingValid ? s_pendingKitId : -1;
}

} // namespace crosspad_sampler
