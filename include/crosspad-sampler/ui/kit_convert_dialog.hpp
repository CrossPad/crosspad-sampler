// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

namespace crosspad_sampler {

/**
 * @brief The Sampler's side of a kit rate conversion.
 *
 * Called from the panel's timer. Once per kit -- after it has loaded and the
 * engine can say what rates it read -- a kit whose samples are not at the
 * engine's rate is offered a conversion: Convert, Not now, or Never (the kit
 * says `keep_rate` from then on). A conversion running, whoever started it,
 * shows its progress with a way to stop it.
 *
 * @p panelGroup is the encoder group the dialogs give back.
 */
void kit_convert_dialog_tick(lv_group_t* panelGroup);

/// The panel is going: close what this put up. A conversion carries on.
void kit_convert_dialog_close();

} // namespace crosspad_sampler
