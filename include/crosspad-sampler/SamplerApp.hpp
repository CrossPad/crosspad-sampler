// SPDX-License-Identifier: MIT
#pragma once

#ifdef USE_LVGL

#include <crosspad/app/AppEntry.hpp>

namespace crosspad_sampler {

/// Register the Sampler application with the global AppRegistry.
void sampler_register();

/// Return the AppEntry descriptor for the Sampler application.
crosspad::AppEntry sampler_get_entry();

} // namespace crosspad_sampler

#endif // USE_LVGL
