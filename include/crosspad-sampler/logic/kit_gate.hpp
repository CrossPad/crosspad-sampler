// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file kit_gate.hpp
 * @brief The kit selector in front of an app that needs a kit.
 *
 * An app that plays the kit (Sampler, Sequencer) launched with no kit loaded
 * -- unless it opens for a song's second kit track, which plays a kit of its
 * own -- gets the kit selector first, and starts once a kit has loaded; backing out
 * of the selector starts nothing. The same selector opens over a running app
 * (the sampler's pick-kit button, its back) and brings that app back when it
 * closes. The gate also keeps the two most recent screens, the selector
 * counting as one, for the recents gesture.
 *
 * Everything here runs on the LVGL thread: the orchestrator's pre-launch
 * hook, the selector's callbacks, the gesture, the launcher rebuild.
 */

#ifdef USE_LVGL

#include <crosspad-sampler/SamplerPlatformCallbacks.hpp>
#include <lvgl.h>

namespace crosspad_gui { class ILvglApp; }

namespace crosspad_sampler {

/// What the gate needs from the platform.
struct KitGateHost {
    /// The container apps start in: the current launcher's. Null before the
    /// first main screen.
    lv_obj_t* (*appContainer)() = nullptr;
    /// False while the card is still being scanned for kits (the list the
    /// selector would show is still growing). Null: the list is always whole.
    bool (*kitScanDone)() = nullptr;
    /// False when no card is mounted. Null: a card is always there.
    bool (*cardPresent)() = nullptr;
    /// One line of the platform log, e.g. "kit gate: scanning". Null: silent.
    void (*log)(const char* line) = nullptr;
    /// Icon path of the selector overlay.
    const char* selectorIcon = "notepick.png";
};

/// Keep @p host and fill @p cb with the gate's selector callbacks (onKitLoaded,
/// onClosed, openSelector, openSelectorForBack); the platform passes @p cb on
/// in its SamplerPlatformCallbacks. Also makes the selector the platform's
/// kit picker (PlatformServices::kitPicker, core IKitPicker): an app's own
/// kit, picked over it and handed back instead of loaded.
void kit_gate_install(const KitGateHost& host, KitSelectorAppCallbacks& cb);

/// The orchestrator's pre-launch hook (OrchestratorConfig::pre_launch).
/// @return true when @p app may start now.
bool kit_gate_pre_launch(crosspad_gui::ILvglApp* app, lv_obj_t* app_container);

/// The recents gesture: go to the most recent screen that is not the one
/// showing. @return false when there is none.
bool kit_gate_previous_screen();

/// The launcher is being rebuilt: the rebuild cleans the whole screen, the
/// selector overlay included, so a launch waiting behind the gate is dropped.
void kit_gate_reset();

/// lv_async callback after a kit scan: rebuild an open selector's list.
void kit_gate_refresh_if_open(void*);

} // namespace crosspad_sampler

#endif // USE_LVGL
