#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"

namespace MicFilter {

// No GUI: the host's generic parameter UI is used on purpose (lightest option).
class Controller : public Steinberg::Vst::EditControllerEx1
{
public:
    Controller () = default;
    ~Controller () override = default;

    static Steinberg::FUnknown* createInstance (void*) { return (Steinberg::Vst::IEditController*) new Controller; }

    Steinberg::tresult PLUGIN_API initialize (Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API setComponentState (Steinberg::IBStream* state) override;
};

} // namespace MicFilter
