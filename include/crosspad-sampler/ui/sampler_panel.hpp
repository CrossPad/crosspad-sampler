// SPDX-License-Identifier: MIT
#pragma once

#ifdef USE_LVGL

#include <lvgl.h>
#include <crosspad-sampler/SamplerPadLogic.hpp>
#include <crosspad-sampler/logic/pad_color_manager.hpp>
#include <crosspad-sampler/logic/kit_orchestrator.hpp>

namespace crosspad_sampler {

/// Create the main sampler panel inside @p parent.
/// Initialises static logic objects, builds the UI, registers pad logic,
/// and starts the periodic update timer.
/// @return Pointer to the top-level lv_obj_t for the panel.
lv_obj_t* sampler_panel_create(lv_obj_t* parent);

/// Destroy the sampler panel previously created by sampler_panel_create().
/// Stops the timer, clears UI state, unregisters pad logic, and
/// asynchronously deletes the panel object.
/// @param panel The lv_obj_t returned by sampler_panel_create().
void sampler_panel_destroy(lv_obj_t* panel);

/// Access the shared SamplerPadLogic instance.
SamplerPadLogic& getSamplerPadLogic();

/// Access the shared PadColorManager instance.
PadColorManager& getPadColorManager();

/// Access the shared KitOrchestrator instance.
KitOrchestrator& getKitOrchestrator();

/// Called by the platform's audio engine NoteOn callback to update GUI state
/// (playhead timing, sample index tracking). PadManager/EventBus updates are
/// handled platform-side; this is GUI-only.
void sampler_on_engine_note_on(uint8_t note, uint8_t wavIdx);

/// Called by the platform's audio engine NoteOff callback to update GUI state.
void sampler_on_engine_note_off(uint8_t note, uint8_t wavIdx);

} // namespace crosspad_sampler

#endif // USE_LVGL
