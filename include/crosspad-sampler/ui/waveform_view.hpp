// SPDX-License-Identifier: MIT
#pragma once

#ifdef USE_LVGL

#include <cstdint>
#include <lvgl.h>

namespace crosspad_sampler {

/// Callback used to fill a buffer with PCM sample data for a given pad/slot.
/// @param padIdx       Index of the pad (0–15).
/// @param sampleSlot   Sample slot index within the pad.
/// @param outData      Output buffer to fill with 16-bit signed PCM samples.
/// @param maxSamples   Capacity of outData (number of int16_t elements).
/// @param actualSamples Set to the number of samples written into outData.
/// @param userData     Opaque pointer passed to waveform_view_set_provider().
using WaveformDataProvider = void (*)(uint8_t padIdx, uint8_t sampleSlot,
                                      int16_t* outData, uint32_t maxSamples,
                                      uint32_t* actualSamples, void* userData);

/// Register the data provider used by waveform_view_load().
/// Must be called before any waveform_view_load() call.
/// @param fn       Provider callback (may be nullptr to disable loading).
/// @param userData Opaque pointer forwarded to fn on each call.
void waveform_view_set_provider(WaveformDataProvider fn, void* userData);

/// Callback used to fill a buffer with PCM data for a file named by path.
/// The counterpart of WaveformDataProvider for views that are not showing a
/// pad: a pitched kit's zones are files of the kit, not slots of a pad.
/// @param path          Resolved path of the audio file.
/// @param outData       Output buffer to fill with 16-bit signed PCM samples.
/// @param maxSamples    Capacity of outData (number of int16_t elements).
/// @param actualSamples Set to the number of samples written into outData.
/// @param userData      Opaque pointer passed to waveform_view_set_file_provider().
using WaveformFileProvider = void (*)(const char* path, int16_t* outData,
                                      uint32_t maxSamples, uint32_t* actualSamples,
                                      void* userData);

/// Register the provider used by waveform_view_load_file().
/// @param fn       Provider callback (may be nullptr to disable loading).
/// @param userData Opaque pointer forwarded to fn on each call.
void waveform_view_set_file_provider(WaveformFileProvider fn, void* userData);

/// Create a waveform view widget inside @p parent.
/// The view is WAVEFORM_CANVAS_WIDTH × WAVEFORM_CANVAS_HEIGHT pixels and
/// contains (by child index):
///   0 – lv_canvas  for drawing waveform lines
///   1 – playhead   (2 px wide red vertical line, initially hidden)
///   2 – start marker (2 px wide green vertical line, initially hidden)
///   3 – end marker   (2 px wide red vertical line, initially hidden)
/// @return Pointer to the container lv_obj_t.
lv_obj_t* waveform_view_create(lv_obj_t* parent);

/// Load and render the waveform for the given pad/slot.
/// Calls the registered WaveformDataProvider and redraws the canvas.
void waveform_view_load(lv_obj_t* waveformView, uint8_t padIdx, uint8_t sampleSlot);

/// Load and render the waveform of @p path via the registered
/// WaveformFileProvider. Does nothing without one.
void waveform_view_load_file(lv_obj_t* waveformView, const char* path);

/// Clear the canvas to the background color and hide all markers.
void waveform_view_clear(lv_obj_t* waveformView);

/// Position the start/end trim markers.
/// @param startNorm  Normalised start position in [0.0, 1.0].
/// @param endNorm    Normalised end   position in [0.0, 1.0].
void waveform_view_set_markers(lv_obj_t* waveformView, float startNorm, float endNorm);

/// Position the playhead indicator.
/// @param position Normalised playback position in [0.0, 1.0].
void waveform_view_set_playhead(lv_obj_t* waveformView, float position);

} // namespace crosspad_sampler

#endif // USE_LVGL
