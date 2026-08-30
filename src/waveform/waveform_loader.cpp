// SPDX-License-Identifier: MIT

#include "crosspad-sampler/waveform/waveform_loader.hpp"
#include "crosspad-sampler/waveform/waveform_cache.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"
#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"

#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include "crosspad-sampler/port/SamplerRtos.hpp"

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

// ── Worker state ─────────────────────────────────────────────────────────

static SamplerWorker  s_worker;
static SamplerQueue<WaveformLoadRequest> s_queue;
static volatile bool  s_running = false;
static WaveformLoaderStats s_stats;

static void requeue_or_fail(WaveformLoadRequest& req, uint32_t& failCounter) {
    if (req.attempt + 1 < kMaxAttempts) {
        req.attempt++;
        if (s_queue.send(req)) {
            s_stats.retried++;
            sampler_delay_ms(kRetryDelayMs);
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

    // Parks between jobs and never returns. It used to end itself on the stop
    // signal, which left waveform_loader_start() believing a worker existed
    // and quietly never loading another waveform after the first stop.
    for (;;) {
        if (!s_queue.receive(req, kWaitForever)) continue;

        // Stop signal, or a wake with nothing to do: go back to the queue.
        if (!s_running || req.filename[0] == '\0') continue;

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

        if (!s_running) { free(buf); continue; }

        // Load via platform callback (handles SD mutex, compression, etc.)
        bool ok = wf.getWaveformSamples(req.filename, buf, samplesToRead,
                                         actualStart, actualEnd);

        if (!s_running) {
            if (cbs.freeWaveformMem) cbs.freeWaveformMem(buf); else free(buf);
            continue;
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

        // Yield between loads
        sampler_delay_ms(10);
    }
}

// ── Public API ──────────────────────────────────────────────────────────

bool waveform_loader_start() {
    if (s_running) return false;

    if (!s_queue.create(LOADER_QUEUE_SIZE)) return false;

    s_running = true;
    if (s_worker.started()) return true;   // already parked on the queue
    if (!s_worker.start(loader_task, nullptr, "WFLoader",
                        LOADER_STACK_SIZE, LOADER_PRIORITY, LOADER_CORE)) {
        s_running = false;
        return false;
    }
    return true;
}

void waveform_loader_stop() {
    if (!s_running) return;
    s_running = false;

    // Wake the worker so it notices the flag; it parks on the queue again
    // rather than ending, so its stack is paid for once per boot.
    WaveformLoadRequest term = {};
    s_queue.send(term);
}

void waveform_loader_request(const char* filename, bool highPriority,
                              uint8_t padIndex,
                              uint32_t sampleStart, uint32_t sampleEnd) {
    if (!filename || !s_queue.valid()) return;
    if (waveform_cache_find(filename)) return;

    WaveformLoadRequest req = {};
    strncpy(req.filename, filename, sizeof(req.filename) - 1);
    req.priority    = highPriority ? 1 : 0;
    req.padIndex    = padIndex;
    req.attempt     = 0;
    req.sampleStart = sampleStart;
    req.sampleEnd   = sampleEnd;

    const bool sent = highPriority ? s_queue.sendToFront(req) : s_queue.send(req);
    if (sent) s_stats.requested++;
    else                s_stats.dropped++;
}

int waveform_loader_preload_kit(int maxPads) {
    if (!s_queue.valid()) return 0;

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
    s_queue.reset();
}

void waveform_loader_get_stats(WaveformLoaderStats* out) {
    if (!out) return;
    *out = s_stats;
    out->queued = (uint32_t)s_queue.count();
}

} // namespace crosspad_sampler
