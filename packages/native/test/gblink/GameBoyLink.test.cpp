#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "host/gblink/GameBoyLink.hpp"

namespace {
using namespace retroplug;
struct State { std::mutex mutex; std::vector<std::uint8_t> writes; std::deque<std::uint8_t> reads; };
class FakePort final : public ISerialPort {
public:
    explicit FakePort(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::size_t write(const std::uint8_t* data, std::size_t size) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->writes.insert(state_->writes.end(), data, data + size);
        return size;
    }
    std::size_t read(std::uint8_t* out, std::size_t size, int) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        std::size_t n = 0;
        while (n < size && !state_->reads.empty()) { out[n++] = state_->reads.front(); state_->reads.pop_front(); }
        return n;
    }
    void flushInput() override {}
private:
    std::shared_ptr<State> state_;
};
bool waitFor(const std::function<bool()>& fn) {
    for (int i = 0; i < 200; ++i) {
        if (fn()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}
}

TEST_CASE("Chromatic adapter emits rpsync ASCII off the audio thread", "[gblink]") {
    auto state = std::make_shared<State>();
    GameBoyLink link([state](const std::string&, SerialPortSettings) { return std::make_unique<FakePort>(state); });
    REQUIRE(link.connect("fake", GameBoyLinkAdapter::Chromatic, 2));
    link.setLookaheadMs(0);
    link.push(7, 0, 0xf8, 48000.0);
    REQUIRE(waitFor([&] { return link.bytesSent() == 1; }));
    link.disconnect();
    const std::string text(state->writes.begin(), state->writes.end());
    CHECK(text == "rpsync 2 f8\n");
}

TEST_CASE("GBLink exchanges one raw 19200 8N1 byte and preserves system id", "[gblink]") {
    auto state = std::make_shared<State>();
    state->reads.push_back(0x42);
    SerialPortSettings opened;
    GameBoyLink link([state, &opened](const std::string&, SerialPortSettings settings) {
        opened = settings;
        return std::make_unique<FakePort>(state);
    });
    REQUIRE(link.connect("fake", GameBoyLinkAdapter::GbLink, 1));
    link.setLookaheadMs(0);
    link.push(99, 0, 0xf8, 44100.0);
    REQUIRE(waitFor([&] { return link.bytesReceived() == 1; }));
    const auto rx = link.drainReceived();
    REQUIRE(rx.size() == 1);
    CHECK(rx[0].system == 99);
    CHECK(rx[0].byte == 0x42);
    CHECK(opened.baudRate == 19200);
    CHECK(opened.dataBits == 8);
    CHECK(opened.parity == SerialPortSettings::Parity::None);
    CHECK(opened.stopBits == SerialPortSettings::StopBits::One);
}

TEST_CASE("GBLink timeout is reported and disconnects cleanly", "[gblink]") {
    auto state = std::make_shared<State>();
    GameBoyLink link([state](const std::string&, SerialPortSettings) { return std::make_unique<FakePort>(state); });
    REQUIRE(link.connect("fake", GameBoyLinkAdapter::GbLink, 1));
    link.setLookaheadMs(0);
    link.push(1, 0, 0xaa, 44100.0);
    REQUIRE(waitFor([&] { return link.errors() == 1; }));
    CHECK_FALSE(link.isConnected());
    CHECK(link.lastError().find("timeout") != std::string::npos);
}
