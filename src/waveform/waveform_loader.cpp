// SPDX-License-Identifier: MIT

#include "crosspad-sampler/waveform/waveform_loader.hpp"
#include "crosspad-sampler/waveform/waveform_cache.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"
#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"

#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace crosspad_sampler {

// ---------------------------------------------------------------------------
// Load request
// ---------------------------------------------------------------------------

struct WaveformLoadRequest {
    char     filename[128];
    uint8_t  priority;
    uint8_t  padIndex;
    uint32_t sampleStart;
    uint32_t sampleEnd;
};

// ---------------------------------------------------------------------------
// FreeRTOS state
// ---------------------------------------------------------------------------

static TaskHandle_t   s_task    = nullptr;
static QueueHandle_t  s_queue   = nullptr;
static volatile bool  s_running = false;

// ---------------------------------------------------------------------------
// Loader task
// ---------------------------------------------------------------------------

static void loader_task(void* /*param*/) {
    auto& wf = sampler_get_platform_callbacks().waveform;

    WaveformLoadRequest req;

    while (s_running) {
        if (!xQueueReceive(s_queue, &req, portMAX_DELAY)) continue;

        // Termination signal
        if (!s_running || req.filename[0] == '\0') break;

        // Already cached?
        if (waveform_cache_find(req.filename)) continue;

        // Get file info via platform callback
        if (!wf.getFileInfo || !wf.getWaveformSamples) {
            printf("[WFLoader] No platform waveform callbacks\n");
            continue;
        }

        AudioFileInfo info = {};
        if (!wf.getFileInfo(req.filename, &info) || info.sampleCount == 0) {
            printf("[WFLoader] Failed to get info: %s\n", req.filename);
            continue;
        }

        uint32_t actualEnd = (req.sampleEnd == 0 || req.sampleEnd > info.sampleCount)
                              ? info.sampleCount : req.sampleEnd;
        uint32_t actualStart = (req.sampleStart >= actualEnd) ? 0 : req.sampleStart;
        uint32_t rangeLength = actualEnd - actualStart;

        uint32_t samplesToRead = (rangeLength < TARGET_CACHE_SAMPLES)
                                  ? rangeLength : TARGET_CACHE_SAMPLES;

        // Allocate temp buffer via platform allocator
        auto& cbs = sampler_get_platform_callbacks().waveform;
        int16_t* buf = nullptr;
        if (cbs.allocWaveformMem)
            buf = (int16_t*)cbs.allocWaveformMem(samplesToRead * sizeof(int16_t));
        else
            buf = (int16_t*)malloc(samplesToRead * sizeof(int16_t));

        if (!buf) {
            printf("[WFLoader] Alloc failed for %s\n", req.filename);
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

        if (ok) {
            waveform_cache_add(req.filename, buf, samplesToRead);
        } else {
            printf("[WFLoader] Failed to load: %s\n", req.filename);
        }

        if (cbs.freeWaveformMem) cbs.freeWaveformMem(buf); else free(buf);

        // Yield between loads
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_running = false;
    s_task = nullptr;
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

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
    req.sampleStart = sampleStart;
    req.sampleEnd   = sampleEnd;

    if (highPriority)
        xQueueSendToFront(s_queue, &req, 0);
    else
        xQueueSend(s_queue, &req, 0);
}

void waveform_loader_preload_kit(int maxPads) {
    if (!s_queue) return;

    auto* mgr = crosspad::getPlatformServices().kitManager;
    if (!mgr) return;
    auto* kit = mgr->getCurrentKit();
    if (!kit) return;

    auto& kitCb = sampler_get_platform_callbacks().kit;
    if (maxPads > 16) maxPads = 16;

    for (int i = 0; i < maxPads; i++) {
        if (kit->pads[i].samples.empty()) continue;

        // Resolve full path via platform callback
        const char* fullPath = nullptr;
        if (kitCb.getFullPath)
            fullPath = kitCb.getFullPath(kit->pads[i].samples[0].sample.c_str());
        if (!fullPath || fullPath[0] == '\0') continue;

        if (waveform_cache_find(fullPath)) continue;

        uint32_t start = kit->pads[i].samples[0].start;
        uint32_t end   = kit->pads[i].samples[0].end;

        waveform_loader_request(fullPath, false, i, start, end);
    }
}

bool waveform_loader_is_running() {
    return s_running;
}

void waveform_loader_clear_queue() {
    if (s_queue) xQueueReset(s_queue);
}

} // namespace crosspad_sampler
