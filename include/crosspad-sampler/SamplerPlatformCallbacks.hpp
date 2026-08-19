// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstddef>

namespace crosspad_sampler {

// ---------------------------------------------------------------------------
// Sample info structure — platform fills this from its audio engine
// ---------------------------------------------------------------------------

struct SampleInfo {
    uint32_t sampleCount    = 0;
    uint32_t sampleRate     = 0;
    uint16_t numberOfChannels = 0;
    uint16_t bitsPerSample  = 0;
};

// ---------------------------------------------------------------------------
// Direct playback callbacks — registered by platform for low-latency
// fingerdrumming response.  When HAS_DIRECT_PLAYBACK is defined the sampler
// calls these instead of (or in addition to) the EventBus path.
//
// The audio engine (e.g. Marcel's ml_sample_stream_player) also calls
// onNoteOn/onNoteOff back INTO the sampler so it can update GUI state
// (playhead, pad colors) with zero EventBus latency.
// ---------------------------------------------------------------------------

/// Callbacks the sampler invokes TO the platform audio engine.
struct SamplerPlayerCallbacks {
    /// Trigger a pad's sample.  velocity=0 means note-off.
    void (*noteOn)(uint8_t padIdx, uint8_t velocity)                                  = nullptr;
    /// Preview a sample file with crop & volume/pan settings.
    void (*previewSample)(const char* path, uint32_t start, uint32_t end,
                          int volume, int pan)                                        = nullptr;
    /// Query sample metadata (sample rate, length, etc.).
    bool (*getSampleInfo)(const char* path, SampleInfo* outInfo)                      = nullptr;
    /// Set playback volume for a pad (0-127).
    void (*setVolume)(uint8_t padIdx, int volume)                                     = nullptr;
    /// Set playback pan for a pad (0-127, 64=center).
    void (*setPan)(uint8_t padIdx, int pan)                                           = nullptr;
    /// Enable looping for a pad (endPos = sample end in samples).
    void (*setLoopEnd)(uint8_t padIdx, uint32_t endPos)                               = nullptr;
    /// Disable looping for a pad.
    void (*clearLoop)(uint8_t padIdx)                                                 = nullptr;
    /// Wipe the sampler engine state for a pad (before re-assigning sample).
    void (*wipeOut)(uint8_t padIdx)                                                   = nullptr;
    /// Return number of free sample slots in the engine.
    int  (*getFreeSlotCount)()                                                        = nullptr;
    /// Provide downsampled waveform data for a pad/slot (for waveform_view).
    /// Fills outData with up to maxSamples PCM values, sets actualSamples.
    void (*getWaveformData)(uint8_t padIdx, uint8_t sampleSlot,
                            int16_t* outData, uint32_t maxSamples,
                            uint32_t* actualSamples)                                  = nullptr;
};

/// Callbacks the audio engine invokes back INTO the sampler (NoteOn/NoteOff
/// from the stream player).  The sampler provides these; the platform hooks
/// them up to the audio engine's callback mechanism.
struct SamplerEngineCallbacks {
    void (*onNoteOn)(uint8_t note, uint8_t wavIdx)   = nullptr;
    void (*onNoteOff)(uint8_t note, uint8_t wavIdx)  = nullptr;
};

// ---------------------------------------------------------------------------
// Audio reload — platform-specific because they hit the audio engine
// (e.g. SampleStreamPlayer_SetupSample, WipeOut, async FreeRTOS task).
// Kit metadata is accessed via PlatformServices (IKitManager).
// ---------------------------------------------------------------------------

struct SamplerAudioCallbacks {
    /// Reload a single pad in the audio engine (after sample assignment changes).
    void (*reloadPad)(uint8_t padIdx)                                                  = nullptr;
    /// Reload all pads in the audio engine.
    void (*reloadPads)()                                                               = nullptr;
    /// Check if a waveform cache file exists for the given kit.
    bool (*hasCacheFile)(int kitId)                                                   = nullptr;
    /// Start async kit loading (platform spawns background task).
    /// kitId = index in IKitManager, onlyModified = reload dirty pads only.
    void (*startLoadKit)(int kitId, bool onlyModified)                                = nullptr;
    /// Called on the UI thread once a background kit load has finished, with
    /// whether it succeeded. The load itself runs off-thread, so this is where
    /// a platform does the follow-up work that must not happen there (cache
    /// top-ups, status messages that need platform state).
    void (*onKitLoadFinished)(bool success)                                            = nullptr;
};

// ---------------------------------------------------------------------------
// Waveform loading — platform provides file info + raw PCM reading.
// The cache and loader live in crosspad-sampler; only I/O is platform-specific.
// ---------------------------------------------------------------------------

/// Audio file metadata (returned by getFileInfo).
struct AudioFileInfo {
    uint32_t sampleCount = 0;
    uint32_t sampleRate  = 0;
    uint8_t  channels    = 0;
    uint8_t  bitDepth    = 0;
};

struct SamplerWaveformCallbacks {
    /// Get metadata for an audio file (sample count, rate, channels, etc.).
    bool (*getFileInfo)(const char* path, AudioFileInfo* outInfo)                     = nullptr;

    /// Read downsampled/compressed waveform data from a file range.
    bool (*getWaveformSamples)(const char* path, int16_t* outBuf,
                               uint32_t outSamples,
                               uint32_t startSample, uint32_t endSample)             = nullptr;

    /// Save binary data to a file path. Returns true on success.
    bool (*writeFile)(const char* path, const uint8_t* data, uint32_t size)          = nullptr;

    /// Read binary data from a file path into outBuf (up to maxSize bytes).
    uint32_t (*readFile)(const char* path, uint8_t* outBuf, uint32_t maxSize)        = nullptr;

    /// Allocate memory for waveform cache (platform routes to PSRAM if available).
    void* (*allocWaveformMem)(uint32_t bytes)                                        = nullptr;

    /// Free memory allocated by allocWaveformMem.
    void  (*freeWaveformMem)(void* ptr)                                              = nullptr;
};

// ---------------------------------------------------------------------------
// Kit selector app callbacks — platform-specific actions around kit loading.
// ---------------------------------------------------------------------------

struct KitSelectorAppCallbacks {
    /// Called before a kit is loaded (e.g. stop sequencer playback).
    void (*onBeforeKitLoad)(int kitId)  = nullptr;
    /// Called after a kit has loaded (e.g. signal orchestrator).
    void (*onKitLoaded)()               = nullptr;
};

// ---------------------------------------------------------------------------
// Aggregate — all platform callbacks in one struct.
// Platform calls sampler_set_platform_callbacks() once at init time.
// ---------------------------------------------------------------------------

struct SamplerPlatformCallbacks {
    SamplerPlayerCallbacks      player;
    SamplerAudioCallbacks       audio;
    SamplerWaveformCallbacks    waveform;
    KitSelectorAppCallbacks     kitSelector;
};

/// Register the platform callbacks.  Must be called before sampler_panel_create().
void sampler_set_platform_callbacks(const SamplerPlatformCallbacks& cb);

/// Retrieve the currently registered platform callbacks.
const SamplerPlatformCallbacks& sampler_get_platform_callbacks();

/// Register the engine-to-sampler callbacks (NoteOn/NoteOff from audio engine).
SamplerEngineCallbacks sampler_get_engine_callbacks();

} // namespace crosspad_sampler
