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

/// Start the background kit load. Must be called from the LVGL thread.
///
/// Requesting a load while one is running does not fail and is not dropped:
/// the request is held and started when the running load reports. Only the
/// most recent one is kept, so spinning through the selector lands on the kit
/// you stopped at rather than replaying every kit you passed.
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

/// Kit id held back until the running load finishes, or -1 if none is waiting.
int kit_load_task_pending_kit();

} // namespace crosspad_sampler
