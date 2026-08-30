// SPDX-License-Identifier: MIT

#include <crosspad-sampler/logic/kit_orchestrator.hpp>
#include <crosspad-sampler/logic/pad_color_manager.hpp>
#include <crosspad-sampler/SamplerConfig.hpp>

#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/event/IEventBus.hpp>
#include <crosspad/event/EventTypes.hpp>
#include <crosspad/platform/PlatformServices.hpp>

namespace crosspad_sampler {

void KitOrchestrator::init(PadColorManager& colorManager)
{
    colorManager_ = &colorManager;
}

crosspad::KitLoadResult KitOrchestrator::changeKit(int kitId)
{
    crosspad::KitLoadResult result;

    if (loading_) {
        result.error = "Already loading";
        return result;
    }

    crosspad::IKitManager* kitManager = crosspad::getKitManager();
    if (!kitManager) {
        result.error = "Kit manager unavailable";
        return result;
    }

    crosspad::KitInfo* kit = kitManager->getKit(kitId);
    if (!kit) {
        result.error = "Kit not found";
        return result;
    }

    loading_ = true;

    // Stop everything that is still sounding.
    crosspad::IEventBus& bus = crosspad::getEventBus();
    for (int pad = 0; pad < PAD_COUNT; ++pad) {
        bus.postSamplePlayback(
            static_cast<uint8_t>(pad),
            /*wavIdx=*/0,
            /*isOn=*/false,
            crosspad::EventSource::App);
    }

    result = kitManager->loadKit(*kit);
    if (result.success) {
        kitManager->setCurrentKitId(kitId);
    }

    if (result.success && colorManager_) {
        colorManager_->loadColorsFromKit(*kit);
    }

    if (progressFn_) {
        progressFn_(result.padsLoaded, PAD_COUNT, progressData_);
    }

    loading_ = false;

    bus.postAppChanged("Sampler", crosspad::EventSource::App);

    return result;
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
