// SPDX-License-Identifier: MIT

#include "crosspad-sampler/waveform/waveform_cache.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"
#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>

namespace crosspad_sampler {

// ---------------------------------------------------------------------------
// Progress tracking
// ---------------------------------------------------------------------------

int totalWaveformsToCache = 0;
int waveformsCached       = 0;

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static WaveformCacheEntry s_cache[WAVEFORM_CACHE_SIZE] = {};
static uint32_t           s_timestamp        = 0;
static void*              s_cachedKit        = nullptr;
static char               s_cachedKitPath[256] = {};
static bool               s_savedThisBatch   = false;
static SemaphoreHandle_t  s_mutex            = nullptr;

// ---------------------------------------------------------------------------
// Memory helpers — route through platform callbacks if available
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// LRU helper
// ---------------------------------------------------------------------------

static WaveformCacheEntry* find_lru_entry() {
    // Prefer an invalid (empty) slot
    for (int i = 0; i < WAVEFORM_CACHE_SIZE; i++) {
        if (!s_cache[i].valid) return &s_cache[i];
    }
    // All valid — find least recently used
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

// ---------------------------------------------------------------------------
// Disk cache path helper
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void waveform_cache_init() {
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return;
    }

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
}

bool waveform_cache_clear_if_kit_changed(void* kitPtr, const char* kitPath) {
    if (s_cachedKit == kitPtr && kitPtr != nullptr) {
        return false;   // same kit — reuse
    }

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

    // Non-blocking try-lock to avoid stalling UI
    if (s_mutex && xSemaphoreTake(s_mutex, 0)) {
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

bool waveform_cache_add(const char* filename, int16_t* waveform, uint32_t sampleCount) {
    if (!filename || !waveform || sampleCount == 0) return false;
    bool ok = false;

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100))) {
        // Already cached?
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
            entry->valid = true;
            entry->lastUsed = ++s_timestamp;
            ok = true;

            waveform_cache_notify_cached();
        }
        xSemaphoreGive(s_mutex);
    }
    return ok;
}

void waveform_cache_get_stats(int* loaded, int* total) {
    if (!loaded || !total) return;
    *total = WAVEFORM_CACHE_SIZE;
    *loaded = 0;
    if (s_mutex && xSemaphoreTake(s_mutex, 0)) {
        for (int i = 0; i < WAVEFORM_CACHE_SIZE; i++) {
            if (s_cache[i].valid) (*loaded)++;
        }
        xSemaphoreGive(s_mutex);
    }
}

void waveform_cache_notify_cached() {
    if (totalWaveformsToCache > 0) waveformsCached++;

    // Persist once when a top-up batch completes so the disk cache
    // covers the newly added entries on next boot.
    if (!s_savedThisBatch
        && totalWaveformsToCache > 0
        && waveformsCached >= totalWaveformsToCache
        && s_cachedKitPath[0] != '\0') {
        s_savedThisBatch = true;
        waveform_cache_save_to_disk(s_cachedKitPath);
    }
}

// ---------------------------------------------------------------------------
// Disk persistence — uses platform file callbacks
// ---------------------------------------------------------------------------

static const char CACHE_MAGIC[4] = {'W', 'F', 'C', 'A'};
// v2: full-file envelope (preload uses start=0/end=0). v1 entries were
// cropped to the kit's in/out and would misalign with new markers.
static const uint8_t CACHE_VERSION = 2;

bool waveform_cache_save_to_disk(const char* kitJsonPath) {
    auto& wf = sampler_get_platform_callbacks().waveform;
    if (!wf.writeFile) return false;

    char cachePath[256];
    if (!get_cache_path(kitJsonPath, cachePath, sizeof(cachePath))) return false;

    // Count valid entries
    uint8_t validCount = 0;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500))) {
        for (int i = 0; i < WAVEFORM_CACHE_SIZE; i++) {
            if (s_cache[i].valid && s_cache[i].waveformData) validCount++;
        }
        xSemaphoreGive(s_mutex);
    }
    if (validCount == 0) return false;

    // Build the blob in memory — entries are small (600 samples * 2 bytes each)
    // Header: 4 magic + 1 version + 1 count + 2 reserved = 8 bytes
    // Per entry: 1 fnLen + fn + 4 sampleCount + data
    uint32_t maxSize = 8;
    for (int i = 0; i < WAVEFORM_CACHE_SIZE; i++) {
        if (s_cache[i].valid && s_cache[i].waveformData) {
            maxSize += 1 + strlen(s_cache[i].filename) + 4 + s_cache[i].sampleCount * sizeof(int16_t);
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

    // Read entire file
    uint32_t maxFile = 256 * 1024;  // 256 KB max
    uint8_t* buf = (uint8_t*)malloc(maxFile);
    if (!buf) return false;

    uint32_t bytesRead = wf.readFile(cachePath, buf, maxFile);
    if (bytesRead < 8) { free(buf); return false; }

    // Verify header
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

        uint32_t dataBytes = sampleCount * sizeof(int16_t);
        if (pos + dataBytes > bytesRead) break;

        int16_t* data = (int16_t*)cache_alloc(dataBytes);
        if (!data) { pos += dataBytes; continue; }

        memcpy(data, buf + pos, dataBytes); pos += dataBytes;

        // Insert directly (bypassing notify to avoid progress counter increments)
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
                entry->valid = true;
                entry->lastUsed = ++s_timestamp;
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
