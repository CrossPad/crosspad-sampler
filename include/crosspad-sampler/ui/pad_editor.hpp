// SPDX-License-Identifier: MIT
#pragma once

#ifdef USE_LVGL

#include <cstdint>
#include <lvgl.h>

namespace crosspad_sampler {

/// Create a full-screen pad parameter editor overlay inside @p parent.
/// @param parent  Parent LVGL object (typically the screen or app container).
/// @param padIdx  Index of the pad to edit (0–15).
/// @return Pointer to the top-level editor lv_obj_t.
lv_obj_t* pad_editor_create(lv_obj_t* parent, uint8_t padIdx);

/// Destroy the pad editor and release all UI state.
/// @param editor The lv_obj_t returned by pad_editor_create().
void pad_editor_destroy(lv_obj_t* editor);

/// Switch the editor to a different pad without recreating the widget tree.
/// Refreshes all spinbox values, the sample list, and the waveform view.
/// @param editor  The lv_obj_t returned by pad_editor_create().
/// @param padIdx  New pad index to display (0–15).
void pad_editor_set_pad(lv_obj_t* editor, uint8_t padIdx);

} // namespace crosspad_sampler

#endif // USE_LVGL
