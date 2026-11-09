#pragma once
// Minimal VST3 host: loads one audio-effect plugin (.vst3) and runs audio
// through it in realtime. No framework, no SDK .cpp files are linked --
// everything goes through the COM-style interfaces in the SDK headers.
//
// Lifecycle implemented here (the standard VST3 hosting sequence):
//   LoadLibrary(bundle) -> InitDll -> GetPluginFactory
//   -> createInstance(IComponent) + createInstance(IEditController)
//   -> IConnectionPoint connect (component <-> controller)
//   -> initialize() both -> activate audio buses
//   -> IAudioProcessor::setBusArrangements / setupProcessing
//   -> IComponent::setActive(true) -> process() per block
//   -> setActive(false) -> disconnect -> terminate -> ExitDll -> FreeLibrary
//
// If the client was built without the SDK (MICFILTER_HAS_VST3_SDK not set),
// Load() fails with a descriptive error and everything else is a no-op stub.

#include <cstdint>
#include <string>

class Vst3Host
{
public:
    Vst3Host() = default;
    ~Vst3Host();
    Vst3Host (const Vst3Host&) = delete;
    Vst3Host& operator= (const Vst3Host&) = delete;

    // pluginPath: either the .vst3 bundle folder
    //   (MicFilter.vst3\Contents\x86_64-win\MicFilter.vst3 is resolved)
    // or the path of the binary itself. channels is fixed to 2 (stereo);
    // a mono mic is duplicated by the engine before Process().
    bool Load (const std::wstring& pluginPath, double sampleRate, int maxBlockFrames,
               std::wstring& errorOut);
    void Unload();

    bool IsLoaded() const { return loaded_; }

    // Queue a parameter change (VST param id, normalized 0..1). Thread-safe;
    // accumulated and injected into ProcessData at the next Process() call.
    void SetParameter (std::uint32_t id, double normalized);

    // Planar float32, channels x frames. in and out must not alias.
    // frames must be <= maxBlockFrames passed to Load().
    void Process (const float* const* in, float* const* out, int frames);

private:
    struct Impl;
    Impl* impl_ = nullptr;   // holds all SDK-typed pointers (keeps this header SDK-free)
    bool loaded_ = false;
};
