#include "processor.h"
#include "controller.h"
#include "version.h"

#include "public.sdk/source/main/pluginfactory.h"

#define VENDOR_URL  ""
#define VENDOR_MAIL ""

BEGIN_FACTORY_DEF (VENDOR_NAME, VENDOR_URL, VENDOR_MAIL)

    DEF_CLASS2 (INLINE_UID_FROM_FUID (MicFilter::kProcessorUID),
                Steinberg::PClassInfo::kManyInstances,
                kVstAudioEffectClass,
                PLUGIN_NAME,
                Steinberg::Vst::kDistributable,
                PLUGIN_CATEGORY,
                FULL_VERSION_STR,
                kVstVersionString,
                MicFilter::Processor::createInstance)

    DEF_CLASS2 (INLINE_UID_FROM_FUID (MicFilter::kControllerUID),
                Steinberg::PClassInfo::kManyInstances,
                kVstComponentControllerClass,
                "MicFilterController",
                0,
                "",
                FULL_VERSION_STR,
                kVstVersionString,
                MicFilter::Controller::createInstance)

END_FACTORY
