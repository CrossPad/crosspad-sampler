// SPDX-License-Identifier: MIT
#include "crosspad-sampler/SamplerUIContext.hpp"

namespace crosspad_sampler {

static SamplerUIContext s_uiCtx = {};

void sampler_set_ui_context(const SamplerUIContext& ctx) {
    s_uiCtx = ctx;
}

const SamplerUIContext& sampler_get_ui_context() {
    return s_uiCtx;
}

} // namespace crosspad_sampler
