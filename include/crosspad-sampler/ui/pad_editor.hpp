// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef USE_LVGL

#include <cstdint>
#include <lvgl.h>
#include "crosspad-gui/app/ILvglApp.h"

namespace crosspad_sampler {

/// Create the pad editor inside @p parent: one pad's sample, trim and
/// settings, laid out as the Recorder is (see pad_editor.cpp).
/// @param parent  Parent LVGL object (typically the screen or app container).
/// @param app     The LVGL app instance (the file browser's parent), or nullptr.
/// @return Pointer to the top-level editor lv_obj_t.
lv_obj_t* pad_editor_create(lv_obj_t* parent, crosspad_gui::ILvglApp* app);

/// Destroy the pad editor and release all UI state.
/// @param editor The lv_obj_t returned by pad_editor_create().
void pad_editor_destroy(lv_obj_t* editor);

/// Switch the editor to a different pad without recreating the widget tree;
/// its settings, sample and picture follow on the editor's next tick.
/// @param editor  The lv_obj_t returned by pad_editor_create().
/// @param padIdx  New pad index to display (0-15).
void pad_editor_set_pad(lv_obj_t* editor, uint8_t padIdx);

} // namespace crosspad_sampler

#endif // USE_LVGL
