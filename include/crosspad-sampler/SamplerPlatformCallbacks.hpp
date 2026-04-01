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
};

/// Callbacks the audio engine invokes back INTO the sampler (NoteOn/NoteOff
/// from the stream player).  The sampler provides these; the platform hooks
/// them up to the audio engine's callback mechanism.
struct SamplerEngineCallbacks {
    void (*onNoteOn)(uint8_t note, uint8_t wavIdx)   = nullptr;
    void (*onNoteOff)(uint8_t note, uint8_t wavIdx)  = nullptr;
};

// ---------------------------------------------------------------------------
// Kit operations — things that are platform-specific because they hit the
// filesystem or the platform's kit loader implementation.
// ---------------------------------------------------------------------------

struct SamplerKitCallbacks {
    /// Resolve a relative sample path to the full filesystem path.
    /// Returns a pointer to an internal buffer valid until the next call.
    const char* (*getFullPath)(const char* sampleRelPath)                              = nullptr;
    /// Save the current kit to persistent storage.
    void (*saveKit)()                                                                  = nullptr;
    /// Reload the current kit from persistent storage (discard in-memory edits).
    void (*loadKit)()                                                                  = nullptr;
    /// Reload a single pad in the audio engine (after sample assignment changes).
    void (*reloadPad)(uint8_t padIdx)                                                  = nullptr;
    /// Reload all pads in the audio engine.
    void (*reloadPads)()                                                               = nullptr;
};

// ---------------------------------------------------------------------------
// File browsing — platform provides a way to open a file browser.
// The callback receives the selected file path or nullptr on cancel.
// ---------------------------------------------------------------------------

using FileSelectedCallback = void (*)(const char* filePath, void* userData);

struct SamplerFileCallbacks {
    /// Open a file browser rooted at basePath.  onSelected is called with
    /// the chosen file path (or nullptr on cancel).
    void (*openFileBrowser)(const char* basePath,
                            FileSelectedCallback onSelected,
                            void* userData)                                            = nullptr;
};

// ---------------------------------------------------------------------------
// Aggregate — all platform callbacks in one struct.
// Platform calls sampler_set_platform_callbacks() once at init time.
// ---------------------------------------------------------------------------

struct SamplerPlatformCallbacks {
    SamplerPlayerCallbacks player;
    SamplerKitCallbacks    kit;
    SamplerFileCallbacks   file;
};

/// Register the platform callbacks.  Must be called before sampler_panel_create().
void sampler_set_platform_callbacks(const SamplerPlatformCallbacks& cb);

/// Retrieve the currently registered platform callbacks.
const SamplerPlatformCallbacks& sampler_get_platform_callbacks();

/// Register the engine-to-sampler callbacks (NoteOn/NoteOff from audio engine).
/// The sampler populates these; the platform should wire them into
/// SampleStreamPlayer_NoteOnCb / SampleStreamPlayer_NoteOffCb.
SamplerEngineCallbacks sampler_get_engine_callbacks();

} // namespace crosspad_sampler
