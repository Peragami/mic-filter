#include "vst3_host.h"

#include "log.h"

#include <windows.h>
#include <cstring>
#include <string>

#if MICFILTER_HAS_VST3_SDK

// NOTE on IIDs: without INIT_CLASS_IID, DECLARE_CLASS_IID in the headers
// only declares ClassName::iid. Base-interface IIDs come from the SDK's
// pluginterfaces/base/coreiids.cpp (compiled by CMake), and the VST
// interface IIDs used here are defined below with DEF_CLASS_IID -- the
// same mechanism public.sdk's pluginterfacesupport.cpp uses for hosts.
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <map>
#include <mutex>
#include <vector>

namespace Steinberg {
namespace Vst {
DEF_CLASS_IID (IComponent)
DEF_CLASS_IID (IAudioProcessor)
DEF_CLASS_IID (IEditController)
DEF_CLASS_IID (IConnectionPoint)
DEF_CLASS_IID (IHostApplication)
DEF_CLASS_IID (IParameterChanges)
DEF_CLASS_IID (IParamValueQueue)
} // namespace Vst
} // namespace Steinberg

namespace {

using namespace Steinberg;

bool TuidEqual (const TUID a, const TUID b) { return std::memcmp (a, b, sizeof (TUID)) == 0; }

template <typename T> void SafeRelease (T*& p)
{
    if (p) { p->release(); p = nullptr; }
}

// Minimal IHostApplication, implemented directly on the interface
// (no public.sdk helpers, so no SDK .cpp files need to be linked).
class HostApplication final : public Vst::IHostApplication
{
public:
    // FUnknown
    tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override
    {
        if (TuidEqual (iid, Vst::IHostApplication::iid) || TuidEqual (iid, FUnknown::iid))
        {
            *obj = static_cast<Vst::IHostApplication*> (this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refCount_; }
    uint32 PLUGIN_API release() override
    {
        const uint32 v = --refCount_;
        if (v == 0) delete this;
        return v;
    }
    // IHostApplication
    tresult PLUGIN_API getName (Vst::String128 name) override
    {
        const char16_t* src = u"MicFilterClient";
        int i = 0;
        for (; src[i] && i < 127; ++i) name[i] = src[i];
        name[i] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API createInstance (TUID, TUID, void** obj) override
    {
        if (obj) *obj = nullptr;
        return kNotImplemented; // this host provides no message/attribute objects in v0.1
    }
private:
    uint32 refCount_ = 1;
};

// Minimal IParamValueQueue / IParameterChanges for injecting GUI-driven
// parameter changes into ProcessData (same manual-FUnknown style as above).
class ParamValueQueue final : public Vst::IParamValueQueue
{
public:
    explicit ParamValueQueue (Vst::ParamID id) : id_ (id) {}
    tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override
    {
        if (TuidEqual (iid, Vst::IParamValueQueue::iid) || TuidEqual (iid, FUnknown::iid))
        {
            *obj = static_cast<Vst::IParamValueQueue*> (this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refCount_; }
    uint32 PLUGIN_API release() override
    {
        const uint32 v = --refCount_;
        if (v == 0) delete this;
        return v;
    }
    Vst::ParamID PLUGIN_API getParameterId() override { return id_; }
    int32 PLUGIN_API getPointCount() override { return (int32) points_.size(); }
    tresult PLUGIN_API getPoint (int32 index, int32& sampleOffset, Vst::ParamValue& value) override
    {
        if (index < 0 || index >= (int32) points_.size()) return kResultFalse;
        sampleOffset = points_[(size_t) index].first;
        value = points_[(size_t) index].second;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint (int32 sampleOffset, Vst::ParamValue value, int32& index) override
    {
        points_.push_back ({ sampleOffset, value });
        index = (int32) points_.size() - 1;
        return kResultOk;
    }
private:
    Vst::ParamID id_;
    std::vector<std::pair<int32, Vst::ParamValue>> points_;
    uint32 refCount_ = 1;
};

class ParameterChanges final : public Vst::IParameterChanges
{
public:
    ~ParameterChanges() { for (auto* q : queues_) q->release(); } // FUnknown has no virtual dtor
    tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override
    {
        if (TuidEqual (iid, Vst::IParameterChanges::iid) || TuidEqual (iid, FUnknown::iid))
        {
            *obj = static_cast<Vst::IParameterChanges*> (this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refCount_; }
    uint32 PLUGIN_API release() override
    {
        const uint32 v = --refCount_;
        if (v == 0) delete this;
        return v;
    }
    int32 PLUGIN_API getParameterCount() override { return (int32) queues_.size(); }
    Vst::IParamValueQueue* PLUGIN_API getParameterData (int32 index) override
    {
        if (index < 0 || index >= (int32) queues_.size()) return nullptr;
        return queues_[(size_t) index];
    }
    Vst::IParamValueQueue* PLUGIN_API addParameterData (const Vst::ParamID& pid, int32& index) override
    {
        for (size_t i = 0; i < queues_.size(); ++i)
            if (queues_[i]->getParameterId() == pid)
            {
                index = (int32) i;
                return queues_[i];
            }
        auto* q = new ParamValueQueue (pid);
        queues_.push_back (q);
        index = (int32) queues_.size() - 1;
        return q;
    }
private:
    std::vector<ParamValueQueue*> queues_;
    uint32 refCount_ = 1;
};

bool FileExists (const std::wstring& p)
{
    const DWORD a = GetFileAttributesW (p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// A .vst3 path may be the bundle folder; the real binary is
//   <Name>.vst3\Contents\x86_64-win\<Name>.vst3
std::wstring ResolveBinaryPath (const std::wstring& path)
{
    const DWORD a = GetFileAttributesW (path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY))
        return path; // assume it already is the binary

    std::wstring dir = path + L"\\Contents\\x86_64-win\\";
    // First try <folder name>.vst3 inside the arch folder.
    std::wstring base = path;
    const size_t slash = base.find_last_of (L"\\/");
    if (slash != std::wstring::npos) base = base.substr (slash + 1);
    if (FileExists (dir + base)) return dir + base;
    // Otherwise take the first *.vst3 file found there.
    WIN32_FIND_DATAW fd {};
    const HANDLE h = FindFirstFileW ((dir + L"*.vst3").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        std::wstring found = dir + fd.cFileName;
        FindClose (h);
        return found;
    }
    return path; // let LoadLibrary fail with a clear error below
}

} // namespace

struct Vst3Host::Impl
{
    using InitDllProc = bool (PLUGIN_API*)();
    using ExitDllProc = bool (PLUGIN_API*)();
    using GetPluginFactoryProc = IPluginFactory* (PLUGIN_API*)();

    HMODULE module = nullptr;
    ExitDllProc exitDll = nullptr;
    IPluginFactory* factory = nullptr;
    Vst::IComponent* component = nullptr;
    Vst::IEditController* controller = nullptr;
    Vst::IAudioProcessor* processor = nullptr;
    Vst::IConnectionPoint* compConn = nullptr;
    Vst::IConnectionPoint* ctrlConn = nullptr;
    HostApplication* hostApp = nullptr;
    bool active = false;
    float* inPtrs[2] = { nullptr, nullptr };
    float* outPtrs[2] = { nullptr, nullptr };
    std::mutex paramMutex;
    std::map<std::uint32_t, double> pendingParams; // drained by Process()
};

Vst3Host::~Vst3Host() { Unload(); }

bool Vst3Host::Load (const std::wstring& pluginPath, double sampleRate, int maxBlockFrames,
                     std::wstring& errorOut)
{
    Unload();
    impl_ = new Impl();
    auto fail = [&] (const std::wstring& msg) {
        miclog::WriteW (L"host: load failed: %s", msg.c_str());
        errorOut = msg;
        Unload();
        return false;
    };

    const std::wstring binary = ResolveBinaryPath (pluginPath);
    miclog::WriteW (L"host: loading binary = %s", binary.c_str());
    impl_->module = LoadLibraryW (binary.c_str());
    if (!impl_->module)
        return fail (L"プラグインを開けません: " + binary +
                     L" (error " + std::to_wstring (GetLastError()) + L")");

    if (auto initDll = reinterpret_cast<Impl::InitDllProc> (GetProcAddress (impl_->module, "InitDll")))
        initDll();
    impl_->exitDll = reinterpret_cast<Impl::ExitDllProc> (GetProcAddress (impl_->module, "ExitDll"));
    auto getFactory = reinterpret_cast<Impl::GetPluginFactoryProc> (
        GetProcAddress (impl_->module, "GetPluginFactory"));
    if (!getFactory) return fail (L"GetPluginFactory が見つかりません（VST3ではありません）");
    impl_->factory = getFactory();
    if (!impl_->factory) return fail (L"IPluginFactory の取得に失敗しました");

    // Find the audio-effect class ("Audio Module Class" == kVSTAudioEffectClass).
    TUID effectCid {};
    bool found = false;
    const int32 classCount = impl_->factory->countClasses();
    for (int32 i = 0; i < classCount; ++i)
    {
        PClassInfo info {};
        if (impl_->factory->getClassInfo (i, &info) == kResultOk &&
            std::strcmp (info.category, "Audio Module Class") == 0)
        {
            std::memcpy (effectCid, info.cid, sizeof (TUID));
            found = true;
            break;
        }
    }
    if (!found) return fail (L"オーディオエフェクトのクラスがプラグイン内にありません");

    if (impl_->factory->createInstance (effectCid, Vst::IComponent::iid,
                                        reinterpret_cast<void**> (&impl_->component)) != kResultOk ||
        !impl_->component)
        return fail (L"IComponent の生成に失敗しました");

    impl_->hostApp = new HostApplication();
    if (impl_->component->initialize (impl_->hostApp) != kResultOk)
        return fail (L"IComponent::initialize に失敗しました");

    // Controller (optional for audio, created + connected the standard way so
    // parameters/editor support can be added without restructuring).
    TUID ctrlCid {};
    if (impl_->component->getControllerClassId (ctrlCid) == kResultOk)
    {
        if (impl_->factory->createInstance (ctrlCid, Vst::IEditController::iid,
                                            reinterpret_cast<void**> (&impl_->controller)) == kResultOk &&
            impl_->controller)
        {
            impl_->controller->initialize (impl_->hostApp);
        }
    }
    if (impl_->controller)
    {
        impl_->component->queryInterface (Vst::IConnectionPoint::iid,
                                          reinterpret_cast<void**> (&impl_->compConn));
        impl_->controller->queryInterface (Vst::IConnectionPoint::iid,
                                           reinterpret_cast<void**> (&impl_->ctrlConn));
        if (impl_->compConn && impl_->ctrlConn)
        {
            impl_->compConn->connect (impl_->ctrlConn);
            impl_->ctrlConn->connect (impl_->compConn);
        }
    }

    impl_->component->setIoMode (Vst::kAdvanced); // result intentionally ignored

    // Activate only the first audio bus in/out (stereo); deactivate the rest.
    for (int32 dir = 0; dir < 2; ++dir)
    {
        const auto busDir = dir == 0 ? Vst::kInput : Vst::kOutput;
        const int32 n = impl_->component->getBusCount (Vst::kAudio, busDir);
        for (int32 b = 0; b < n; ++b)
            impl_->component->activateBus (Vst::kAudio, busDir, b, b == 0);
    }

    if (impl_->component->queryInterface (Vst::IAudioProcessor::iid,
                                          reinterpret_cast<void**> (&impl_->processor)) != kResultOk ||
        !impl_->processor)
        return fail (L"IAudioProcessor の取得に失敗しました");
    if (impl_->processor->canProcessSampleSize (Vst::kSample32) != kResultOk)
        return fail (L"プラグインが float32 処理に対応していません");

    Vst::SpeakerArrangement inArr = Vst::SpeakerArr::kStereo;
    Vst::SpeakerArrangement outArr = Vst::SpeakerArr::kStereo;
    impl_->processor->setBusArrangements (&inArr, 1, &outArr, 1); // may legitimately adjust

    Vst::ProcessSetup setup {};
    setup.processMode = Vst::kRealtime;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.sampleRate = sampleRate;
    setup.maxSamplesPerBlock = maxBlockFrames;
    if (impl_->processor->setupProcessing (setup) != kResultOk)
        return fail (L"setupProcessing に失敗しました");

    if (impl_->component->setActive (true) != kResultOk)
        return fail (L"setActive(true) に失敗しました");
    impl_->active = true;

    miclog::Write ("host: plugin loaded ok (classes=%d, latency=%d samples, sampleRate=%.0f, maxBlock=%d)",
                   (int) classCount, (int) impl_->processor->getLatencySamples(), sampleRate, maxBlockFrames);
    loaded_ = true;
    return true;
}

void Vst3Host::Process (const float* const* in, float* const* out, int frames)
{
    if (!loaded_ || !impl_ || !impl_->processor)
    {
        for (int ch = 0; ch < 2; ++ch)
            std::memcpy (out[ch], in[ch], sizeof (float) * (size_t) frames);
        return;
    }
    for (int ch = 0; ch < 2; ++ch)
    {
        impl_->inPtrs[ch] = const_cast<float*> (in[ch]);
        impl_->outPtrs[ch] = out[ch];
    }
    Vst::AudioBusBuffers inBus {}, outBus {};
    inBus.numChannels = 2;
    inBus.channelBuffers32 = impl_->inPtrs;
    outBus.numChannels = 2;
    outBus.channelBuffers32 = impl_->outPtrs;

    Vst::ProcessData data {};
    data.processMode = Vst::kRealtime;
    data.symbolicSampleSize = Vst::kSample32;
    data.numSamples = frames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &inBus;
    data.outputs = &outBus;
    data.processContext = nullptr; // v0.1: no tempo/transport info (documented limitation)

    // Inject any queued GUI parameter changes with this block.
    ParameterChanges changes;
    bool haveChanges = false;
    {
        std::map<std::uint32_t, double> pending;
        {
            std::lock_guard<std::mutex> lock (impl_->paramMutex);
            pending.swap (impl_->pendingParams);
        }
        for (const auto& [id, value] : pending)
        {
            int32 idx = 0;
            if (auto* q = changes.addParameterData (id, idx))
            {
                int32 pointIdx = 0;
                q->addPoint (0, value, pointIdx);
                haveChanges = true;
            }
        }
    }
    if (haveChanges) data.inputParameterChanges = &changes;
    impl_->processor->process (data);
}

void Vst3Host::SetParameter (std::uint32_t id, double normalized)
{
    if (!impl_) return;
    std::lock_guard<std::mutex> lock (impl_->paramMutex);
    impl_->pendingParams[id] = normalized < 0.0 ? 0.0 : (normalized > 1.0 ? 1.0 : normalized);
}

void Vst3Host::Unload()
{
    if (!impl_) { loaded_ = false; return; }
    if (impl_->component && impl_->active)
        impl_->component->setActive (false);
    impl_->active = false;
    if (impl_->compConn && impl_->ctrlConn)
    {
        impl_->compConn->disconnect (impl_->ctrlConn);
        impl_->ctrlConn->disconnect (impl_->compConn);
    }
    SafeRelease (impl_->compConn);
    SafeRelease (impl_->ctrlConn);
    SafeRelease (impl_->processor);
    if (impl_->controller) impl_->controller->terminate();
    SafeRelease (impl_->controller);
    if (impl_->component) impl_->component->terminate();
    SafeRelease (impl_->component);
    SafeRelease (impl_->factory);
    if (impl_->hostApp) { impl_->hostApp->release(); impl_->hostApp = nullptr; }
    if (impl_->exitDll) impl_->exitDll();
    if (impl_->module) FreeLibrary (impl_->module);
    delete impl_;
    impl_ = nullptr;
    loaded_ = false;
}

#else // !MICFILTER_HAS_VST3_SDK

struct Vst3Host::Impl {};

Vst3Host::~Vst3Host() { Unload(); }

bool Vst3Host::Load (const std::wstring&, double, int, std::wstring& errorOut)
{
    errorOut = L"このビルドには VST3 SDK が組み込まれていません。"
               L" -DVST3_SDK_PATH=<SDKルート> を付けて再ビルドしてください（client/README.md 参照）。";
    return false;
}

void Vst3Host::Process (const float* const* in, float* const* out, int frames)
{
    for (int ch = 0; ch < 2; ++ch)
        std::memcpy (out[ch], in[ch], sizeof (float) * (size_t) frames);
}

void Vst3Host::SetParameter (std::uint32_t, double) {}

void Vst3Host::Unload() { loaded_ = false; }

#endif // MICFILTER_HAS_VST3_SDK
