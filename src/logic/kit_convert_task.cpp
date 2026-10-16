// SPDX-License-Identifier: GPL-3.0-or-later
//
// The kit rate conversion's worker and its follow-up on the LVGL thread. See
// kit_convert_task.hpp for the order of things; three facts are load-bearing:
//
//  * The worker never touches the kit model, LVGL or the loader. It runs the
//    converter and publishes a copy of its progress under a mutex; a timer on
//    the LVGL thread does the rest.
//  * Its stack is PSRAM and it sits at priority 1 on core 1: the conversion
//    is CPU (FFTs) and card time nobody is waiting for, and everything that
//    is waited for -- the render, the streamer, the loader, the UI -- outranks
//    it. It sleeps a tick every kSliceMs so the idle task is never starved.
//  * The reload goes through the kit loader only when no load is running: the
//    loader keeps one pending request, and ours would replace a kit the user
//    asked for meanwhile.

#include <crosspad-sampler/logic/kit_convert_task.hpp>
#include <crosspad-sampler/logic/kit_load_task.hpp>
#include <crosspad-sampler/SamplerConfig.hpp>
#include <crosspad-sampler/SamplerPlatformCallbacks.hpp>

#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/platform/IClock.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include <lvgl.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/idf_additions.h>

#include <esp_heap_caps.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace crosspad_sampler {

namespace {

constexpr size_t      kStack       = 8192;
constexpr UBaseType_t kPriority    = 1;
constexpr BaseType_t  kCore        = 1;
constexpr uint32_t    kSliceMs     = 20;      // work between two sleeps
constexpr uint32_t    kPublishMs   = 100;     // progress copies for the UI
constexpr uint32_t    kPollMs      = 100;     // the LVGL timer
constexpr uint32_t    kTidyRetryMs = 10000;   // a file a voice still read
constexpr uint8_t     kTidyRetries = 6;

TaskHandle_t      s_worker = nullptr;
SemaphoreHandle_t s_doorbell = nullptr;
SemaphoreHandle_t s_mux = nullptr;

// The request. Written on the LVGL thread before the doorbell, read by the
// worker after it. Paths are fixed arrays: a std::string of this length is a
// small allocation, and small allocations come out of internal RAM.
KitConvertJob s_reqJob = KitConvertJob::None;
char          s_reqPath[192] = {};
uint32_t      s_reqRate = 0;
bool          s_reqKeep = false;
std::atomic<bool> s_cancelReq{false};

// Published by the worker under s_mux.
KitConvertStatus s_status;
bool             s_workerBusy = false;
bool             s_finished = false;           // a job ended, the timer has not seen it
std::vector<crosspad::KitConvertEdit> s_edits;  // the last Convert's, handed to the timer

// The LVGL thread's own.
lv_timer_t* s_timer = nullptr;
int         s_tidyKit = -1;            // tidy once no load runs
uint32_t    s_tidyRate = 0;
uint8_t     s_tidyTries = 0;
uint32_t    s_tidyAtMs = 0;            // not before this
bool        s_reloadPending = false;   // the converted kit is current: reload it first
int64_t     s_startUs = 0;

uint32_t now_ms() { return static_cast<uint32_t>(crosspad::getClock().getTimeUs() / 1000); }

class Lock {
public:
    Lock() { xSemaphoreTake(s_mux, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mux); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

crosspad::IKitConvertHost* host() { return sampler_get_platform_callbacks().audio.convertHost; }

void publish(const crosspad::KitRateConverter& conv) {
    Lock lk;
    s_status.progress = conv.progress();
    s_status.elapsedMs = static_cast<uint32_t>((crosspad::getClock().getTimeUs() - s_startUs) / 1000);
}

void run_convert() {
    crosspad::KitRateConverter conv;
    conv.begin(*host(), s_reqPath, s_reqRate);
    uint32_t sliceStart = now_ms();
    uint32_t lastPublish = 0;
    for (;;) {
        if (s_cancelReq.load(std::memory_order_relaxed)) conv.cancel();
        if (!conv.step()) break;
        const uint32_t t = now_ms();
        if (t - lastPublish >= kPublishMs) { publish(conv); lastPublish = t; }
        if (t - sliceStart >= kSliceMs) { vTaskDelay(1); sliceStart = now_ms(); }
    }
    publish(conv);
    Lock lk;
    s_edits = conv.edits();
}

void run_job() {
    s_startUs = crosspad::getClock().getTimeUs();
    switch (s_reqJob) {
    case KitConvertJob::Convert:
        run_convert();
        break;
    case KitConvertJob::KeepRate: {
        const bool ok = crosspad::setKitKeepRate(*host(), s_reqPath, s_reqKeep);
        Lock lk;
        s_status.keepOk = ok;
        break;
    }
    case KitConvertJob::Tidy: {
        const crosspad::KitTidyResult r = crosspad::tidyKit(*host(), s_reqPath, DEFAULT_KIT_PATH, s_reqRate);
        Lock lk;
        s_status.tidy = r;
        break;
    }
    default:
        break;
    }
    Lock lk;
    s_status.elapsedMs = static_cast<uint32_t>((crosspad::getClock().getTimeUs() - s_startUs) / 1000);
    ++s_status.seq;
    s_workerBusy = false;
    s_finished = true;
}

void worker_fn(void*) {
    for (;;) {
        if (xSemaphoreTake(s_doorbell, portMAX_DELAY) == pdTRUE) run_job();
    }
}

bool ensure_worker() {
    if (s_worker) return true;
    if (!s_mux) s_mux = xSemaphoreCreateMutex();
    if (!s_doorbell) s_doorbell = xSemaphoreCreateBinary();
    if (!s_mux || !s_doorbell) return false;
#if defined(ARDUINO)
    static StaticTask_t s_tcb;
    static StackType_t  s_stack[kStack / sizeof(StackType_t)];
    s_worker = xTaskCreateStaticPinnedToCore(worker_fn, "kit_conv", kStack, nullptr, kPriority,
                                             s_stack, &s_tcb, kCore);
    return s_worker != nullptr;
#else
    if (xTaskCreatePinnedToCoreWithCaps(worker_fn, "kit_conv", kStack, nullptr, kPriority, &s_worker,
                                        kCore, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_worker = nullptr;
        return false;
    }
    return true;
#endif
}

crosspad::KitInfo* kit_by_id(int id) {
    auto* km = crosspad::getKitManager();
    return km ? km->getKit(id) : nullptr;
}

/// The worker's request, and the timer that sees it through.
bool post(KitConvertJob job, int kitId, uint32_t rate, bool keep = false) {
    crosspad::KitInfo* kit = kit_by_id(kitId);
    if (!kit || kit->path.empty() || kit->path.size() >= sizeof(s_reqPath) || !host()) return false;
    if (!ensure_worker()) return false;
    {
        Lock lk;
        if (s_workerBusy) return false;
        s_workerBusy = true;
        s_finished = false;
        s_status.job = job;
        s_status.kitId = kitId;
        s_status.elapsedMs = 0;
        if (job == KitConvertJob::Convert) {
            s_status.progress = crosspad::KitRateConverter::Progress();
            s_status.progress.phase = crosspad::KitRateConverter::Phase::Planning;
            s_status.progress.toRate = rate;
            s_status.reloaded = false;
        }
    }
    s_reqJob = job;
    std::snprintf(s_reqPath, sizeof(s_reqPath), "%s", kit->path.c_str());
    s_reqRate = rate;
    s_reqKeep = keep;
    s_cancelReq.store(false, std::memory_order_relaxed);
    xSemaphoreGive(s_doorbell);
    return true;
}

/// The commit's edits, made to the kit model in memory: an entry of that pad
/// still saying what the card said before is moved to the converted file.
void apply_edits(crosspad::KitInfo& kit, const std::vector<crosspad::KitConvertEdit>& edits) {
    if (!kit.parsed) return;     // read from the card at its next load: already converted there
    for (const auto& e : edits) {
        if (e.pad >= KIT_PADS) continue;
        for (auto& s : kit.pads[e.pad].samples) {
            if (s.sample != e.oldSample || s.start != e.oldStart || s.end != e.oldEnd) continue;
            s.sample = e.newSample;
            s.start = e.start;
            s.end = e.end;
        }
    }
}

void poll_cb(lv_timer_t*);

void ensure_timer() {
    if (!s_timer) s_timer = lv_timer_create(poll_cb, kPollMs, nullptr);
}

void on_finished(const KitConvertStatus& st, std::vector<crosspad::KitConvertEdit>& edits) {
    if (st.job == KitConvertJob::Convert) {
        if (st.progress.phase != crosspad::KitRateConverter::Phase::Done) return;
        crosspad::KitInfo* kit = kit_by_id(st.kitId);
        if (kit && !edits.empty()) apply_edits(*kit, edits);
        auto* km = crosspad::getKitManager();
        s_reloadPending = !edits.empty() && km && km->getCurrentKitId() == st.kitId;
        s_tidyKit = st.kitId;
        s_tidyRate = st.progress.toRate;
        s_tidyTries = kTidyRetries;
        s_tidyAtMs = 0;
        return;
    }
    if (st.job == KitConvertJob::Tidy && st.tidy.pending > 0 && s_tidyTries > 0) {
        s_tidyKit = st.kitId;
        s_tidyAtMs = now_ms() + kTidyRetryMs;
    }
}

void poll_cb(lv_timer_t*) {
    KitConvertStatus st;
    std::vector<crosspad::KitConvertEdit> edits;
    bool finished = false;
    {
        Lock lk;
        finished = s_finished;
        s_finished = false;
        st = s_status;
        if (finished) edits = std::move(s_edits);
        s_edits.clear();
    }
    if (finished) on_finished(st, edits);

    if (kit_load_task_is_loading()) return;   // nothing below may go under a load
    if (s_reloadPending) {
        s_reloadPending = false;
        {
            Lock lk;
            s_status.reloaded = true;
        }
        kit_load_task_start(false, -1);
        return;                               // the tidy waits for that load
    }
    if (s_tidyKit >= 0 && !kit_convert_busy() && static_cast<int32_t>(now_ms() - s_tidyAtMs) >= 0) {
        const int kit = s_tidyKit;
        s_tidyKit = -1;
        if (s_tidyTries) --s_tidyTries;
        post(KitConvertJob::Tidy, kit, s_tidyRate);
        return;
    }
    bool idle;
    {
        Lock lk;
        idle = !s_workerBusy && !s_finished;
    }
    if (idle && s_tidyKit < 0 && !s_reloadPending) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
}

} // namespace

uint32_t kit_convert_engine_rate() {
    KitRateReport r;
    auto& cb = sampler_get_platform_callbacks();
    if (cb.audio.kitRateReport && cb.audio.kitRateReport(&r) && r.engineRate) return r.engineRate;
    return 44100;
}

bool kit_convert_start(int kitId, uint32_t rate) {
    if (s_reloadPending || s_tidyKit >= 0) return false;
    if (!post(KitConvertJob::Convert, kitId, rate ? rate : kit_convert_engine_rate())) return false;
    ensure_timer();
    return true;
}

void kit_convert_cancel() {
    // The converter lives on the worker's stack: the worker hands this on to
    // it before its next step.
    s_cancelReq.store(true, std::memory_order_relaxed);
}

bool kit_convert_busy() {
    if (!s_mux) return false;
    Lock lk;
    return s_workerBusy || s_finished;
}

KitConvertStatus kit_convert_status() {
    KitConvertStatus st;
    if (!s_mux) return st;
    {
        Lock lk;
        st = s_status;
        st.busy = s_workerBusy || s_finished;
    }
    st.busy = st.busy || s_reloadPending || s_tidyKit >= 0;
    return st;
}

bool kit_convert_keep_rate(int kitId, bool keep) {
    crosspad::KitInfo* kit = kit_by_id(kitId);
    if (!kit) return false;
    if (!post(KitConvertJob::KeepRate, kitId, 0, keep)) return false;
    kit->keepRate = keep;
    ensure_timer();
    return true;
}

bool kit_convert_tidy(int kitId) {
    if (!post(KitConvertJob::Tidy, kitId, kit_convert_engine_rate())) return false;
    ensure_timer();
    return true;
}

} // namespace crosspad_sampler
