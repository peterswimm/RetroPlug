#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <vector>

// Apple lifecycle adapter for the canonical RetroPlug Engine + TypeScript control plane.
// Swift/Objective-C++ never owns an emulator core: ROM classification, project migrations, roles,
// and persistence all enter through the same control-plane bundle as the desktop plugin.
class AppleEngineHost final {
public:
    struct MidiMessage {
        std::uint32_t frame = 0;
        std::vector<std::uint8_t> bytes;
    };

    explicit AppleEngineHost(double sampleRate);
    ~AppleEngineHost();
    AppleEngineHost(const AppleEngineHost&) = delete;
    AppleEngineHost& operator=(const AppleEngineHost&) = delete;

    bool ready() const;
    void setSampleRate(double sampleRate);
    void resume();
    void suspend();

    bool loadProjectBase64(const std::string& base64);
    std::string saveProjectBase64();
    bool loadProjectPath(const std::string& path);
    bool loadRomPath(const std::string& romPath, const std::string& savPath = {});
    bool loadEmbeddedMgb(); // false in distribution builds where mGB is deliberately absent
    bool loadSramPath(const std::string& path);
    bool loadStatePath(const std::string& path);
    std::vector<std::uint8_t> saveSram();
    std::vector<std::uint8_t> saveState();
    bool reset();
    bool setGainDb(double gainDb);
    bool setSameBoyConfig(const std::string& configJson);
    bool setLsdjConfig(const std::string& configJson);
    void setNoteOutEnabled(bool enabled);
    void setNoteOutChannel(std::size_t voice, std::uint8_t oneBasedChannel);

    std::uint32_t primarySystemId();
    bool pressButton(std::uint8_t button, bool down);
    bool copyFrame(std::uint32_t* destination, std::size_t pixelCapacity);

    void setTransport(bool playing, double bpm, double ppq);
    void stageMidi(std::uint32_t frame, const std::uint8_t* bytes, std::size_t size);

    // Exactly one call per AU render timestamp. lanes[0..1] is the mix and lanes[2..9] are
    // Pulse 1/Pulse 2/Wave/Noise stereo pairs. The Engine renders the four channel pairs once;
    // this adapter derives the sum without asking the emulator to advance again for another bus.
    void render(std::uint32_t frames, float* const lanes[10]);
    std::vector<MidiMessage> drainMidiOutput();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
