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

/// Start the background kit load.
///
/// @param onlyModified  reload only pads the kit marks as modified.
/// @param kitId         when >= 0, the task runs KitOrchestrator::changeKit()
///                      itself before touching the pads. That call parses the
///                      kit JSON off storage and takes seconds on a cold SD
///                      card, so running it on the caller's thread froze the
///                      UI long enough to trip the task watchdog. Pass -1 when
///                      a kit is already current and only the pads need work.
void kit_load_task_start(bool onlyModified, int kitId = -1);

/// True while a background load task is running.
bool kit_load_task_is_loading();

} // namespace crosspad_sampler
