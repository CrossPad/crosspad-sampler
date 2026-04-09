// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace crosspad_sampler {

/// Initialize kit load task resources. Call once at startup.
/// On platforms using lv_async_call this is a no-op.
void kit_load_task_init();

/// Poll for kit load progress updates. Call from LVGL loop.
/// On platforms using lv_async_call this is a no-op.
void kit_load_task_poll();

/// Start async audio sample loading for the current kit.
/// KitOrchestrator::changeKit() must be called first (JSON parse, colors).
void kit_load_task_start(bool onlyModified);

/// True while a background load task is running.
bool kit_load_task_is_loading();

} // namespace crosspad_sampler
