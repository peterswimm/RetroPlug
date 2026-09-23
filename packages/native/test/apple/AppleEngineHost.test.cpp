#include "AppleEngineHost.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

int main() {
    AppleEngineHost host(48000.0);
    assert(host.ready());
    assert(host.loadProjectBase64(""));
    assert(host.saveProjectBase64().empty());

    constexpr std::uint32_t frames = 64;
    std::array<std::vector<float>, 10> storage;
    float* lanes[10];
    for (std::size_t i = 0; i < storage.size(); ++i) {
        storage[i].resize(frames);
        lanes[i] = storage[i].data();
    }
    host.resume();
    host.setTransport(false, 120.0, 0.0);
    host.render(frames, lanes); // empty project is a valid silent graph
    host.suspend();

    // Development builds embed mGB, which gives the portable host test a real system while the
    // iOS distribution build independently proves that the ROM can be compiled out.
    assert(host.loadEmbeddedMgb());
    const auto project = host.saveProjectBase64();
    assert(!project.empty());
    assert(host.loadProjectBase64(project));
    assert(host.primarySystemId() != 0);

    const std::uint8_t noteOn[] = {0x90, 60, 100};
    host.stageMidi(0, noteOn, sizeof(noteOn));
    host.resume();
    bool heardStem = false;
    for (int block = 0; block < 32 && !heardStem; ++block) {
        host.render(frames, lanes);
        for (std::size_t lane = 2; lane < storage.size(); ++lane)
            for (float sample : storage[lane]) heardStem |= std::fabs(sample) > 1.0e-6f;
    }
    host.suspend();
    assert(heardStem);
    return 0;
}
