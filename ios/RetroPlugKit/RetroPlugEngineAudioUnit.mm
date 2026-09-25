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
constexpr AUParameterAddress kChannelBase = 16, kCcModeBase = 40, kCcScalingBase = 44;
constexpr AUParameterAddress kCcNumberBase = 48, kMgbBaseChannel = 80;
constexpr std::uint8_t kChannelDefaults[17] = {
    1, 1, 1, 1, 1, 2, 3, 4, 5, 1, 2, 3, 4, 1, 2, 3, 4,
};
constexpr std::uint8_t kCcNumberDefaults[7] = {1, 2, 3, 7, 10, 11, 12};
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

std::string RPJson(id object) {
    NSData* data = [NSJSONSerialization dataWithJSONObject:object options:0 error:nil];
    if (!data) return {};
    return std::string(static_cast<const char*>(data.bytes), data.length);
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
    if (!_state->host->ready()) {
        if (error) {
            const std::string& detail = _state->host->startupError();
            NSString* message = detail.empty() ? @"Control plane failed to start."
                                               : [NSString stringWithUTF8String:detail.c_str()];
            *error = RPError(RPCoreBridgeErrorNoSystem, message);
        }
        return nil;
    }

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
    NSArray<NSString*>* channelIds = @[
        @"chSlaveSync", @"chMasterSync", @"chKeyboard", @"chMidiMap",
        @"chMgbPu1", @"chMgbPu2", @"chMgbWav", @"chMgbNoi", @"chMgbPoly",
        @"chMidiOutNotePu1", @"chMidiOutNotePu2", @"chMidiOutNoteWav", @"chMidiOutNoteNoi",
        @"chMidiOutCcPu1", @"chMidiOutCcPu2", @"chMidiOutCcWav", @"chMidiOutCcNoi",
    ];
    NSArray<NSString*>* channelNames = @[
        @"Slave Sync Channel", @"Master Sync Channel", @"Keyboard Channel", @"MIDI Map Channel",
        @"mGB PU1 Channel", @"mGB PU2 Channel", @"mGB WAV Channel", @"mGB NOI Channel", @"mGB POLY Channel",
        @"MIDI Out PU1 Note Ch", @"MIDI Out PU2 Note Ch", @"MIDI Out WAV Note Ch", @"MIDI Out NOI Note Ch",
        @"MIDI Out PU1 CC Ch", @"MIDI Out PU2 CC Ch", @"MIDI Out WAV CC Ch", @"MIDI Out NOI CC Ch",
    ];
    NSMutableArray<AUParameter*>* params = [NSMutableArray arrayWithObjects:mode, divisor, autoStart, nil];
    for (NSUInteger i = 0; i < RPMidiChannelSettingCount; ++i)
        [params addObject:[AUParameterTree createParameterWithIdentifier:channelIds[i] name:channelNames[i]
            address:kChannelBase+i min:1 max:16 unit:kAudioUnitParameterUnit_Indexed unitName:nil
            flags:rw valueStrings:nil dependentParameters:nil]];

    NSArray<NSString*>* voiceIds = @[@"Pu1", @"Pu2", @"Wav", @"Noi"];
    NSArray<NSString*>* voiceNames = @[@"PU1", @"PU2", @"WAV", @"NOI"];
    for (NSUInteger voice = 0; voice < RPMidiOutVoiceCount; ++voice) {
        [params addObject:[AUParameterTree createParameterWithIdentifier:
            [NSString stringWithFormat:@"ccMode%@", voiceIds[voice]]
            name:[NSString stringWithFormat:@"%@ CC Mode", voiceNames[voice]] address:kCcModeBase+voice
            min:0 max:1 unit:kAudioUnitParameterUnit_Indexed unitName:nil flags:rw
            valueStrings:@[@"Single CC", @"7-CC Select"] dependentParameters:nil]];
        [params addObject:[AUParameterTree createParameterWithIdentifier:
            [NSString stringWithFormat:@"ccScaling%@", voiceIds[voice]]
            name:[NSString stringWithFormat:@"%@ CC Scaling", voiceNames[voice]] address:kCcScalingBase+voice
            min:0 max:1 unit:kAudioUnitParameterUnit_Boolean unitName:nil flags:rw
            valueStrings:nil dependentParameters:nil]];
        for (NSUInteger index = 0; index < RPMidiOutCcNumberCount; ++index)
            [params addObject:[AUParameterTree createParameterWithIdentifier:
                [NSString stringWithFormat:@"ccNum%@_%lu", voiceIds[voice], (unsigned long)index]
                name:[NSString stringWithFormat:@"%@ CC#%lu", voiceNames[voice], (unsigned long)index]
                address:kCcNumberBase+voice*RPMidiOutCcNumberCount+index min:0 max:127
                unit:kAudioUnitParameterUnit_Indexed unitName:nil flags:rw valueStrings:nil dependentParameters:nil]];
    }
    [params addObject:[AUParameterTree createParameterWithIdentifier:@"chMgbBase" name:@"mGB Base Channel"
        address:kMgbBaseChannel min:0 max:12 unit:kAudioUnitParameterUnit_Indexed unitName:nil
        flags:rw valueStrings:nil dependentParameters:nil]];

    _parameters = [AUParameterTree createTreeWithChildren:params];
    mode.value = RPMidiSyncModeMgb;
    divisor.value = 1;
    autoStart.value = 0;
    for (NSUInteger i = 0; i < RPMidiChannelSettingCount; ++i)
        [_parameters parameterWithAddress:kChannelBase+i].value = kChannelDefaults[i];
    for (NSUInteger voice = 0; voice < RPMidiOutVoiceCount; ++voice) {
        [_parameters parameterWithAddress:kCcModeBase+voice].value = RPMidiOutCcModeMulti;
        [_parameters parameterWithAddress:kCcScalingBase+voice].value = 1;
        for (NSUInteger index = 0; index < RPMidiOutCcNumberCount; ++index)
            [_parameters parameterWithAddress:kCcNumberBase+voice*RPMidiOutCcNumberCount+index].value = kCcNumberDefaults[index];
    }
    [_parameters parameterWithAddress:kMgbBaseChannel].value = 0;
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
    NSMutableArray<NSNumber*>* channels = [NSMutableArray arrayWithCapacity:RPMidiChannelSettingCount];
    for (NSUInteger i = 0; i < RPMidiChannelSettingCount; ++i)
        [channels addObject:@((NSUInteger)std::clamp<AUValue>([_parameters parameterWithAddress:kChannelBase+i].value, 1, 16))];
    NSMutableArray<NSString*>* ccModes = [NSMutableArray arrayWithCapacity:RPMidiOutVoiceCount];
    NSMutableArray<NSNumber*>* ccScaling = [NSMutableArray arrayWithCapacity:RPMidiOutVoiceCount];
    NSMutableArray<NSNumber*>* ccNumbers = [NSMutableArray arrayWithCapacity:RPMidiOutVoiceCount*RPMidiOutCcNumberCount];
    for (NSUInteger voice = 0; voice < RPMidiOutVoiceCount; ++voice) {
        [ccModes addObject:[_parameters parameterWithAddress:kCcModeBase+voice].value >= 0.5f ? @"multi" : @"single"];
        [ccScaling addObject:@([_parameters parameterWithAddress:kCcScalingBase+voice].value >= 0.5f)];
        for (NSUInteger cc = 0; cc < RPMidiOutCcNumberCount; ++cc)
            [ccNumbers addObject:@((NSUInteger)std::clamp<AUValue>(
                [_parameters parameterWithAddress:kCcNumberBase+voice*RPMidiOutCcNumberCount+cc].value, 0, 127))];
    }
    const NSUInteger mgbBase = (NSUInteger)std::clamp<AUValue>(
        [_parameters parameterWithAddress:kMgbBaseChannel].value, 0, 12);
    NSArray* mgbChannels = [channels subarrayWithRange:NSMakeRange(RPMidiChannelSettingMgbPu1, 5)];
    NSDictionary* mgb = @{ @"channels": mgbChannels, @"baseChannel": @(mgbBase) };
    NSDictionary* lsdj = @{
        @"mode": modes[index], @"tempoDivisor": @(divisor), @"autoStart": @(autoStart),
        @"slaveChannel": channels[RPMidiChannelSettingArduinoboySlave],
        @"masterSyncChannel": channels[RPMidiChannelSettingMasterSync],
        @"keyboardChannel": channels[RPMidiChannelSettingKeyboard],
        @"midiMapChannel": channels[RPMidiChannelSettingMidiMap],
        @"midiOutNoteChannels": [channels subarrayWithRange:NSMakeRange(RPMidiChannelSettingMidiOutNotePu1, 4)],
        @"midiOutCcChannels": [channels subarrayWithRange:NSMakeRange(RPMidiChannelSettingMidiOutCcPu1, 4)],
        @"midiOutCcModes": ccModes, @"midiOutCcScaling": ccScaling, @"midiOutCcNumbers": ccNumbers,
    };
    _state->host->setMgbConfig(RPJson(mgb));
    _state->host->setLsdjConfig(RPJson(lsdj));
    for (NSUInteger voice = 0; voice < RPMidiOutVoiceCount; ++voice) {
        _state->host->setNoteOutChannel(voice, channels[RPMidiChannelSettingMidiOutNotePu1+voice].unsignedCharValue);
        _state->host->setNoteOutCcChannel(voice, channels[RPMidiChannelSettingMidiOutCcPu1+voice].unsignedCharValue);
    }
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
- (NSData*)snapshotSramForAutosave { auto b = _state->host->snapshotSram(); return b.empty() ? nil : [NSData dataWithBytes:b.data() length:b.size()]; }
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
    if (s >= RPMidiChannelSettingCount) return;
    [_parameters parameterWithAddress:kChannelBase+s].value = (AUValue)std::clamp<NSUInteger>(c, 1, 16);
}
- (void)setMgbBaseChannel:(NSUInteger)b { [_parameters parameterWithAddress:kMgbBaseChannel].value = (AUValue)std::min<NSUInteger>(b, 12); }
- (void)setMidiOutCcMode:(RPMidiOutCcMode)m forVoice:(NSUInteger)v {
    if (v < RPMidiOutVoiceCount) [_parameters parameterWithAddress:kCcModeBase+v].value = m == RPMidiOutCcModeMulti ? 1 : 0;
}
- (void)setMidiOutCcScaling:(BOOL)s forVoice:(NSUInteger)v {
    if (v < RPMidiOutVoiceCount) [_parameters parameterWithAddress:kCcScalingBase+v].value = s ? 1 : 0;
}
- (void)setMidiOutCcNumber:(NSUInteger)c atIndex:(NSUInteger)i forVoice:(NSUInteger)v {
    if (v < RPMidiOutVoiceCount && i < RPMidiOutCcNumberCount)
        [_parameters parameterWithAddress:kCcNumberBase+v*RPMidiOutCcNumberCount+i].value = (AUValue)std::min<NSUInteger>(c, 127);
}
- (BOOL)copyFrameInto:(uint32_t*)dst capacityPixels:(NSUInteger)n { return _state->host->copyFrame(dst,n); }
@end
