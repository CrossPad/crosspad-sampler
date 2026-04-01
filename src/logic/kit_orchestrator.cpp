// SPDX-License-Identifier: MIT

#include <crosspad-sampler/logic/kit_orchestrator.hpp>
#include <crosspad-sampler/logic/pad_color_manager.hpp>
#include <crosspad-sampler/SamplerConfig.hpp>

#include <crosspad/kit/IKitLoader.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/event/IEventBus.hpp>
#include <crosspad/event/EventTypes.hpp>
#include <crosspad/platform/PlatformServices.hpp>

namespace crosspad_sampler {

void KitOrchestrator::init(PadColorManager& colorManager)
{
    colorManager_ = &colorManager;
}

bool KitOrchestrator::changeKit(int kitId)
{
    // 1. Bail out if a load is already in progress.
    if (loading_) {
        return false;
    }

    // 2. Obtain service pointers.
    crosspad::IKitManager* kitManager = crosspad::getKitManager();
    crosspad::IKitLoader*  kitLoader  = crosspad::getKitLoader();

    if (!kitManager || !kitLoader) {
        return false;
    }

    // 3. Retrieve the kit metadata.
    crosspad::KitInfo* kit = kitManager->getKit(kitId);
    if (!kit) {
        return false;
    }

    // 4. Mark as loading.
    loading_ = true;

    // 5. Stop all active sample playback.
    crosspad::IEventBus& bus = crosspad::getEventBus();
    for (int pad = 0; pad < PAD_COUNT; ++pad) {
        bus.postSamplePlayback(
            static_cast<uint8_t>(pad),
            /*wavIdx=*/0,
            /*isOn=*/false,
            crosspad::EventSource::App);
    }

    // 6. Load the kit from storage.
    crosspad::KitLoadResult result = kitLoader->loadKit(*kit);

    // 7. Sync pad colors with the newly loaded kit.
    if (result.success && colorManager_) {
        colorManager_->loadColorsFromKit(*kit);
    }

    // 8. Notify caller of load progress.
    if (progressFn_) {
        progressFn_(result.padsLoaded, PAD_COUNT, progressData_);
    }

    // 9. Clear loading flag.
    loading_ = false;

    // 10. Notify the rest of the system that the active app has changed.
    bus.postAppChanged("Sampler", crosspad::EventSource::App);

    return result.success;
}

void KitOrchestrator::setProgressCallback(KitLoadProgressFn fn, void* userData)
{
    progressFn_   = fn;
    progressData_ = userData;
}

bool KitOrchestrator::isLoading() const
{
    return loading_;
}

} // namespace crosspad_sampler
