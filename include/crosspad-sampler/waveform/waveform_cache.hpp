// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstdbool>
#include <cstring>

namespace crosspad_sampler {

// Forward — actual value comes from SamplerConfig.hpp but we don't want the
// mega-include here.  The .cpp picks up the real constant.
#ifndef CROSSPAD_SAMPLER_WAVEFORM_CACHE_SIZE
#define CROSSPAD_SAMPLER_WAVEFORM_CACHE_SIZE 17
#endif

// ---------------------------------------------------------------------------
// Cache entry — one per waveform slot
// ---------------------------------------------------------------------------

struct WaveformCacheEntry {
    char      filename[128] = {};
    int16_t*  waveformData  = nullptr;   // Platform-allocated (PSRAM or heap)
    uint32_t  sampleCount   = 0;
    bool      valid         = false;
    uint32_t  lastUsed      = 0;         // LRU timestamp
};

// ---------------------------------------------------------------------------
// Progress tracking (shared with UI)
// ---------------------------------------------------------------------------

extern int totalWaveformsToCache;
extern int waveformsCached;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Initialise / clear the waveform cache.
void waveform_cache_init();

/// Clear cache only if kit changed (by comparing pointer).
/// Also attempts to load disk cache for the new kit.
/// Returns true if cache was cleared.
bool waveform_cache_clear_if_kit_changed(void* kitPtr, const char* kitPath);

/// Clear entire cache.
void waveform_cache_clear();

/// Thread-safe lookup.  Updates LRU timestamp on hit.
WaveformCacheEntry* waveform_cache_find(const char* filename);

/// Thread-safe insert.  Uses LRU eviction when full.
bool waveform_cache_add(const char* filename, int16_t* waveform, uint32_t sampleCount);

/// Statistics.
void waveform_cache_get_stats(int* loaded, int* total);

/// Increment progress counter (called after successful cache_add).
void waveform_cache_notify_cached();

/// Disk persistence — save / load all valid entries.
bool waveform_cache_save_to_disk(const char* kitJsonPath);
bool waveform_cache_load_from_disk(const char* kitJsonPath);

} // namespace crosspad_sampler
