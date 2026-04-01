// SPDX-License-Identifier: MIT
#pragma once

#ifdef USE_LVGL
#include <lvgl.h>
#endif

namespace crosspad_sampler {

/// Platform-provided UI context for sampler components that need
/// styles not available through crosspad-gui's standard headers.
/// Encoder access uses crosspad_gui::getGuiPlatform().getNavigationEncoder().
struct SamplerUIContext {
    // Currently empty — all styles come from crosspad-gui/styles/styles.h
    // and encoder comes from IGuiPlatform. This struct exists as an
    // extension point for future platform-specific UI needs.
};

/// Register the platform UI context. Call at init alongside sampler_set_platform_callbacks().
void sampler_set_ui_context(const SamplerUIContext& ctx);

/// Retrieve the registered UI context.
const SamplerUIContext& sampler_get_ui_context();

} // namespace crosspad_sampler
