// SPDX-License-Identifier: MIT
#pragma once

#ifdef USE_LVGL

#include <lvgl.h>

namespace crosspad_sampler {

/// Create the editor overlay for a `KitEngine::Pitched` kit inside @p parent.
///
/// The pitched counterpart of pad_editor_create(): a pitched kit has no
/// per-pad sample to trim, so what it edits is the pad's note/volume/pan on
/// one tab and the instrument's zones and envelope on the other. Edits apply
/// to the bank that is playing; nothing reaches the card until Save.
///
/// @return the overlay's root object, or nullptr when the current kit is not
///         a pitched one.
lv_obj_t* pitched_editor_create(lv_obj_t* parent);

/// Tear the editor down. Safe when nothing is open.
void pitched_editor_destroy();

} // namespace crosspad_sampler

#endif // USE_LVGL
