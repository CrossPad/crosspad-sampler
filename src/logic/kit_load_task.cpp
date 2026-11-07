// SPDX-License-Identifier: GPL-3.0-or-later
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

#include <crosspad-sampler/ui/sampler_panel.hpp>   // getKitOrchestrator(), getPadColorManager()
#include <crosspad-sampler/logic/pad_color_manager.hpp>
#include <crosspad-sampler/waveform/waveform_cache.hpp>

#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/sampletools/ISampleTools.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#include <crosspad/platform/IClock.hpp>
#include <crosspad/pad/PadManager.hpp>
#include <crosspad/event/IEventBus.hpp>

#include <crosspad-gui/components/kit_loader.h>
#include <crosspad-gui/components/status_bar.h>
#include <lvgl.h>

#include <crosspad-sampler/port/SamplerRtos.hpp>

#if defined(ARDUINO)
  #include <freertos/FreeRTOS.h>
  #include <freertos/task.h>
#endif

#include <crosspad/CrosspadCoreVersion.hpp>
#if CROSSPAD_CORE_VERSION_MAJOR >= 2
  #include <crosspad/platform/IFileSystem.hpp>
#else
  #include <sys/stat.h>
#endif

#include <atomic>
#include <cstring>
#include <cstdio>

namespace crosspad_sampler {

namespace {
// __builtin_popcount()/__builtin_ctz() are GCC/Clang; MSVC (crosspad-pc) has neither.
int pad_count(uint16_t mask) {
    int n = 0;
    for (; mask; mask = static_cast<uint16_t>(mask & (mask - 1))) ++n;
    return n;
}

int lowest_pad(uint16_t mask) {
    if (!mask) return 0;
    int i = 0;
    for (; !(mask & 1u); mask = static_cast<uint16_t>(mask >> 1)) ++i;
    return i;
}
}  // namespace

static volatile bool s_loading = false;

static constexpr size_t LOAD_TASK_STACK = 8096;

// A load for the prebuilt streaming engine is blocked on SD reads and only its
// wake latency matters; at priority 1 it round-robined with the
// console-blocking CDC loop and a ~3 s load stretched past 20 s. A pitched
// load is CPU: ~2 s of FFT and resampling, which on core 0 at priority 5
// starved LVGL (4) and the CDC drain loop (1) for the whole build — a frozen
// screen and no reply to any verb. So on
// ESP-IDF the worker lives on core 1, under TinyUSB (5) and the RT audio
// task (19), which take what they need and leave the rest to the build.
// The Arduino build keeps it on core 0, where its LVGL does not run.
#if defined(ARDUINO)
static constexpr int LOAD_TASK_PRIORITY = 5;
static constexpr int LOAD_TASK_CORE     = 0;
#else
static constexpr int LOAD_TASK_PRIORITY = 4;
static constexpr int LOAD_TASK_CORE     = 1;
#endif

// ── The worker, created once ────────────────────────────────────────────────
#if defined(ARDUINO)
static TaskHandle_t  s_arduinoWorker = nullptr;
#else
static SamplerTask   s_worker;
#endif
static SamplerSignal s_workPending;

// The request the worker picks up. Written before the semaphore is given,
// read after it is taken; a second load waits in the pending slot below.
static bool s_reqOnlyModified = false;
static int  s_reqKitId        = -1;

// ── Cross-thread progress hand-off ──────────────────────────────────────────
static std::atomic<int> s_progress{0};        // 0..100
static std::atomic<int> s_outcome{0};         // 0 = running, 1 = done, -1 = error
static std::atomic<int> s_padsDone{0};        // for "KIT LOADED done/total"
static std::atomic<int> s_padsTotal{0};

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

// ── Swap log ────────────────────────────────────────────────────────────────
// Written by the worker while a load runs, read over CDC afterwards. The
// slot being written is s_swapCount % kSwapLog; a reader that arrives mid-load
// sees a row with doneMs still 0, which is the honest state.
static constexpr int kSwapLog = 8;
static KitSwapRecord* s_swaps = nullptr;   // kSwapLog records, PSRAM, on the first load
static uint32_t s_swapCount = 0;
static int64_t  s_reqUs     = 0;    // stamped at kit_load_task_start()

static uint32_t since_req_ms() {
    return static_cast<uint32_t>((crosspad::getClock().getTimeUs() - s_reqUs) / 1000);
}

static void engine_stats(EngineLoadStats& out) {
    auto& cb = sampler_get_platform_callbacks();
    if (cb.audio.engineLoadStats) cb.audio.engineLoadStats(&out);
}

// ── Reloading pads under a performance ──────────────────────────────────────
// A pad whose voice is still sounding keeps sounding: its reload waits for
// the voice to end, and the pads that are idle go first. Only when nothing
// but sounding pads is left is one forced — the last one, or all of them
// once the deadline passes (a looped pad never ends on its own).
static bool s_engineWasPitched = false;   // what the previous load left running

static constexpr uint32_t kDeferPollMs      = 5;
static constexpr uint32_t kDeferDeadlineMs  = 4000;   // after the parse
static constexpr uint32_t kLastPadGraceMs   = 1000;   // the last sounding pad may finish on its own
static constexpr uint32_t kForceQuietWaitMs = 200;    // NoteOff → voice gone
static constexpr uint32_t kRenderPollMs    = 20;     // a pad's fx copy being rendered
static constexpr uint32_t kRenderWaitMs    = 60000;  // longest a load waits for one

static bool pad_sounding(uint8_t pad) {
    auto& cb = sampler_get_platform_callbacks();
    if (cb.audio.padSounding) return cb.audio.padSounding(pad);
    return crosspad::getPadManager().isPadPlaying(pad);
}

/// What the engine holds for a pad just reloaded. A pitched kit's pads all
/// play its one instrument, whatever their sample lists say.
static PadLoad pad_load(const crosspad::KitInfo& kit, uint8_t pad) {
    const bool named = kit.engine == crosspad::KitEngine::Pitched || kit.pads[pad].sampleCount > 0;
    if (!named) return PadLoad::Empty;
    auto& cb = sampler_get_platform_callbacks();
    if (cb.audio.padPlayable && !cb.audio.padPlayable(pad)) return PadLoad::Failed;
    return PadLoad::Ready;
}

/// False when the platform found a voice on the pad at the last moment and
/// left it alone; the caller keeps the pad on its list.
static bool reload_pad(const crosspad::KitInfo& kit, uint8_t pad, KitSwapRecord& rec, bool onlyIfIdle) {
    auto& cb = sampler_get_platform_callbacks();
    if (onlyIfIdle && cb.audio.reloadPadIfIdle) {
        if (!cb.audio.reloadPadIfIdle(pad)) return false;
    } else if (cb.audio.reloadPad) {
        cb.audio.reloadPad(pad);
    }
    rec.padDoneMs[pad] = static_cast<int32_t>(since_req_ms());
    // The pad now plays the incoming kit -- or nothing -- and says which in
    // its color.
    const PadLoad state = pad_load(kit, pad);
    const uint16_t bit = static_cast<uint16_t>(1u << pad);
    if (state == PadLoad::Empty)  rec.empty  |= bit;
    if (state == PadLoad::Failed) rec.failed |= bit;
    getPadColorManager().markPadReloaded(pad, state);
    return true;
}

/// A pad with pitch, filters or reverse plays its sample rendered with them --
/// the player can do none of it; see crosspad::renderedSamplePath(). A copy the card
/// does not have yet (a first load, a kit edited on a computer) is rendered
/// here, off the UI thread, through the sample tools, which run one job at a
/// time.
static void render_fx(const crosspad::KitInfo& kit, const crosspad::Pad& pad) {
    auto* st = crosspad::getPlatformServices().sampleTools;
    const crosspad::SampleFx fx{pad.pitch, pad.hp, pad.lp, pad.reverse};
    if (!st || !fx.any()) return;
    for (uint8_t i = 0; i < pad.sampleCount; ++i) {
        const std::string src = crosspad::resolveKitSamplePath(kit.path, pad.samples[i].sample);
        const std::string out = crosspad::kitFsPath(crosspad::renderedSamplePath(src, fx));
#if CROSSPAD_CORE_VERSION_MAJOR >= 2
        if (crosspad::getFileSystem().exists(out.c_str())) continue;
#else
        struct stat sb;
        if (stat(out.c_str(), &sb) == 0) continue;
#endif
        uint32_t waited = 0;
        while (!st->requestRendered(crosspad::kitFsPath(src), out, fx)) {   // another job runs
            if (waited >= kRenderWaitMs) return;
            sampler_delay_ms(kRenderPollMs);
            waited += kRenderPollMs;
        }
        while (st->job() == crosspad::ToolJob::Busy && waited < kRenderWaitMs) {
            sampler_delay_ms(kRenderPollMs);
            waited += kRenderPollMs;
        }
    }
}

// Stop the pad's voices and give them a moment to go: the reload after this
// then finds an idle pad like any other. A voice that is still there after
// the wait is reloaded regardless — the platform gates the renderer for that.
static void force_quiet(uint8_t pad) {
    crosspad::getEventBus().postSamplePlayback(pad, /*wavIdx=*/0, /*isOn=*/false,
                                               crosspad::EventSource::App);
    for (uint32_t waited = 0; waited < kForceQuietWaitMs && pad_sounding(pad);
         waited += kDeferPollMs) {
        sampler_delay_ms(kDeferPollMs);
    }
}

static void progress_poll_timer_cb(lv_timer_t* timer) {
    int outcome = s_outcome.load(std::memory_order_acquire);
    if (outcome == 0) {
        const int progress = s_progress.load(std::memory_order_relaxed);
        crosspad_gui::statusbar_set_loading(progress);
        crosspad_gui::kit_loader_set(static_cast<uint8_t>(progress));
        // Every LED write of the swap happens here, on the LVGL thread; the
        // loader only clears bits.
        getPadColorManager().tickKitSwap();
        return;
    }
    if (outcome > 0) {
        crosspad_gui::statusbar_process_loader_status(100, true, false, "Kit loaded");
        crosspad_gui::kit_loader_done(static_cast<uint8_t>(s_padsDone.load(std::memory_order_relaxed)),
                                      static_cast<uint8_t>(s_padsTotal.load(std::memory_order_relaxed)));
    } else {
        crosspad_gui::statusbar_process_loader_status(100, true, true, s_error);
    }
    getPadColorManager().endKitSwap();
    // The swap ends on the kit's colours; under 16 levels the ramp belongs on
    // top, recomputed from the new kit's source-pad colour.
    if (getSamplerPadLogic().sixteenLevels()) getSamplerPadLogic().refreshColors();

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
static KitSwapRecord s_noRecord;   // when the ring could not be allocated

static void run_one_load(bool onlyModified, int kitId) {
    if (!s_swaps) {
        s_swaps = static_cast<KitSwapRecord*>(sampler_alloc_big(kSwapLog * sizeof(KitSwapRecord)));
        if (s_swaps) memset(s_swaps, 0, kSwapLog * sizeof(KitSwapRecord));
    }
    KitSwapRecord& rec = s_swaps ? s_swaps[s_swapCount % kSwapLog] : s_noRecord;
    rec = KitSwapRecord{};
    rec.seq   = ++s_swapCount;
    rec.kitId = kitId;
    rec.reqMs = static_cast<uint32_t>(s_reqUs / 1000);
    for (auto& t : rec.padDoneMs) t = -1;

    // Here, not on the caller's thread: parsing the kit JSON off storage is
    // what made the UI thread miss the task watchdog.
    if (kitId >= 0) {
        auto result = getKitOrchestrator().changeKit(kitId);
        if (!result.success) {
            set_error(result.error.c_str());
            rec.why = KitLoadWhy::Parse;
            rec.doneMs = since_req_ms();
            s_outcome.store(-1, std::memory_order_release);
            return;
        }
    }
    rec.parseMs = since_req_ms();

    auto* kitMgr = crosspad::getKitManager();
    if (!kitMgr || !kitMgr->getCurrentKit()) {
        set_error("No kit loaded");
        rec.why = KitLoadWhy::Parse;
        rec.doneMs = since_req_ms();
        s_outcome.store(-1, std::memory_order_release);
        return;
    }

    auto* kit = kitMgr->getCurrentKit();

    // Let the streaming background release the storage bus and its file
    // handles before the reload reopens them -- when there is a stream: with
    // no pad sounding the streamer reads nothing, and since every task has
    // its own file slot (platform-idf 671dcc5) nothing of the loader's is
    // held by it. 150 ms of every quiet swap otherwise.
    bool anySounding = false;
    for (uint8_t p = 0; p < PAD_COUNT && !anySounding; ++p) anySounding = pad_sounding(p);
    if (anySounding) sampler_delay_ms(150);

    EngineLoadStats before;
    engine_stats(before);

    // A pitched kit is not reloaded pad by pad — its bank replaces the whole
    // engine — and what it replaces cannot keep sounding. Same for the way
    // back. Only a sampler kit following a sampler kit is swapped underneath
    // the performance.
    const bool seamless = kit->engine == crosspad::KitEngine::Sampler && !s_engineWasPitched;
    s_engineWasPitched = kit->engine == crosspad::KitEngine::Pitched;

    uint16_t todo = 0;
    for (uint8_t padIdx = 0; padIdx < PAD_COUNT; ++padIdx) {
        if (onlyModified && !kit->pads[padIdx].IsModified()) continue;
        todo |= static_cast<uint16_t>(1u << padIdx);
    }
    const uint16_t reloaded = todo;
    const int total = pad_count(todo);
    int done = 0;

    /* Copies of pads with pitch or filters first, while the outgoing kit still
     * plays: a pad swapped before its copy exists would come back plain. */
    if (kit->playsOnSampler()) {
        for (uint8_t padIdx = 0; padIdx < PAD_COUNT; ++padIdx) {
            if (todo & (1u << padIdx)) render_fx(*kit, kit->pads[padIdx]);
        }
    }

    // Until each of those pads is reloaded it still plays the outgoing kit;
    // dimmed and breathing is what that looks like.
    getPadColorManager().beginKitSwap(*kit, todo);

    uint32_t waitedMs = 0;
    uint32_t lastAloneMs = 0;

    while (todo) {
        bool progressed = false;
        for (uint8_t padIdx = 0; padIdx < PAD_COUNT; ++padIdx) {
            const uint16_t bit = static_cast<uint16_t>(1u << padIdx);
            if (!(todo & bit)) continue;
            if (seamless && pad_sounding(padIdx)) {
                rec.deferred |= bit;
                continue;
            }
            if (!reload_pad(*kit, padIdx, rec, seamless)) {
                // A hit landed while the files were being prepared: the old
                // sample plays out, and this pad is looked at again.
                rec.deferred |= bit;
                continue;
            }
            todo &= static_cast<uint16_t>(~bit);
            progressed = true;
            s_progress.store((++done * 100) / (total ? total : 1), std::memory_order_relaxed);
        }
        if (!todo || progressed) continue;

        // Only sounding pads are left. The last one gets a moment to end on
        // its own — most samples are shorter than that — but not to hold the
        // whole kit hostage; nor does anything past the deadline.
        if (pad_count(todo) == 1) lastAloneMs += kDeferPollMs;
        if (lastAloneMs >= kLastPadGraceMs || waitedMs >= kDeferDeadlineMs) {
            const uint8_t padIdx = static_cast<uint8_t>(lowest_pad(todo));
            const uint16_t bit = static_cast<uint16_t>(1u << padIdx);
            force_quiet(padIdx);
            rec.forced |= bit;
            reload_pad(*kit, padIdx, rec, false);
            todo &= static_cast<uint16_t>(~bit);
            s_progress.store((++done * 100) / (total ? total : 1), std::memory_order_relaxed);
            continue;
        }
        sampler_delay_ms(kDeferPollMs);
        waitedMs += kDeferPollMs;
    }

    EngineLoadStats after;
    engine_stats(after);
    rec.engine.gatedBlocks = after.gatedBlocks - before.gatedBlocks;
    rec.engine.gateMaxUs   = after.gateMaxUs;
    rec.engine.gateTotalUs = after.gateTotalUs - before.gateTotalUs;
    rec.engine.gateCount   = after.gateCount - before.gateCount;
    /* An engine that took the kit whole counts from zero again. */
    rec.engine.underruns   = after.underruns >= before.underruns ? after.underruns - before.underruns
                                                                 : after.underruns;
    rec.engine.noFreeBlock = after.noFreeBlock - before.noFreeBlock;
    rec.engine.droppedHits = after.droppedHits - before.droppedHits;
    rec.engine.rtGapMaxUs  = after.rtGapMaxUs;
    rec.engine.lateHits    = after.lateHits - before.lateHits;
    rec.engine.streamFopens = after.streamFopens - before.streamFopens;
    rec.engine.bgPassMaxUs = after.bgPassMaxUs;
    rec.engine.fopenMaxUs  = after.fopenMaxUs;
    rec.engine.streamKb    = after.streamKb - before.streamKb;
    rec.engine.loaderKb    = after.loaderKb - before.loaderKb;
    rec.doneMs = since_req_ms();

    // The waveforms' file too, here rather than on the LVGL thread where the
    // load's follow-up reads it (platform onKitLoadFinished).
    if (!onlyModified) waveform_cache_prefetch(kit->path.c_str());

    // What the pads hold, not how many were visited: "KIT LOADED n/m" counts
    // the pads that sound out of those the kit gives a sample. A kit that gives
    // samples of which none sounds is not loaded -- it is current, so the pad
    // editor can fill it, but saying "loaded" lit sixteen dead pads as ready.
    // A kit that gives none (+ NEW KIT, + NEW DRUMS) is loaded: empty, made
    // to be filled, not broken -- and the open song takes it.
    const uint16_t named = static_cast<uint16_t>(reloaded & ~rec.empty);
    const uint16_t ready = static_cast<uint16_t>(named & ~rec.failed);
    s_padsDone.store(pad_count(ready), std::memory_order_relaxed);
    s_padsTotal.store(pad_count(named), std::memory_order_relaxed);
    rec.ok = onlyModified || reloaded == 0 || named == 0 || ready != 0;
    if (!rec.ok) {
        char msg[64];
        rec.why = KitLoadWhy::NoneReadable;
        snprintf(msg, sizeof(msg), "%.22s: no sample loaded", kit->name.c_str());
        set_error(msg);
    }

    s_outcome.store(rec.ok ? 1 : -1, std::memory_order_release);
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
#if defined(ARDUINO)
    if (s_arduinoWorker) return true;
#else
    if (s_worker.running()) return true;
#endif

    if (!s_workPending.create()) return false;

    // Stack from PSRAM. xTaskCreatePinnedToCoreWithCaps is ESP-IDF v5 only;
    // the Arduino build uses a static BSS stack, its largest internal free
    // block (~7 kB) being under LOAD_TASK_STACK.
#if defined(ARDUINO)
    // 4 kB, as the Arduino loader task ran the same LoadPad work on.
    constexpr size_t kArduinoLoadStack = 4096;
    static StaticTask_t s_loadTcb;
    static StackType_t  s_loadStack[kArduinoLoadStack / sizeof(StackType_t)];
    s_arduinoWorker = xTaskCreateStaticPinnedToCore(load_fn, "kit_load", kArduinoLoadStack,
                                                    nullptr, LOAD_TASK_PRIORITY, s_loadStack,
                                                    &s_loadTcb, LOAD_TASK_CORE);
    return s_arduinoWorker != nullptr;
#else
    return s_worker.start(load_fn, nullptr, "kit_load", LOAD_TASK_STACK,
                          LOAD_TASK_PRIORITY, LOAD_TASK_CORE);
#endif
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
    s_padsDone.store(0, std::memory_order_relaxed);
    s_padsTotal.store(0, std::memory_order_relaxed);
    set_error(nullptr);
    s_outcome.store(0, std::memory_order_release);

    crosspad_gui::statusbar_set_loading(0);
    /* A whole kit gets the bar in the context row. A pad editor's reload of
     * the pads it changed does not: it would flash over the editor at every
     * edit. kitId -1 is the current kit again. */
    if (!onlyModified) {
        crosspad::KitInfo* kit = nullptr;
        if (auto* km = crosspad::getKitManager()) kit = kitId >= 0 ? km->getKit(kitId) : km->getCurrentKit();
        crosspad_gui::kit_loader_start(kit ? kit->name.c_str() : nullptr);
    }

    s_reqOnlyModified = onlyModified;
    s_reqKitId        = kitId;
    s_reqUs           = crosspad::getClock().getTimeUs();

    lv_timer_create(progress_poll_timer_cb, 100, nullptr);

    // Last: the worker runs as soon as this is given.
    s_workPending.give();
}

bool kit_load_task_is_worker() {
#if defined(ARDUINO)
    return s_arduinoWorker != nullptr && xTaskGetCurrentTaskHandle() == s_arduinoWorker;
#else
    return s_worker.isCurrent();
#endif
}

bool kit_load_task_is_loading() {
    return s_loading;
}

int kit_load_task_progress() {
    return s_progress.load(std::memory_order_relaxed);
}

int kit_load_task_pending_kit() {
    return s_pendingValid ? s_pendingKitId : -1;
}

int kit_load_task_swap_log_count() {
    if (!s_swaps) return 0;
    return s_swapCount < static_cast<uint32_t>(kSwapLog) ? static_cast<int>(s_swapCount) : kSwapLog;
}

bool kit_load_task_swap_log(int i, KitSwapRecord* out) {
    const int n = kit_load_task_swap_log_count();
    if (i < 0 || i >= n || !out) return false;
    const uint32_t seq = s_swapCount - n + i;          // 0-based index of the row
    *out = s_swaps[seq % kSwapLog];
    return true;
}

} // namespace crosspad_sampler
