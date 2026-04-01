// SPDX-License-Identifier: MIT
#pragma once

#ifdef USE_LVGL

#include <cstdint>
#include <lvgl.h>

namespace crosspad_sampler {

/// Callback used to list directory entries for a given path.
/// Implementations must call onResult exactly once, synchronously or
/// asynchronously, with the resulting entries and count.
/// @param path     Directory path to list.
/// @param onResult Callback to invoke with the results.
/// @param ctx      Opaque context pointer forwarded to onResult.
using FileListProvider = void (*)(const char* path,
                                   void (*onResult)(const char** entries, uint32_t count, void* ctx),
                                   void* ctx);

/// Callback invoked when the user selects a file.
/// @param filePath  Full path of the selected file.
/// @param userData  Opaque pointer passed to file_explorer_set_callback().
using FileSelectedFn = void (*)(const char* filePath, void* userData);

/// Register the directory listing provider used by file_explorer_create().
/// @param fn Provider callback (may be nullptr to disable file listing).
void file_explorer_set_provider(FileListProvider fn);

/// Register the selection callback and its opaque user data.
/// @param fn       Callback invoked when a file is selected (may be nullptr).
/// @param userData Opaque pointer forwarded to fn on each call.
void file_explorer_set_callback(FileSelectedFn fn, void* userData);

/// Create a full-screen file explorer overlay inside @p parent.
/// If a FileListProvider has been set, it is called immediately with
/// @p initialPath to populate the file list.
/// @param parent      Parent LVGL object.
/// @param initialPath Initial directory path to display.
/// @return Pointer to the root lv_obj_t of the explorer.
lv_obj_t* file_explorer_create(lv_obj_t* parent, const char* initialPath);

/// Destroy the file explorer created by file_explorer_create().
/// Uses lv_obj_delete_async so it is safe to call from within an event handler.
/// @param explorer Object returned by file_explorer_create().
void file_explorer_destroy(lv_obj_t* explorer);

} // namespace crosspad_sampler

#endif // USE_LVGL
