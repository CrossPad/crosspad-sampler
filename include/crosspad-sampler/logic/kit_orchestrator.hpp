// SPDX-License-Identifier: MIT
#pragma once

#include <crosspad/kit/IKitManager.hpp>

namespace crosspad_sampler {

class PadColorManager;

/// Callback invoked during kit load to report progress.
/// @param current  Number of pads loaded so far.
/// @param total    Total number of pads to load.
/// @param userData Opaque user context pointer.
using KitLoadProgressFn = void (*)(int current, int total, void* userData);

/**
 * @brief Coordinates kit switching for the Sampler app.
 *
 * Stops all active sample playback, loads the new kit via IKitManager,
 * updates pad colors, reports load progress, and posts AppChanged to the
 * event bus.
 */
class KitOrchestrator {
public:
    KitOrchestrator() = default;

    /// Store a reference to the PadColorManager used to apply kit colors.
    void init(PadColorManager& colorManager);

    /// Switch to the kit identified by @p kitId.
    ///
    /// Flow:
    ///   1. Return false if already loading.
    ///   2. Retrieve IKitManager from PlatformServices.
    ///   3. Retrieve the KitInfo for @p kitId.
    ///   4. Set loading_ = true.
    ///   5. Stop all playback (pads 0–15 via postSamplePlayback).
    ///   6. Call IKitManager::loadKit() and setCurrentKitId().
    ///   7. On success, call PadColorManager::loadColorsFromKit().
    ///   8. Invoke the progress callback with (padsLoaded, PAD_COUNT).
    ///   9. Set loading_ = false.
    ///  10. Post AppChanged("Sampler") via IEventBus.
    ///
    /// @return KitLoadResult — success flag, error string, padsLoaded.
    crosspad::KitLoadResult changeKit(int kitId);

    /// Register a callback that is invoked with load progress.
    void setProgressCallback(KitLoadProgressFn fn, void* userData);

    /// @return true while a kit load is in progress.
    bool isLoading() const;

private:
    PadColorManager*  colorManager_  = nullptr;
    KitLoadProgressFn progressFn_    = nullptr;
    void*             progressData_  = nullptr;
    bool              loading_       = false;
};

} // namespace crosspad_sampler
