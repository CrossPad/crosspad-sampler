// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef USE_LVGL

#include "lvgl.h"
#include <crosspad/app/AppEntry.hpp>

namespace crosspad_sampler {

/// Register the Sampler application with the global AppRegistry.
void sampler_register();

/// Make the sampler the system instrument: its pad logic is registered for the
/// process lifetime and installed as the PadManager base, so pads keep playing
/// samples everywhere — the launcher, settings, any app that does not claim the
/// pads. Instrument apps preempt it via pushPadLogic() and hand it back on
/// teardown. Call once during platform bootstrap, independently of whether the
/// sampler app is ever opened.
void sampler_service_init();

/// Return the AppEntry descriptor for the Sampler application.
crosspad::AppEntry sampler_get_entry();

} // namespace crosspad_sampler

#endif // USE_LVGL
