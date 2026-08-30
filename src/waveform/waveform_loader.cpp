// SPDX-License-Identifier: MIT

#include "crosspad-sampler/waveform/waveform_loader.hpp"
#include "crosspad-sampler/waveform/waveform_cache.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"
#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"

#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace crosspad_sampler {

// ── Load request ────────────────────────────────────────────────────────

struct WaveformLoadRequest {
    char     filename[128];
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

// ── FreeRTOS state ──────────────────────────────────────────────────────

static TaskHandle_t   s_task    = nullptr;
static QueueHandle_t  s_queue   = nullptr;
static volatile bool  s_running = false;
static WaveformLoaderStats s_stats;

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
        if (!xQueueReceive(s_queue, &req, portMAX_DELAY)) continue;

        if (!s_running || req.filename[0] == '\0') break;

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

        // Load via platform callback (handles SD mutex, compression, etc.)
        bool ok = wf.getWaveformSamples(req.filename, buf, samplesToRead,
                                         actualStart, actualEnd);

        if (!s_running) {
            if (cbs.freeWaveformMem) cbs.freeWaveformMem(buf); else free(buf);
            break;
        }

        if (ok && waveform_cache_add(req.filename, buf, samplesToRead)) {
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
    vTaskDelete(nullptr);
}

// ── Public API ──────────────────────────────────────────────────────────

bool waveform_loader_start() {
    if (s_running) return false;

    if (!s_queue) {
        s_queue = xQueueCreate(LOADER_QUEUE_SIZE, sizeof(WaveformLoadRequest));
        if (!s_queue) return false;
    }

    BaseType_t r = xTaskCreatePinnedToCore(
        loader_task, "WFLoader",
        LOADER_STACK_SIZE, nullptr,
        LOADER_PRIORITY, &s_task, LOADER_CORE);

    if (r == pdPASS) { s_running = true; return true; }
    return false;
}

void waveform_loader_stop() {
    if (!s_running) return;
    s_running = false;

    WaveformLoadRequest term = {};
    xQueueSend(s_queue, &term, 0);

    for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_task) { vTaskDelete(s_task); s_task = nullptr; }
}

void waveform_loader_request(const char* filename, bool highPriority,
                              uint8_t padIndex,
                              uint32_t sampleStart, uint32_t sampleEnd) {
    if (!filename || !s_queue) return;
    if (waveform_cache_find(filename)) return;

    WaveformLoadRequest req = {};
    strncpy(req.filename, filename, sizeof(req.filename) - 1);
    req.priority    = highPriority ? 1 : 0;
    req.padIndex    = padIndex;
    req.attempt     = 0;
    req.sampleStart = sampleStart;
    req.sampleEnd   = sampleEnd;

    const BaseType_t sent = highPriority
        ? xQueueSendToFront(s_queue, &req, 0)
        : xQueueSend(s_queue, &req, 0);
    if (sent == pdTRUE) s_stats.requested++;
    else                s_stats.dropped++;
}

int waveform_loader_preload_kit(int maxPads) {
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
}

void waveform_loader_get_stats(WaveformLoaderStats* out) {
    if (!out) return;
    *out = s_stats;
    out->queued = s_queue ? (uint32_t)uxQueueMessagesWaiting(s_queue) : 0;
}

} // namespace crosspad_sampler
