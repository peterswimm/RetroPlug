#include "AppleEngineHost.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <utility>

#include "AppleQuickJsHost.hpp"
#include "host/engine/Engine.hpp"
#include "host/engine/EngineInvoker.hpp"
#include "host/rpc/BackendRpcRegistration.hpp"
#include "system/SystemFactory.hpp"
#include "system/sameboy/SameBoyBackend.hpp"
#include "system/sameboy/SameBoySystem.hpp"
#include "TypedRpcServer.h"
#include "codecs/QuickJSCodec.h"
#include "transports/QuickJSTransport.h"

extern "C" {
extern const std::uint8_t rp_cp_bundle[];
extern const std::uint32_t rp_cp_bundle_size;
}

using AppleRpcServer = rpcpp::TypedRpcServer<rpcpp::Empty, rpcpp::QuickJSCodec>;

struct AppleEngineHost::Impl {
    AppleQuickJsHost host;
    Engine engine;
    SystemFactory factory;
    QueuedInvoker invoker{engine, engine.registry()};
    HostRpcService hostService;
    EngineRpcService engineService{engine, factory, invoker};
    std::unique_ptr<rpcpp::QuickJSTransport> transport;
    std::unique_ptr<AppleRpcServer> server;
    bool isReady = false;
    std::string startupError;
    bool running = false;
    std::atomic<std::uint32_t> primaryId{0};
    bool noteOutEnabled = false;
    std::array<std::uint8_t, 4> noteChannels{0, 1, 2, 3};
    std::array<std::uint8_t, 4> ccChannels{0, 1, 2, 3};
    struct NoteVoice {
        int note = -1;
        std::uint16_t frequency = 0;
        std::uint8_t envelope = 0;
        std::uint8_t duty = 0xff;
        std::uint16_t bend = 0x2000;
    } noteVoices[4];
    std::uint8_t nr51 = 0xff;
    bool waveDac = false;
    std::vector<AppleEngineHost::MidiMessage> appleMidi;

    explicit Impl(double sampleRate) : engine(sampleRate) {}

    bool boot() {
        if (!host.init()) return false;
        JSContext* ctx = host.context();

        // Distribution boundary: iOS registers SameBoy only. No Mesen translation unit or GPL
        // console backend is reachable from this target.
        factory.registerBackend("sameboy", std::make_unique<SameBoyBackend>());
        transport = std::make_unique<rpcpp::QuickJSTransport>(ctx, [](JSContext*, JSValue) {});
        server = std::make_unique<AppleRpcServer>(*transport, rpcpp::QuickJSCodec{ctx});
        registerHostRpc(*server, hostService);
        registerEmulatorRpc(*server, engineService);
        registerDspKernelRpc(*server, engineService);
        server->addDiscoveryMethod();

        JSValue global = JS_GetGlobalObject(ctx);
        JSValue sym = JS_NewSymbol(ctx, "plugin", 1);
        JSAtom atom = JS_ValueToAtom(ctx, sym);
        JSValue ns = JS_NewObjectProto(ctx, JS_NULL);
        AppleRpcServer* rpc = server.get();
        host.bindRpcSend(ns, [rpc](JSContext* callCtx, JSValueConst request) -> JSValue {
            auto response = rpc->processMessage(request);
            return response ? response->materialize(callCtx) : JS_NULL;
        });
        JS_DefinePropertyValue(ctx, global, atom, ns, JS_PROP_C_W_E);
        JS_FreeAtom(ctx, atom);
        JS_FreeValue(ctx, sym);
        JS_FreeValue(ctx, global);

        if (host.evalModuleBytecode(rp_cp_bundle, rp_cp_bundle_size) != 0) {
            startupError = host.lastError();
            return false;
        }
        for (int i = 0; i < 1000 && !globalBool("__rp_ready"); ++i) host.pump();
        isReady = globalBool("__rp_ready");
        if (!isReady) startupError = "Control plane did not report ready";
        return isReady;
    }

    JSValue call(const char* name, std::vector<JSValue> args = {}) {
        JSContext* ctx = host.context();
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue fn = JS_GetPropertyStr(ctx, global, name);
        JSValue result = JS_DupValue(ctx, fn);
        if (JS_IsFunction(ctx, fn)) {
            JS_FreeValue(ctx, result);
            result = JS_Call(ctx, fn, global, static_cast<int>(args.size()), args.data());
        }
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, global);
        return result;
    }

    bool globalBool(const char* name) {
        JSValue value = call(name);
        const bool result = !JS_IsException(value) && JS_ToBool(host.context(), value) > 0;
        JS_FreeValue(host.context(), value);
        return result;
    }

    bool callBool(const char* name, const std::vector<std::string>& strings) {
        JSContext* ctx = host.context();
        std::vector<JSValue> args;
        args.reserve(strings.size());
        for (const auto& value : strings) args.push_back(JS_NewStringLen(ctx, value.data(), value.size()));
        JSValue result = call(name, args);
        for (JSValue arg : args) JS_FreeValue(ctx, arg);
        const bool ok = !JS_IsException(result) && JS_ToBool(ctx, result) > 0;
        JS_FreeValue(ctx, result);
        return ok;
    }

    std::string callString(const char* name) {
        JSContext* ctx = host.context();
        JSValue result = call(name);
        std::string out;
        if (!JS_IsException(result)) {
            const char* value = JS_ToCString(ctx, result);
            if (value) { out = value; JS_FreeCString(ctx, value); }
        }
        JS_FreeValue(ctx, result);
        return out;
    }

    std::uint32_t callId(const char* name) {
        JSContext* ctx = host.context();
        JSValue result = call(name);
        std::uint32_t id = 0;
        if (!JS_IsException(result)) JS_ToUint32(ctx, &id, result);
        JS_FreeValue(ctx, result);
        return id;
    }

    std::uint32_t refreshPrimaryId() {
        const auto id = callId("__rp_applePrimarySystemId");
        primaryId.store(id, std::memory_order_release);
        return id;
    }

    bool setRole(const char* role, const std::string& json) {
        const auto id = primaryId.load(std::memory_order_acquire);
        if (!id) return false;
        JSContext* ctx = host.context();
        std::vector<JSValue> args{JS_NewUint32(ctx, id), JS_NewString(ctx, role),
                                  JS_NewStringLen(ctx, json.data(), json.size())};
        JSValue result = call("__rp_appleSetRoleConfig", args);
        for (JSValue arg : args) JS_FreeValue(ctx, arg);
        const bool ok = !JS_IsException(result) && JS_ToBool(ctx, result) > 0;
        JS_FreeValue(ctx, result);
        return ok;
    }

    void emit(std::uint32_t frame, std::initializer_list<std::uint8_t> bytes) {
        appleMidi.push_back({frame, std::vector<std::uint8_t>(bytes)});
    }
    double pitch(std::size_t voice, std::uint16_t raw) const {
        double hz;
        if (voice == 3) {
            const double ratio = (raw & 7) ? double(raw & 7) : 0.5;
            hz = 524288.0 / ratio / std::exp2(double(raw >> 4) + 1.0);
        } else {
            hz = (voice == 2 ? 65536.0 : 131072.0) / (2048.0 - double(raw & 0x7ff));
        }
        return 69.0 + 12.0 * std::log2(hz / 440.0);
    }
    std::uint8_t volume(std::size_t voice, std::uint8_t envelope) const {
        if (voice == 2) {
            constexpr std::uint8_t levels[4] = {0, 127, 64, 32};
            return levels[(envelope >> 5) & 3];
        }
        return std::uint8_t((envelope >> 4) * 127 / 15);
    }
    bool dacOn(std::size_t voice) const {
        if (voice == 2) return waveDac && ((noteVoices[2].envelope >> 5) & 3) != 0;
        return (noteVoices[voice].envelope & 0xf8) != 0;
    }
    void noteOff(std::size_t voice, std::uint32_t frame) {
        auto& state = noteVoices[voice];
        if (state.note < 0) return;
        emit(frame, {std::uint8_t(0x80 | noteChannels[voice]), std::uint8_t(state.note), 0});
        state.note = -1;
    }
    void resetBend(std::size_t voice, std::uint32_t frame) {
        auto& state = noteVoices[voice];
        if (state.bend == 0x2000) return;
        state.bend = 0x2000;
        emit(frame, {std::uint8_t(0xe0 | noteChannels[voice]), 0, 0x40});
    }
    void trigger(std::size_t voice, std::uint32_t frame) {
        auto& state = noteVoices[voice];
        noteOff(voice, frame);
        if (!dacOn(voice)) return;
        const int note = std::clamp(int(std::lround(pitch(voice, state.frequency))), 0, 127);
        resetBend(voice, frame);
        emit(frame, {std::uint8_t(0x90 | noteChannels[voice]), std::uint8_t(note),
                     std::max<std::uint8_t>(1, volume(voice, state.envelope))});
        state.note = note;
    }
    void slide(std::size_t voice, std::uint32_t frame) {
        auto& state = noteVoices[voice];
        if (state.note < 0) return;
        const double semitones = pitch(voice, state.frequency) - double(state.note);
        if (std::fabs(semitones) > 2.0) { trigger(voice, frame); return; }
        const auto bend = std::uint16_t(std::clamp(int(std::lround(8192.0 + semitones * 4096.0)), 0, 16383));
        if (bend == state.bend) return;
        state.bend = bend;
        emit(frame, {std::uint8_t(0xe0 | noteChannels[voice]), std::uint8_t(bend & 0x7f), std::uint8_t(bend >> 7)});
    }
    void cc(std::size_t voice, std::uint8_t number, std::uint8_t value, std::uint32_t frame) {
        emit(frame, {std::uint8_t(0xb0 | ccChannels[voice]), number, value});
    }
    void envelope(std::size_t voice, std::uint8_t value, std::uint32_t frame) {
        const auto previous = volume(voice, noteVoices[voice].envelope);
        noteVoices[voice].envelope = value;
        const auto current = volume(voice, value);
        if ((voice == 2 && current == 0) || (voice != 2 && (value & 0xf8) == 0)) noteOff(voice, frame);
        else if (current != previous) cc(voice, 7, current, frame);
    }
    void drainNoteOut(SameBoySystem& system) {
        for (const auto& write : system.apuWriteLog_) {
            const auto frame = write.offset;
            switch (write.reg) {
                case 0x13: case 0x18: case 0x1d: {
                    const std::size_t voice = write.reg == 0x13 ? 0 : write.reg == 0x18 ? 1 : 2;
                    noteVoices[voice].frequency = std::uint16_t((noteVoices[voice].frequency & 0x700) | write.value);
                    slide(voice, frame); break;
                }
                case 0x14: case 0x19: case 0x1e: {
                    const std::size_t voice = write.reg == 0x14 ? 0 : write.reg == 0x19 ? 1 : 2;
                    noteVoices[voice].frequency = std::uint16_t((noteVoices[voice].frequency & 0xff) | ((write.value & 7) << 8));
                    if (write.value & 0x80) trigger(voice, frame); else slide(voice, frame); break;
                }
                case 0x11: case 0x16: {
                    const std::size_t voice = write.reg == 0x11 ? 0 : 1;
                    const auto duty = std::uint8_t(write.value >> 6);
                    if (duty != noteVoices[voice].duty) { noteVoices[voice].duty = duty; cc(voice, 70, std::uint8_t(duty * 42), frame); }
                    break;
                }
                case 0x12: envelope(0, write.value, frame); break;
                case 0x17: envelope(1, write.value, frame); break;
                case 0x1c: envelope(2, write.value, frame); break;
                case 0x21: envelope(3, write.value, frame); break;
                case 0x1a: waveDac = (write.value & 0x80) != 0; if (!waveDac) noteOff(2, frame); break;
                case 0x22: noteVoices[3].frequency = write.value; if (noteVoices[3].note >= 0) trigger(3, frame); break;
                case 0x23: if (write.value & 0x80) trigger(3, frame); break;
                case 0x25: {
                    const auto previous = nr51; nr51 = write.value;
                    for (std::size_t voice = 0; voice < 4; ++voice) {
                        const auto bits = std::uint8_t(((write.value >> voice) & 1) | (((write.value >> (4 + voice)) & 1) << 1));
                        const auto old = previous == 0xff ? std::uint8_t(0xff) : std::uint8_t(((previous >> voice) & 1) | (((previous >> (4 + voice)) & 1) << 1));
                        if (bits == old) continue;
                        if (!bits) noteOff(voice, frame); else cc(voice, 10, bits == 2 ? 0 : bits == 1 ? 127 : 64, frame);
                    }
                    break;
                }
                case 0x26: if (!(write.value & 0x80)) for (std::size_t voice = 0; voice < 4; ++voice) noteOff(voice, frame); break;
                default: break;
            }
        }
    }
};

AppleEngineHost::AppleEngineHost(double sampleRate) : impl_(std::make_unique<Impl>(sampleRate)) {
    impl_->boot();
}
AppleEngineHost::~AppleEngineHost() { suspend(); }
bool AppleEngineHost::ready() const { return impl_->isReady; }
const std::string& AppleEngineHost::startupError() const { return impl_->startupError; }
void AppleEngineHost::setSampleRate(double sampleRate) { if (!impl_->running) impl_->engine.setSampleRate(sampleRate); }
void AppleEngineHost::resume() { impl_->invoker.setAudioThreadOwns(true); impl_->running = true; }
void AppleEngineHost::suspend() {
    if (!impl_ || !impl_->running) return;
    impl_->invoker.setAudioThreadOwns(false);
    impl_->invoker.reclaimReleased();
    impl_->running = false;
}

bool AppleEngineHost::loadProjectBase64(const std::string& b64) {
    const bool ok = impl_->callBool("__rp_loadProjectB64", {b64});
    if (ok) impl_->refreshPrimaryId();
    return ok;
}
std::string AppleEngineHost::saveProjectBase64() { return impl_->callString("__rp_saveProjectB64"); }
bool AppleEngineHost::loadProjectPath(const std::string& path) {
    const bool ok = impl_->callBool("__rp_loadProjectPath", {path});
    if (ok) impl_->refreshPrimaryId();
    return ok;
}
bool AppleEngineHost::loadRomPath(const std::string& rom, const std::string& sav) {
    JSContext* ctx = impl_->host.context();
    std::vector<JSValue> args{JS_NewString(ctx, rom.c_str()), JS_NewString(ctx, sav.c_str())};
    JSValue result = impl_->call("__rp_appleLoadRom", args);
    for (JSValue arg : args) JS_FreeValue(ctx, arg);
    std::uint32_t id = 0;
    if (!JS_IsException(result)) JS_ToUint32(ctx, &id, result);
    JS_FreeValue(ctx, result);
    if (id) impl_->primaryId.store(id, std::memory_order_release);
    return id != 0;
}
bool AppleEngineHost::loadEmbeddedMgb() {
    const auto id = impl_->callId("__rp_appleLoadMgb");
    if (id) impl_->primaryId.store(id, std::memory_order_release);
    return id != 0;
}
bool AppleEngineHost::loadSramPath(const std::string& path) { return impl_->callBool("__rp_appleLoadSramPath", {path}); }
bool AppleEngineHost::loadStatePath(const std::string& path) { return impl_->callBool("__rp_appleLoadStatePath", {path}); }
bool AppleEngineHost::reset() { return impl_->globalBool("__rp_appleReset"); }
bool AppleEngineHost::setSameBoyConfig(const std::string& json) { return impl_->setRole("sameboy", json); }
bool AppleEngineHost::setMgbConfig(const std::string& json) { return impl_->setRole("mgb", json); }
bool AppleEngineHost::setLsdjConfig(const std::string& json) { return impl_->setRole("lsdj-sync", json); }
void AppleEngineHost::setNoteOutEnabled(bool enabled) {
    if (impl_->noteOutEnabled == enabled) return;
    if (!enabled) for (std::size_t voice = 0; voice < 4; ++voice) impl_->noteOff(voice, 0);
    impl_->noteOutEnabled = enabled;
    if (auto* system = dynamic_cast<SameBoySystem*>(impl_->engine.findSystem(primarySystemId())))
        system->setApuWriteCapture(enabled);
}
void AppleEngineHost::setNoteOutChannel(std::size_t voice, std::uint8_t oneBasedChannel) {
    if (voice < impl_->noteChannels.size()) impl_->noteChannels[voice] = std::uint8_t(std::clamp<int>(oneBasedChannel, 1, 16) - 1);
}
void AppleEngineHost::setNoteOutCcChannel(std::size_t voice, std::uint8_t oneBasedChannel) {
    if (voice < impl_->ccChannels.size()) impl_->ccChannels[voice] = std::uint8_t(std::clamp<int>(oneBasedChannel, 1, 16) - 1);
}
bool AppleEngineHost::setGainDb(double gainDb) {
    JSContext* ctx = impl_->host.context();
    JSValue arg = JS_NewFloat64(ctx, gainDb);
    JSValue result = impl_->call("__rp_appleSetGain", {arg});
    JS_FreeValue(ctx, arg);
    const bool ok = !JS_IsException(result) && JS_ToBool(ctx, result) > 0;
    JS_FreeValue(ctx, result);
    return ok;
}

std::vector<std::uint8_t> AppleEngineHost::saveSram() {
    const std::string b64 = impl_->callString("__rp_appleSaveSramB64");
    // Raw snapshots are available directly; using the canonical control-plane id avoids duplicating
    // base64 decoding in native and returns the exact bytes the project store sees.
    const auto id = primarySystemId();
    auto bytes = id ? impl_->engine.readSram(id) : std::nullopt;
    return !b64.empty() && bytes ? std::move(*bytes) : std::vector<std::uint8_t>{};
}
std::vector<std::uint8_t> AppleEngineHost::snapshotSram() {
    const auto id = impl_->primaryId.load(std::memory_order_acquire);
    auto bytes = id ? impl_->engine.readSram(id) : std::nullopt;
    return bytes ? std::move(*bytes) : std::vector<std::uint8_t>{};
}
std::vector<std::uint8_t> AppleEngineHost::saveState() {
    const auto id = primarySystemId();
    auto bytes = id ? impl_->engine.readState(id) : std::nullopt;
    return bytes ? std::move(*bytes) : std::vector<std::uint8_t>{};
}
std::uint32_t AppleEngineHost::primarySystemId() { return impl_->refreshPrimaryId(); }
bool AppleEngineHost::pressButton(std::uint8_t button, bool down) {
    const auto id = primarySystemId();
    if (!id) return false;
    impl_->invoker.pressButton(id, button, down);
    return true;
}
bool AppleEngineHost::copyFrame(std::uint32_t* destination, std::size_t capacity) {
    const auto id = primarySystemId();
    if (!id) return false;
    const auto frame = impl_->engine.getFrame(id);
    const std::size_t pixels = static_cast<std::size_t>(frame.width) * frame.height;
    if (!frame.published || capacity < pixels || frame.data.size() < pixels * sizeof(std::uint32_t)) return false;
    std::memcpy(destination, frame.data.data(), pixels * sizeof(std::uint32_t));
    return true;
}
void AppleEngineHost::setTransport(bool playing, double bpm, double ppq) {
    impl_->engine.setTransport(playing);
    impl_->engine.setBpm(bpm);
    impl_->engine.setPpq(ppq);
}
void AppleEngineHost::stageMidi(std::uint32_t frame, const std::uint8_t* bytes, std::size_t size) {
    if (!bytes || !size) return;
    impl_->engine.stageMidi(frame, std::vector<std::uint8_t>(bytes, bytes + size));
}
void AppleEngineHost::render(std::uint32_t frames, float* const lanes[10]) {
    impl_->invoker.drainInto(impl_->engine);
    for (std::size_t i = 0; i < 10; ++i) std::fill(lanes[i], lanes[i] + frames, 0.0f);
    float* left[4] = {lanes[2], lanes[4], lanes[6], lanes[8]};
    float* right[4] = {lanes[3], lanes[5], lanes[7], lanes[9]};
    impl_->engine.processBlockPerChannel(frames, left, right, 4);
    if (impl_->noteOutEnabled)
        if (auto* system = dynamic_cast<SameBoySystem*>(impl_->engine.findSystem(primarySystemId())))
            impl_->drainNoteOut(*system);
    for (std::size_t channel = 0; channel < 4; ++channel)
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            lanes[0][frame] += left[channel][frame];
            lanes[1][frame] += right[channel][frame];
        }
}
std::vector<AppleEngineHost::MidiMessage> AppleEngineHost::drainMidiOutput() {
    std::vector<MidiMessage> out = std::move(impl_->appleMidi);
    impl_->appleMidi.clear();
    for (const auto& event : impl_->engine.midiOut()) out.push_back({event.frame, event.data});
    impl_->engine.clearMidiOut();
    return out;
}
