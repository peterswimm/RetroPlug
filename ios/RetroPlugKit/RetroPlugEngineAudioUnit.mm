#import "RetroPlugAudioUnit.h"
#import "RetroPlugCoreBridge.h"

#include "AppleEngineHost.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

NSErrorDomain const RPCoreBridgeErrorDomain = @"RetroPlugCoreBridge";
const NSUInteger RPScreenWidth = 160, RPScreenHeight = 144;
const NSUInteger RPMidiChannelSettingCount = 17, RPMidiOutVoiceCount = 4, RPMidiOutCcNumberCount = 7;

namespace {
constexpr AUAudioFrameCount kMaxFrames = 4096;
constexpr std::size_t kBusCount = 5, kLaneCount = 10;
constexpr AUParameterAddress kMode = 0, kDivisor = 1, kAutoStart = 2;
static NSString* const kProjectKey = @"retroplug-project-b64";

struct RenderState {
    std::unique_ptr<AppleEngineHost> host;
    std::array<std::vector<float>, kLaneCount> lanes;
    double sampleTime = std::numeric_limits<double>::quiet_NaN();
    AUHostMusicalContextBlock musical = nil;
    AUHostTransportStateBlock transport = nil;
    AUMIDIOutputEventBlock midiOut = nil;
};

NSError* RPError(RPCoreBridgeError code, NSString* text) {
    return [NSError errorWithDomain:RPCoreBridgeErrorDomain code:code
                           userInfo:@{NSLocalizedDescriptionKey: text}];
}

std::string RPTemporaryFile(NSData* data, NSString* extension) {
    NSString* file = [NSString stringWithFormat:@"retroplug-%@.%@", NSUUID.UUID.UUIDString, extension];
    NSString* path = [NSTemporaryDirectory() stringByAppendingPathComponent:file];
    return [data writeToFile:path atomically:YES] ? path.UTF8String : std::string{};
}

void RPCopy(const std::vector<float>& left, const std::vector<float>& right,
            AudioBufferList* output, AUAudioFrameCount frames) {
    if (!output || !output->mNumberBuffers) return;
    if (output->mNumberBuffers >= 2) {
        if (output->mBuffers[0].mData) std::memcpy(output->mBuffers[0].mData, left.data(), frames * sizeof(float));
        else output->mBuffers[0].mData = const_cast<float*>(left.data());
        if (output->mBuffers[1].mData) std::memcpy(output->mBuffers[1].mData, right.data(), frames * sizeof(float));
        else output->mBuffers[1].mData = const_cast<float*>(right.data());
    } else if (output->mBuffers[0].mData) {
        float* dst = static_cast<float*>(output->mBuffers[0].mData);
        for (AUAudioFrameCount i = 0; i < frames; ++i) { dst[2*i] = left[i]; dst[2*i+1] = right[i]; }
    }
}
} // namespace

@implementation RetroPlugAudioUnit {
    std::unique_ptr<RenderState> _state;
    AUAudioUnitBusArray* _outputs;
    AUParameterTree* _parameters;
}

- (nullable instancetype)initWithComponentDescription:(AudioComponentDescription)desc
                                               options:(AudioComponentInstantiationOptions)options
                                                 error:(NSError**)error {
    self = [super initWithComponentDescription:desc options:options error:error];
    if (!self) return nil;
    _state = std::make_unique<RenderState>();
    _state->host = std::make_unique<AppleEngineHost>(44100.0);
    if (!_state->host->ready()) { if (error) *error = RPError(RPCoreBridgeErrorNoSystem, @"Control plane failed to start."); return nil; }

    AVAudioFormat* format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100 channels:2];
    NSArray* names = @[@"Mix", @"Pulse 1", @"Pulse 2", @"Wave", @"Noise"];
    NSMutableArray* busses = [NSMutableArray arrayWithCapacity:kBusCount];
    for (NSUInteger i = 0; i < kBusCount; ++i) {
        AUAudioUnitBus* bus = [[AUAudioUnitBus alloc] initWithFormat:format error:error];
        if (!bus) return nil; bus.name = names[i]; bus.maximumChannelCount = 2; [busses addObject:bus];
    }
    _outputs = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self busType:AUAudioUnitBusTypeOutput busses:busses];
    self.maximumFramesToRender = kMaxFrames;
    const AudioUnitParameterOptions rw = kAudioUnitParameterFlag_IsWritable | kAudioUnitParameterFlag_IsReadable;
    AUParameter* mode = [AUParameterTree createParameterWithIdentifier:@"syncMode" name:@"MIDI Mode" address:kMode
        min:0 max:8 unit:kAudioUnitParameterUnit_Indexed unitName:nil flags:rw
        valueStrings:@[@"Off", @"mGB", @"MIDI Sync", @"Arduinoboy", @"MIDI Map", @"Keyboard MIDI", @"MIDI Out", @"Master Sync", @"Note Out"] dependentParameters:nil];
    AUParameter* divisor = [AUParameterTree createParameterWithIdentifier:@"syncTempoDivisor" name:@"Tempo Divisor" address:kDivisor
        min:1 max:8 unit:kAudioUnitParameterUnit_Indexed unitName:nil flags:rw valueStrings:nil dependentParameters:nil];
    AUParameter* autoStart = [AUParameterTree createParameterWithIdentifier:@"syncAutoStart" name:@"Auto Start" address:kAutoStart
        min:0 max:1 unit:kAudioUnitParameterUnit_Boolean unitName:nil flags:rw valueStrings:nil dependentParameters:nil];
    _parameters = [AUParameterTree createTreeWithChildren:@[mode, divisor, autoStart]];
    mode.value = RPMidiSyncModeMgb; divisor.value = 1;
    __weak RetroPlugAudioUnit* weakSelf = self;
    _parameters.implementorValueObserver = ^(AUParameter*, AUValue) { [weakSelf rpApplySync]; };
    return self;
}

- (AUParameterTree*)parameterTree { return _parameters; }
- (AUAudioUnitBusArray*)outputBusses { return _outputs; }
- (NSArray<NSString*>*)MIDIOutputNames { return @[@"MIDI Out"]; }

- (void)rpApplySync {
    if (!_state || !_state->host) return;
    NSArray* modes = @[@"off", @"midiPassthrough", @"midiSync", @"midiSyncArduinoboy",
                       @"midiMap", @"keyboardMidi", @"midiOut", @"masterSync", @"off"];
    NSInteger index = std::clamp<NSInteger>((NSInteger)[_parameters parameterWithAddress:kMode].value, 0, 8);
    NSUInteger divisor = std::max<NSUInteger>(1, (NSUInteger)[_parameters parameterWithAddress:kDivisor].value);
    BOOL autoStart = [_parameters parameterWithAddress:kAutoStart].value >= 0.5f;
    NSString* json = [NSString stringWithFormat:@"{\"mode\":\"%@\",\"tempoDivisor\":%lu,\"autoStart\":%@}",
                      modes[index], (unsigned long)divisor, autoStart ? @"true" : @"false"];
    _state->host->setLsdjConfig(json.UTF8String);
    _state->host->setNoteOutEnabled(index == RPMidiSyncModeNoteOut);
}

- (BOOL)allocateRenderResourcesAndReturnError:(NSError**)error {
    if (![super allocateRenderResourcesAndReturnError:error]) return NO;
    double sr = _outputs[0].format.sampleRate;
    _state->host->setSampleRate(sr);
    for (auto& lane : _state->lanes) lane.assign(kMaxFrames, 0.0f);
    _state->sampleTime = std::numeric_limits<double>::quiet_NaN();
    _state->musical = self.musicalContextBlock; _state->transport = self.transportStateBlock;
    _state->midiOut = self.MIDIOutputEventBlock; _state->host->resume();
    return YES;
}

- (void)deallocateRenderResources {
    _state->host->suspend(); _state->musical = nil; _state->transport = nil; _state->midiOut = nil;
    [super deallocateRenderResources];
}

- (AUInternalRenderBlock)internalRenderBlock {
    RenderState* state = _state.get();
    return ^AUAudioUnitStatus(AudioUnitRenderActionFlags*, const AudioTimeStamp* timestamp,
                              AUAudioFrameCount frames, NSInteger bus, AudioBufferList* output,
                              const AURenderEvent* events, AURenderPullInputBlock) {
        if (frames > kMaxFrames || bus < 0 || bus >= (NSInteger)kBusCount) return kAudioUnitErr_InvalidElement;
        if (state->sampleTime != timestamp->mSampleTime) {
            state->sampleTime = timestamp->mSampleTime;
            for (const AURenderEvent* event = events; event; event = event->head.next) if (event->head.eventType == AURenderEventMIDI) {
                const AUMIDIEvent& midi = event->MIDI;
                std::uint32_t offset = midi.eventSampleTime > timestamp->mSampleTime
                    ? (std::uint32_t)std::min<double>(frames - 1, midi.eventSampleTime - timestamp->mSampleTime) : 0;
                state->host->stageMidi(offset, midi.data, midi.length);
            }
            double bpm = 120.0, ppq = 0.0; bool playing = false;
            if (state->musical) { double numerator = 4, beat = 0; NSInteger denominator = 4;
                state->musical(&bpm, &numerator, &denominator, &beat, nullptr, nullptr); ppq = beat; }
            if (state->transport) { AUHostTransportStateFlags flags = 0; double sample = 0, begin = 0, end = 0;
                state->transport(&flags, &sample, &begin, &end); playing = (flags & AUHostTransportStateMoving) != 0; }
            state->host->setTransport(playing, bpm, ppq);
            float* lanes[kLaneCount]; for (std::size_t i = 0; i < kLaneCount; ++i) lanes[i] = state->lanes[i].data();
            state->host->render(frames, lanes);
            if (state->midiOut) for (const auto& message : state->host->drainMidiOutput())
                state->midiOut((AUEventSampleTime)timestamp->mSampleTime + message.frame, 0,
                               (NSInteger)message.bytes.size(), message.bytes.data());
        }
        std::size_t pair = bus == 0 ? 0 : 2 * (std::size_t)bus;
        RPCopy(state->lanes[pair], state->lanes[pair+1], output, frames);
        return noErr;
    };
}

- (NSDictionary<NSString*, id>*)fullState {
    NSMutableDictionary* state = [NSMutableDictionary dictionaryWithDictionary:[super fullState] ?: @{}];
    std::string project = _state->host->saveProjectBase64();
    if (!project.empty()) state[kProjectKey] = [NSString stringWithUTF8String:project.c_str()];
    return state;
}
- (void)setFullState:(NSDictionary<NSString*, id>*)state {
    [super setFullState:state];
    NSString* project = [state[kProjectKey] isKindOfClass:NSString.class] ? state[kProjectKey] : nil;
    if (project) { _state->host->loadProjectBase64(project.UTF8String); [self rpApplySync]; }
}

- (BOOL)hasSystem { return _state->host->primarySystemId() != 0; }
- (BOOL)loadRomData:(NSData*)rom sram:(NSData*)sram state:(NSData*)saved error:(NSError**)error {
    if (!rom.length) { if (error) *error = RPError(RPCoreBridgeErrorEmptyRom, @"ROM data is empty."); return NO; }
    std::string romPath = RPTemporaryFile(rom, @"gb"), savPath = sram.length ? RPTemporaryFile(sram, @"sav") : std::string{};
    BOOL ok = !romPath.empty() && _state->host->loadRomPath(romPath, savPath);
    if (ok && saved.length) ok = _state->host->loadStatePath(RPTemporaryFile(saved, @"state"));
    if (!ok && error) *error = RPError(RPCoreBridgeErrorStateRejected, @"The control plane rejected this cartridge or state.");
    [self rpApplySync]; return ok;
}
- (BOOL)loadEmbeddedMGBWithSram:(NSData*)sram error:(NSError**)error {
    BOOL ok = _state->host->loadEmbeddedMgb();
    if (ok && sram.length) ok = _state->host->loadSramPath(RPTemporaryFile(sram, @"sav"));
    if (!ok && error) *error = RPError(RPCoreBridgeErrorNoSystem, @"mGB is not bundled. Import a ROM you are licensed to use.");
    [self rpApplySync]; return ok;
}
- (NSData*)saveSram { auto b = _state->host->saveSram(); return b.empty() ? nil : [NSData dataWithBytes:b.data() length:b.size()]; }
- (NSData*)saveState { auto b = _state->host->saveState(); return b.empty() ? nil : [NSData dataWithBytes:b.data() length:b.size()]; }
- (NSData*)snapshotSramForAutosave { return [self saveSram]; }
- (BOOL)loadState:(NSData*)data error:(NSError**)error { BOOL ok = data.length && _state->host->loadStatePath(RPTemporaryFile(data,@"state")); if (!ok&&error)*error=RPError(RPCoreBridgeErrorStateRejected,@"Savestate rejected."); return ok; }
- (BOOL)loadSram:(NSData*)data error:(NSError**)error { BOOL ok = data.length && _state->host->loadSramPath(RPTemporaryFile(data,@"sav")); if (!ok&&error)*error=RPError(RPCoreBridgeErrorSramRejected,@"Battery RAM rejected."); return ok; }
- (BOOL)setModel:(RPSameBoyModel)model error:(NSError**)error {
    NSArray* names=@[@"auto",@"dmgB",@"mgb",@"sgb",@"sgbPal",@"sgb2",@"cgb0",@"cgbA",@"cgbB",@"cgbC",@"cgbD",@"cgbE",@"agb",@"gbp"];
    BOOL ok=model<names.count&&_state->host->setSameBoyConfig([[NSString stringWithFormat:@"{\"model\":\"%@\"}",names[model]] UTF8String]);
    if(!ok&&error)*error=RPError(RPCoreBridgeErrorNoSystem,@"No Game Boy system is running."); return ok;
}
- (void)pressButton:(RPGameboyButton)b down:(BOOL)down { _state->host->pressButton((uint8_t)b,down); }
- (void)resetEmulator { _state->host->reset(); }
- (void)setGainDb:(float)dB { _state->host->setGainDb(dB); }
- (void)setFastBoot:(BOOL)on { _state->host->setSameBoyConfig(on?"{\"fastBoot\":true}":"{\"fastBoot\":false}"); }
- (void)setMidiSyncMode:(RPMidiSyncMode)m { [_parameters parameterWithAddress:kMode].value=m; [self rpApplySync]; }
- (void)setSyncTempoDivisor:(NSUInteger)d { [_parameters parameterWithAddress:kDivisor].value=d; [self rpApplySync]; }
- (void)setSyncAutoStart:(BOOL)on { [_parameters parameterWithAddress:kAutoStart].value=on; [self rpApplySync]; }
- (void)setMidiChannel:(NSUInteger)c forSetting:(RPMidiChannelSetting)s {
    if (s >= RPMidiChannelSettingMidiOutNotePu1 && s <= RPMidiChannelSettingMidiOutNoteNoi)
        _state->host->setNoteOutChannel((std::size_t)s - RPMidiChannelSettingMidiOutNotePu1, (std::uint8_t)c);
}
- (void)setMgbBaseChannel:(NSUInteger)b { (void)b; }
- (void)setMidiOutCcMode:(RPMidiOutCcMode)m forVoice:(NSUInteger)v { (void)m;(void)v; }
- (void)setMidiOutCcScaling:(BOOL)s forVoice:(NSUInteger)v { (void)s;(void)v; }
- (void)setMidiOutCcNumber:(NSUInteger)c atIndex:(NSUInteger)i forVoice:(NSUInteger)v { (void)c;(void)i;(void)v; }
- (BOOL)copyFrameInto:(uint32_t*)dst capacityPixels:(NSUInteger)n { return _state->host->copyFrame(dst,n); }
@end
