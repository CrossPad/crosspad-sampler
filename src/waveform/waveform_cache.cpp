// SPDX-License-Identifier: MIT

#include "crosspad-sampler/waveform/waveform_cache.hpp"
#include "crosspad-sampler/waveform/waveform_loader.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"
#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cstdio>

namespace crosspad_sampler {

// ── Progress tracking ───────────────────────────────────────────────────

int totalWaveformsToCache = 0;
int waveformsCached       = 0;

// ── Internal state ──────────────────────────────────────────────────────

static WaveformCacheEntry s_cache[WAVEFORM_CACHE_SIZE] = {};
static uint32_t           s_timestamp        = 0;
static void*              s_cachedKit        = nullptr;
static char               s_cachedKitPath[256] = {};
static bool               s_savedThisBatch   = false;
static SemaphoreHandle_t  s_mutex            = nullptr;
static std::atomic<uint32_t> s_generation{0};

// ── Memory helpers — route through platform callbacks if available ──────

static void* cache_alloc(uint32_t bytes) {
    auto& wf = sampler_get_platform_callbacks().waveform;
    if (wf.allocWaveformMem) return wf.allocWaveformMem(bytes);
    return malloc(bytes);
}

static void cache_free(void* ptr) {
    auto& wf = sampler_get_platform_callbacks().waveform;
    if (wf.freeWaveformMem) { wf.freeWaveformMem(ptr); return; }
    free(ptr);
}

// ── Lock ────────────────────────────────────────────────────────────────

/* Created on first use, whichever function that is. It used to exist only
 * after waveform_cache_init(), which only the kit selector called — every
 * other way into a kit left it null, and a null mutex made add() and find()
 * answer "no" silently, so nothing was ever cached. */
static SemaphoreHandle_t lock() {
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    return s_mutex;
}

// ── LRU helper ──────────────────────────────────────────────────────────

static WaveformCacheEntry* find_lru_entry() {
    for (int i = 0; i < WAVEFORM_CACHE_SIZE; i++) {
        if (!s_cache[i].valid) return &s_cache[i];
    }
    uint32_t minTs = UINT32_MAX;
    int lru = 0;
    for (int i = 0; i < WAVEFORM_CACHE_SIZE; i++) {
        if (s_cache[i].lastUsed < minTs) {
            minTs = s_cache[i].lastUsed;
            lru = i;
        }
    }
    return &s_cache[lru];
}

// ── Disk cache path helper ──────────────────────────────────────────────

static bool get_cache_path(const char* kitJsonPath, char* out, int maxLen) {
    if (!kitJsonPath || !out) return false;
    const char* lastSlash = strrchr(kitJsonPath, '/');
    if (!lastSlash) return false;
    int dirLen = (int)(lastSlash - kitJsonPath);
    if (dirLen + 16 >= maxLen) return false;        // "/waveform.cache\0"
    memcpy(out, kitJsonPath, dirLen);
    strcpy(out + dirLen, "/waveform.cache");
    return true;
}

// ── Public API ──────────────────────────────────────────────────────────

void waveform_cache_init() {
    if (!lock()) return;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY)) {
        for (int i = 0; i < WAVEFORM_CACHE_SIZE; i++) {
            if (s_cache[i].waveformData) {
                cache_free(s_cache[i].waveformData);
            }
            s_cache[i] = WaveformCacheEntry{};
        }
        s_timestamp = 0;
        s_cachedKit = nullptr;
        s_cachedKitPath[0] = '\0';
        s_savedThisBatch = false;
        xSemaphoreGive(s_mutex);
    }

    totalWaveformsToCache = 0;
    waveformsCached = 0;
    s_generation.fetch_add(1);
}

bool waveform_cache_clear_if_kit_changed(void* kitPtr, const char* kitPath) {
    if (s_cachedKit == kitPtr && kitPtr != nullptr) {
        return false;   // same kit — reuse
    }

    /* The queue outlives the cache. Requests for the kit we are leaving would
     * otherwise keep the loader busy with files nobody can see any more, and
     * the new kit's own layers hit a full queue and are dropped — 274 of them
     * in one 30-kit sweep, with the current kit at cached=0 throughout. */
    waveform_loader_clear_queue();

    waveform_cache_init();
    s_cachedKit = kitPtr;
    s_savedThisBatch = false;
    if (kitPath && kitPath[0]) {
        snprintf(s_cachedKitPath, sizeof(s_cachedKitPath), "%s", kitPath);
        waveform_cache_load_from_disk(kitPath);
    } else {
        s_cachedKitPath[0] = '\0';
    }

    return true;
}

void waveform_cache_clear() {
    waveform_cache_init();
}

WaveformCacheEntry* waveform_cache_find(const char* filename) {
    if (!filename) return nullptr;
    WaveformCacheEntry* result = nullptr;

    /* A short wait, not a try-lock: the loader holds the mutex only for a
     * memcpy of 1.2 kB, but a try-lock that met it reported "not cached" and
     * the panel drew an empty box for a waveform that was there. */
    if (lock() && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20))) {
        for (int i = 0; i < WAVEFORM_CACHE_SIZE; i++) {
            if (s_cache[i].valid && strcmp(s_cache[i].filename, filename) == 0) {
                s_cache[i].lastUsed = ++s_timestamp;
                result = &s_cache[i];
                break;
            }
        }
        xSemaphoreGive(s_mutex);
    }
    return result;
}

bool waveform_cache_add(const char* filename, int16_t* waveform, uint32_t sampleCount,
                        uint32_t srcFrames, uint32_t srcSampleRate,
                        uint8_t srcChannels, uint8_t srcBitDepth) {
    if (!filename || !waveform || sampleCount == 0) return false;
    bool ok = false;

    if (lock() && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100))) {
        WaveformCacheEntry* entry = nullptr;
        for (int i = 0; i < WAVEFORM_CACHE_SIZE; i++) {
            if (s_cache[i].valid && strcmp(s_cache[i].filename, filename) == 0) {
                entry = &s_cache[i];
                break;
            }
        }

        if (!entry) {
            entry = find_lru_entry();
            if (entry->waveformData) { cache_free(entry->waveformData); entry->waveformData = nullptr; }
        } else if (entry->waveformData) {
            cache_free(entry->waveformData);
            entry->waveformData = nullptr;
        }

        entry->waveformData = (int16_t*)cache_alloc(sampleCount * sizeof(int16_t));
        if (!entry->waveformData) {
            entry->valid = false;
        } else {
            memcpy(entry->waveformData, waveform, sampleCount * sizeof(int16_t));
            snprintf(entry->filename, sizeof(entry->filename), "%s", filename);
            entry->sampleCount = sampleCount;
            entry->srcFrames = srcFrames;
            entry->srcSampleRate = srcSampleRate;
            entry->srcChannels = srcChannels;
            entry->srcBitDepth = srcBitDepth;
            entry->valid = true;
            entry->lastUsed = ++s_timestamp;
            s_generation.fetch_add(1);
            ok = true;
        }
        xSemaphoreGive(s_mutex);
    }
    // Outside the lock: notify can reach save_to_disk, which takes s_mutex
    // itself, and it is not recursive.
    if (ok) waveform_cache_notify_cached();
    return ok;
}

bool waveform_cache_contains(const char* filename) {
    if (!filename) return false;
    bool found = false;
    if (lock() && xSemaphoreTake(s_mutex, 0)) {
        for (int i = 0; i < WAVEFORM_CACHE_SIZE; i++) {
            if (s_cache[i].valid && strcmp(s_cache[i].filename, filename) == 0) {
                found = true;
                break;
            }
        }
        xSemaphoreGive(s_mutex);
    }
    return found;
}

uint32_t waveform_cache_generation() {
    return s_generation.load();
}

void waveform_cache_get_stats(int* loaded, int* total) {
    if (!loaded || !total) return;
    *total = WAVEFORM_CACHE_SIZE;
    *loaded = 0;
    if (lock() && xSemaphoreTake(s_mutex, 0)) {
        for (int i = 0; i < WAVEFORM_CACHE_SIZE; i++) {
            if (s_cache[i].valid) (*loaded)++;
        }
        xSemaphoreGive(s_mutex);
    }
}

void waveform_cache_notify_cached() {
    if (totalWaveformsToCache > 0) waveformsCached++;

    // Persist once per completed batch, so the next boot sees the additions.
    if (!s_savedThisBatch
        && totalWaveformsToCache > 0
        && waveformsCached >= totalWaveformsToCache
        && s_cachedKitPath[0] != '\0') {
        s_savedThisBatch = true;
        waveform_cache_save_to_disk(s_cachedKitPath);
    }
}

// ── Disk persistence — uses platform file callbacks ─────────────────────

static const char CACHE_MAGIC[4] = {'W', 'F', 'C', 'A'};
// v2: full-file envelope (preload uses start=0/end=0). v1 entries were
// cropped to the kit's in/out and would misalign with new markers.
/* 3: entries carry the source file's frames/rate/channels/depth, so the pad
 * editor can draw its markers without reopening the wav. An older file is
 * rejected rather than migrated -- it is a cache, the loader refills it. */
static const uint8_t CACHE_VERSION = 3;

bool waveform_cache_save_to_disk(const char* kitJsonPath) {
    auto& wf = sampler_get_platform_callbacks().waveform;
    if (!wf.writeFile) return false;

    char cachePath[256];
    if (!get_cache_path(kitJsonPath, cachePath, sizeof(cachePath))) return false;

    uint8_t validCount = 0;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500))) {
        for (int i = 0; i < WAVEFORM_CACHE_SIZE; i++) {
            if (s_cache[i].valid && s_cache[i].waveformData) validCount++;
        }
        xSemaphoreGive(s_mutex);
    }
    if (validCount == 0) return false;

    // Blob layout: 8-byte header (4 magic, version, count, 2 reserved), then
    // per entry 1 fnLen + fn + 4 sampleCount + 4 srcFrames + 4 srcSampleRate
    // + 1 srcChannels + 1 srcBitDepth + data.
    uint32_t maxSize = 8;
    for (int i = 0; i < WAVEFORM_CACHE_SIZE; i++) {
        if (s_cache[i].valid && s_cache[i].waveformData) {
            maxSize += 1 + strlen(s_cache[i].filename) + 4 + 10
                     + s_cache[i].sampleCount * sizeof(int16_t);
        }
    }

    uint8_t* buf = (uint8_t*)malloc(maxSize);
    if (!buf) return false;

    uint32_t pos = 0;
    memcpy(buf + pos, CACHE_MAGIC, 4); pos += 4;
    buf[pos++] = CACHE_VERSION;
    buf[pos++] = validCount;
    uint16_t reserved = 0;
    memcpy(buf + pos, &reserved, 2); pos += 2;

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500))) {
        for (int i = 0; i < WAVEFORM_CACHE_SIZE; i++) {
            if (!s_cache[i].valid || !s_cache[i].waveformData) continue;
            uint8_t fnLen = (uint8_t)strlen(s_cache[i].filename);
            buf[pos++] = fnLen;
            memcpy(buf + pos, s_cache[i].filename, fnLen); pos += fnLen;
            memcpy(buf + pos, &s_cache[i].sampleCount, 4); pos += 4;
            memcpy(buf + pos, &s_cache[i].srcFrames, 4); pos += 4;
            memcpy(buf + pos, &s_cache[i].srcSampleRate, 4); pos += 4;
            buf[pos++] = s_cache[i].srcChannels;
            buf[pos++] = s_cache[i].srcBitDepth;
            uint32_t dataBytes = s_cache[i].sampleCount * sizeof(int16_t);
            memcpy(buf + pos, s_cache[i].waveformData, dataBytes); pos += dataBytes;
        }
        xSemaphoreGive(s_mutex);
    }

    bool ok = wf.writeFile(cachePath, buf, pos);
    free(buf);
    return ok;
}

bool waveform_cache_load_from_disk(const char* kitJsonPath) {
    auto& wf = sampler_get_platform_callbacks().waveform;
    if (!wf.readFile) return false;

    char cachePath[256];
    if (!get_cache_path(kitJsonPath, cachePath, sizeof(cachePath))) return false;

    uint32_t maxFile = 256 * 1024;  // 256 KB max
    uint8_t* buf = (uint8_t*)malloc(maxFile);
    if (!buf) return false;

    uint32_t bytesRead = wf.readFile(cachePath, buf, maxFile);
    if (bytesRead < 8) { free(buf); return false; }

    if (memcmp(buf, CACHE_MAGIC, 4) != 0 || buf[4] != CACHE_VERSION) {
        free(buf);
        return false;
    }

    uint8_t entryCount = buf[5];
    uint32_t pos = 8;  // skip header

    int loaded = 0;
    for (int e = 0; e < entryCount && pos < bytesRead; e++) {
        if (pos >= bytesRead) break;
        uint8_t fnLen = buf[pos++];
        if (fnLen == 0 || fnLen > 127 || pos + fnLen > bytesRead) break;

        char filename[128];
        memcpy(filename, buf + pos, fnLen); pos += fnLen;
        filename[fnLen] = '\0';

        if (pos + 4 > bytesRead) break;
        uint32_t sampleCount = 0;
        memcpy(&sampleCount, buf + pos, 4); pos += 4;
        if (sampleCount == 0 || sampleCount > 10000) break;

        if (pos + 10 > bytesRead) break;
        uint32_t srcFrames = 0, srcSampleRate = 0;
        memcpy(&srcFrames, buf + pos, 4); pos += 4;
        memcpy(&srcSampleRate, buf + pos, 4); pos += 4;
        uint8_t srcChannels = buf[pos++];
        uint8_t srcBitDepth = buf[pos++];

        uint32_t dataBytes = sampleCount * sizeof(int16_t);
        if (pos + dataBytes > bytesRead) break;

        int16_t* data = (int16_t*)cache_alloc(dataBytes);
        if (!data) { pos += dataBytes; continue; }

        memcpy(data, buf + pos, dataBytes); pos += dataBytes;

        // Direct insert: notify would move the progress counter.
        if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500))) {
            WaveformCacheEntry* entry = nullptr;
            for (int i = 0; i < WAVEFORM_CACHE_SIZE; i++) {
                if (!s_cache[i].valid) { entry = &s_cache[i]; break; }
            }
            if (!entry) {
                cache_free(data);
            } else {
                snprintf(entry->filename, sizeof(entry->filename), "%s", filename);
                entry->waveformData = data;
                entry->sampleCount = sampleCount;
                entry->srcFrames = srcFrames;
                entry->srcSampleRate = srcSampleRate;
                entry->srcChannels = srcChannels;
                entry->srcBitDepth = srcBitDepth;
                entry->valid = true;
                entry->lastUsed = ++s_timestamp;
                s_generation.fetch_add(1);
                loaded++;
            }
            xSemaphoreGive(s_mutex);
        } else {
            cache_free(data);
        }
    }

    free(buf);
    return loaded > 0;
}

} // namespace crosspad_sampler
