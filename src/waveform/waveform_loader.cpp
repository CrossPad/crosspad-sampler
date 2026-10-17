// SPDX-License-Identifier: GPL-3.0-or-later

#include "crosspad-sampler/waveform/waveform_loader.hpp"
#include "crosspad-sampler/waveform/waveform_cache.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"
#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"
#include "crosspad-sampler/logic/kit_load_task.hpp"

#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#if !defined(ARDUINO)
#include "esp_heap_caps.h"
#endif

#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace crosspad_sampler {

// ── Load request ────────────────────────────────────────────────────────

/* A pad request (kPadRequest in sampleSlot's place) names a pad's sample of
 * the current kit; the loader resolves it to a file. kSweep in padIndex asks
 * for a walk of the whole kit. */
constexpr uint8_t kNoSlot = 0xFF;
constexpr uint8_t kSweep  = 0xFF;
struct WaveformLoadRequest {
    char     filename[128];
    uint8_t  sampleSlot;      ///< kNoSlot: filename is the file
    uint8_t  priority;
    uint8_t  padIndex;
    uint8_t  attempt;
    uint32_t sampleStart;
    uint32_t sampleEnd;
};

/* A read can fail for a passing reason: the FAT descriptor pool is shared
 * with the sample streamer, so an fopen() under playback can lose the race.
 * A failed request goes back on the queue until this many attempts. */
constexpr uint8_t  kMaxAttempts  = 4;
constexpr uint32_t kRetryDelayMs = 150;

/* A request that meets a full queue is lost, and nothing used to ask for it
 * again: a kit with more layers than the queue, or one loaded while the queue
 * still held the last kit's, simply never got those waveforms for as long as
 * it stayed loaded. So the queue running dry is a cue to walk the current kit
 * again. The budget is per kit — a layer whose file is broken fails for good,
 * and re-queueing it forever would be a card read every second. */
constexpr uint32_t kSweepIdleMs  = 1000;
constexpr uint8_t  kSweepBudget  = 8;

// ── FreeRTOS state ──────────────────────────────────────────────────────

static TaskHandle_t   s_task    = nullptr;
static QueueHandle_t  s_queue   = nullptr;
static volatile bool  s_running = false;
static WaveformLoaderStats s_stats;
static uint8_t s_sweepsLeft = kSweepBudget;   // loader task + clear_queue only
static uint32_t s_kitRequested = 0;           // requests since this kit was picked

static void requeue_or_fail(WaveformLoadRequest& req, uint32_t& failCounter) {
    if (req.attempt + 1 < kMaxAttempts) {
        req.attempt++;
        if (xQueueSend(s_queue, &req, 0) == pdTRUE) {
            s_stats.retried++;
            vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
            return;
        }
        s_stats.dropped++;
    }
    failCounter++;
}

// ── Loader task ─────────────────────────────────────────────────────────

static void loader_task(void* /*param*/) {
    auto& wf = sampler_get_platform_callbacks().waveform;

    WaveformLoadRequest req;

    while (s_running) {
        if (!xQueueReceive(s_queue, &req, pdMS_TO_TICKS(kSweepIdleMs))) {
            // Nothing waiting: top up from the kit, then block again.
            if (s_running && s_sweepsLeft > 0) {
                s_stats.sweeps++;
                if (waveform_loader_preload_kit(16) == 0) s_sweepsLeft = 0;
                else                                      s_sweepsLeft--;
            }
            continue;
        }

        if (!s_running) break;

        /* A pad's sample or a walk of the kit, asked for by the LVGL thread:
         * the stat() that tells the kit's two layouts apart happens here. */
        if (req.sampleSlot != kNoSlot) {
            if (req.padIndex == kSweep) {
                waveform_loader_preload_kit(16);
                continue;
            }
            auto* km = crosspad::getPlatformServices().kitManager;
            auto* kit = km ? km->getCurrentKit() : nullptr;
            if (!kit || req.padIndex >= PAD_COUNT ||
                req.sampleSlot >= kit->pads[req.padIndex].samples.size()) continue;
            const std::string path = crosspad::resolveKitSamplePath(
                kit->path, kit->pads[req.padIndex].samples[req.sampleSlot].sample);
            if (path.empty() || path.size() >= sizeof(req.filename)) continue;
            memcpy(req.filename, path.c_str(), path.size() + 1);
            req.sampleSlot = kNoSlot;
        }

        if (req.filename[0] == '\0') break;

        if (waveform_cache_find(req.filename)) continue;

        if (!wf.getFileInfo || !wf.getWaveformSamples) {
            printf("[WFLoader] No platform waveform callbacks\n");
            continue;
        }

        AudioFileInfo info = {};
        if (!wf.getFileInfo(req.filename, &info) || info.sampleCount == 0) {
            printf("[WFLoader] Failed to get info (attempt %u): %s\n",
                   (unsigned)req.attempt + 1, req.filename);
            requeue_or_fail(req, s_stats.failedInfo);
            continue;
        }

        uint32_t actualEnd = (req.sampleEnd == 0 || req.sampleEnd > info.sampleCount)
                              ? info.sampleCount : req.sampleEnd;
        uint32_t actualStart = (req.sampleStart >= actualEnd) ? 0 : req.sampleStart;
        uint32_t rangeLength = actualEnd - actualStart;

        uint32_t samplesToRead = (rangeLength < TARGET_CACHE_SAMPLES)
                                  ? rangeLength : TARGET_CACHE_SAMPLES;

        auto& cbs = sampler_get_platform_callbacks().waveform;
        int16_t* buf = nullptr;
        if (cbs.allocWaveformMem)
            buf = (int16_t*)cbs.allocWaveformMem(samplesToRead * sizeof(int16_t));
        else
            buf = (int16_t*)malloc(samplesToRead * sizeof(int16_t));

        if (!buf) {
            printf("[WFLoader] Alloc failed for %s\n", req.filename);
            requeue_or_fail(req, s_stats.allocFail);
            continue;
        }

        if (!s_running) { free(buf); break; }

        /* Cosmetic, and the card is what a kit swap is short of: every hit on
         * a pad of a freshly changed kit asked for its waveform, a whole-file
         * read per hit beside the loader and the streamer. Wait here, where a
         * stop is still seen — waiting inside the platform read left this task
         * to be killed mid-fopen by waveform_loader_stop(), a handle and a
         * buffer of internal RAM gone each time. */
        while (s_running && kit_load_task_is_loading()) vTaskDelay(pdMS_TO_TICKS(50));
        if (!s_running) { free(buf); break; }

        // Load via platform callback (handles SD mutex, compression, etc.)
        bool ok = wf.getWaveformSamples(req.filename, buf, samplesToRead,
                                         actualStart, actualEnd);

        if (!s_running) {
            if (cbs.freeWaveformMem) cbs.freeWaveformMem(buf); else free(buf);
            break;
        }

        /* A read the platform abandoned because a kit load started is not a
         * failed read: it must not burn one of the four attempts, or a long
         * load would spend them all and lose the layer. */
        if (!ok && kit_load_task_is_loading()) {
            if (cbs.freeWaveformMem) cbs.freeWaveformMem(buf); else free(buf);
            while (s_running && kit_load_task_is_loading()) vTaskDelay(pdMS_TO_TICKS(50));
            if (xQueueSend(s_queue, &req, 0) != pdTRUE) s_stats.dropped++;
            continue;
        }

        if (ok && waveform_cache_add(req.filename, buf, samplesToRead,
                                     info.sampleCount, info.sampleRate,
                                     info.channels, info.bitDepth)) {
            s_stats.loaded++;
        } else {
            printf("[WFLoader] Failed to load (attempt %u): %s\n",
                   (unsigned)req.attempt + 1, req.filename);
            if (cbs.freeWaveformMem) cbs.freeWaveformMem(buf); else free(buf);
            buf = nullptr;
            requeue_or_fail(req, s_stats.failedRead);
            continue;
        }

        if (cbs.freeWaveformMem) cbs.freeWaveformMem(buf); else free(buf);

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_running = false;
    s_task = nullptr;
#if defined(ARDUINO)
    vTaskDelete(nullptr);
#else
    vTaskDeleteWithCaps(nullptr);
#endif
}

// ── Public API ──────────────────────────────────────────────────────────

bool waveform_loader_start() {
    if (s_running) return false;

    if (!s_queue) {
        /* Storage in PSRAM, like the task's stack: 4.5 kB of internal RAM
         * otherwise, and the consumer already runs on an external stack. The
         * queue is never deleted, so nothing has to free it WithCaps. */
#if defined(ARDUINO)
        s_queue = xQueueCreate(LOADER_QUEUE_SIZE, sizeof(WaveformLoadRequest));
#else
        s_queue = xQueueCreateWithCaps(LOADER_QUEUE_SIZE, sizeof(WaveformLoadRequest),
                                       MALLOC_CAP_SPIRAM);
#endif
        if (!s_queue) return false;
    }

    /* Running before the create, not after: the loader outranks the LVGL
     * task that starts it, so it could run at once, find the flag still
     * false and end — and then the flag went true over a task that was
     * gone. Nothing asked for it again, the queue filled and every waveform
     * stayed LOADING. */
    s_running = true;

    /* Stack from PSRAM, the way kit_load takes its. 6 kB of internal RAM in
     * one piece is more than the launcher with a kit up tends to have left:
     * the create failed, the queue filled behind it and the kit had no
     * waveform at all. A task created WithCaps must be deleted WithCaps. */
#if defined(ARDUINO)
    BaseType_t r = xTaskCreatePinnedToCore(
        loader_task, "WFLoader",
        LOADER_STACK_SIZE, nullptr,
        LOADER_PRIORITY, &s_task, LOADER_CORE);
#else
    BaseType_t r = xTaskCreatePinnedToCoreWithCaps(
        loader_task, "WFLoader",
        LOADER_STACK_SIZE, nullptr,
        LOADER_PRIORITY, &s_task, LOADER_CORE,
        MALLOC_CAP_SPIRAM);
#endif

    if (r == pdPASS) return true;
    s_running = false;

    /* 6 kB of internal RAM in one piece, and the launcher with a kit up has
     * had less than that. A failure here used to be a false nobody read: the
     * queue filled, WAVE_STATUS said loader=0 and the kit had no waveforms. */
    s_stats.startFail++;
    printf("[WFLoader] task create failed (stack %d B internal)\n", LOADER_STACK_SIZE);
    return false;
}

void waveform_loader_stop() {
    if (!s_running) return;
    s_running = false;

    WaveformLoadRequest term = {};
    xQueueSend(s_queue, &term, 0);

    for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_task) {
#if defined(ARDUINO)
        vTaskDelete(s_task);
#else
        vTaskDeleteWithCaps(s_task);
#endif
        s_task = nullptr;
    }
}

void waveform_loader_request(const char* filename, bool highPriority,
                              uint8_t padIndex,
                              uint32_t sampleStart, uint32_t sampleEnd) {
    if (!filename) return;
    // Self-heal: a request is also the cue to bring the task back if a
    // previous create failed for want of internal RAM.
    if (!s_running) waveform_loader_start();
    if (!s_queue) return;
    if (waveform_cache_find(filename)) return;

    WaveformLoadRequest req = {};
    strncpy(req.filename, filename, sizeof(req.filename) - 1);
    req.sampleSlot  = kNoSlot;
    req.priority    = highPriority ? 1 : 0;
    req.padIndex    = padIndex;
    req.attempt     = 0;
    req.sampleStart = sampleStart;
    req.sampleEnd   = sampleEnd;

    const BaseType_t sent = highPriority
        ? xQueueSendToFront(s_queue, &req, 0)
        : xQueueSend(s_queue, &req, 0);
    if (sent == pdTRUE) { s_stats.requested++; s_kitRequested++; }
    else                s_stats.dropped++;
}

static void post_pad_request(uint8_t padIndex, uint8_t sampleSlot, bool highPriority) {
    if (!s_running) waveform_loader_start();
    if (!s_queue) return;
    WaveformLoadRequest req = {};
    req.padIndex   = padIndex;
    req.sampleSlot = sampleSlot;
    req.priority   = highPriority ? 1 : 0;
    const BaseType_t sent = highPriority ? xQueueSendToFront(s_queue, &req, 0)
                                         : xQueueSend(s_queue, &req, 0);
    if (sent != pdTRUE) s_stats.dropped++;
}

void waveform_loader_request_pad(uint8_t padIndex, uint8_t sampleSlot, bool highPriority) {
    if (padIndex >= PAD_COUNT || sampleSlot == kNoSlot) return;
    post_pad_request(padIndex, sampleSlot, highPriority);
}

void waveform_loader_sweep_now() {
    post_pad_request(kSweep, 0, false);
}

int waveform_loader_preload_kit(int maxPads) {
    if (!s_running) waveform_loader_start();
    if (!s_queue) return 0;

    auto* mgr = crosspad::getPlatformServices().kitManager;
    if (!mgr) return 0;
    auto* kit = mgr->getCurrentKit();
    if (!kit) return 0;

    if (maxPads > 16) maxPads = 16;

    int queued = 0;
    for (int i = 0; i < maxPads; i++) {
        for (const auto& sample : kit->pads[i].samples) {
            std::string resolvedPath = crosspad::resolveKitSamplePath(kit->path, sample.sample);
            if (resolvedPath.empty()) continue;
            if (waveform_cache_find(resolvedPath.c_str())) continue;

            // The full-file envelope: crop in/out is drawn as markers over
            // the rendered view, so trimming the cache would misalign them.
            const uint32_t before = s_stats.requested;
            waveform_loader_request(resolvedPath.c_str(), false, (uint8_t)i, 0, 0);
            if (s_stats.requested != before) ++queued;
        }
    }
    return queued;
}

bool waveform_loader_is_running() {
    return s_running;
}

void waveform_loader_clear_queue() {
    if (s_queue) xQueueReset(s_queue);
    s_sweepsLeft = kSweepBudget;   // a new kit gets its own budget
    s_kitRequested = 0;
}

int waveform_loader_progress() {
    if (!s_queue || s_kitRequested == 0) return -1;
    const uint32_t left = (uint32_t)uxQueueMessagesWaiting(s_queue);
    if (left == 0) return -1;
    const uint32_t done = (left >= s_kitRequested) ? 0 : (s_kitRequested - left);
    return (int)(done * 100 / s_kitRequested);
}

void waveform_loader_get_stats(WaveformLoaderStats* out) {
    if (!out) return;
    *out = s_stats;
    out->queued = s_queue ? (uint32_t)uxQueueMessagesWaiting(s_queue) : 0;
}

} // namespace crosspad_sampler
