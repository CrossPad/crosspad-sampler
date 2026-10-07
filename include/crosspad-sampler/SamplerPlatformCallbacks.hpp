// SPDX-License-Identifier: MIT
#pragma once

#include <crosspad/pad/IPadMix.hpp>
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

    /// Pull the newest mono samples rendered by the ordinary 16-pad engine.
    /// This is deliberately a snapshot callback instead of exposing a
    /// platform-owned ScopeTap to the portable sampler UI.
    uint32_t (*getLiveScopeData)(float* outData, uint32_t maxSamples,
                                 float* fundamentalHz,
                                 uint32_t* sampleRate)                                = nullptr;
    /// Start/stop the engine-side scope tap. Disabled means no audio copy.
    void (*setLiveScopeEnabled)(bool enabled)                                         = nullptr;

    /**
     * @brief Does the engine play this combination of effects itself?
     *
     * The pad editor renders a copy of the sample with the effects applied and
     * points the pad at that copy, because the engine it was written against
     * can do none of them. An engine that transposes in the voice does not
     * want the copy -- it wants the original and a step -- and asking for one
     * anyway is a write to the card and a reload on every turn of the knob.
     *
     * Null, or false, keeps the copy. Scalars rather than SampleFx so the
     * answer stays cheap to ask per redraw.
     */
    bool (*engineHandlesFx)(int semitones, uint16_t hpHz, uint16_t lpHz, bool reverse) = nullptr;

    /**
     * @brief Set a pad's step now, with no reload and no card.
     *
     * A pad's pitch in an engine that transposes in the voice is one number.
     * Nothing has to be read, nothing re-parsed, nothing rebuilt -- so the
     * editor can apply it on the detent instead of debouncing for 400 ms and
     * then rebuilding the pad, which is what a rendered copy needed.
     *
     * Voices already sounding keep the step they started with: a knob turned
     * over a ringing note should not retune that note.
     *
     * False when the engine cannot, and the editor falls back to a reload.
     */
    bool (*setPadPitch)(uint8_t padIdx, int semitones)                                 = nullptr;
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

/// Engine-side counters a kit load is judged by. The load task reads them
/// before and after the pad loop and stores the difference in its swap log
/// (kit_load_task_swap_log()), so a swap's audible cost is on the record.
/// Cumulative unless said otherwise; a platform without an engine leaves the
/// hook null.
struct EngineLoadStats {
    uint32_t gatedBlocks = 0;   ///< render blocks the engine had to skip (silence)
    uint32_t gateMaxUs   = 0;   ///< longest single render-gate hold since the previous call
    uint32_t gateTotalUs = 0;
    uint32_t gateCount   = 0;
    uint32_t underruns   = 0;   ///< streamer refills that came too late
    uint32_t noFreeBlock = 0;   ///< voices refused a stream block
    uint32_t droppedHits = 0;   ///< pad hits refused because their pad was mid-reload
    uint32_t rtGapMaxUs  = 0;   ///< longest wait between two render passes since the previous call
    uint32_t lateHits    = 0;   ///< hits held during their pad's reload and played after it
    uint32_t streamFopens = 0;  ///< file opens the streamer could not serve from its handle cache
    uint32_t bgPassMaxUs = 0;   ///< longest single streamer refill pass since the previous call
    uint32_t fopenMaxUs  = 0;   ///< slowest file open on the card since the previous call
    uint32_t streamKb    = 0;   ///< kilobytes the streamer read off the card
    uint32_t loaderKb    = 0;   ///< kilobytes the loader read off the card
};

struct SamplerAudioCallbacks {
    /// Reload a single pad in the audio engine (after sample assignment changes).
    void (*reloadPad)(uint8_t padIdx)                                                  = nullptr;
    /// Like reloadPad, but only if the pad has no voice by the time the engine
    /// would be restructured — the platform prepares the files first, and a
    /// hit that lands meanwhile plays the old sample to its end. Returns false
    /// when it did not reload; the load task comes back for that pad later.
    /// Null: reloadPad is used unconditionally.
    bool (*reloadPadIfIdle)(uint8_t padIdx)                                            = nullptr;
    /// Fill the engine counters above; see kit_load_task_swap_log().
    void (*engineLoadStats)(EngineLoadStats* out)                                      = nullptr;
    /// True while any voice of this pad is still rendering. The kit load task
    /// reloads a pad only once this is false (or forces it, last of all), so
    /// the engine is never restructured under a voice it is playing. Null =
    /// fall back to PadManager::isPadPlaying().
    bool (*padSounding)(uint8_t padIdx)                                                = nullptr;
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
    /// Called once the selector app has been torn down, whether it closed
    /// because a kit finished loading or because the user backed out. The
    /// platform that opened it as a gate in front of another app launches
    /// that app from here.
    void (*onClosed)()                  = nullptr;
    /// Open the selector over a running app and come back to that app when it
    /// closes. Until this existed the selector was reachable only as a gate in
    /// front of a launch, so once a kit was loaded there was no way back to it
    /// at all — the sampler's own pick-kit button was a placeholder for exactly
    /// that reason. Null on a platform with no selector.
    void (*openSelector)()              = nullptr;
    /// The same selector, opened by the power button's back from the sampler:
    /// backing out of it goes on to the launcher. Returning to the sampler
    /// there would bounce sampler -> selector -> sampler with no way out.
    /// Null falls back to openSelector.
    void (*openSelectorForBack)()       = nullptr;
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

/// The pads' level and pan in whichever engine plays the current kit: the
/// pitched instrument for a pitched kit, the platform's sample player
/// (player.setVolume / setPan) otherwise. sampler_service_init() puts it in
/// PlatformServices::padMix.
crosspad::IPadMix& sampler_pad_mix();

/// Register the engine-to-sampler callbacks (NoteOn/NoteOff from audio engine).
SamplerEngineCallbacks sampler_get_engine_callbacks();

} // namespace crosspad_sampler
