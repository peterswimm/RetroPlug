#include "host/gblink/GameBoyLink.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace retroplug {
namespace {
using Steady = std::chrono::steady_clock;
std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Steady::now().time_since_epoch()).count();
}
}

GameBoyLink::GameBoyLink(PortFactory factory) : factory_(std::move(factory)) {}
GameBoyLink::~GameBoyLink() { disconnect(); }

void GameBoyLink::setLookaheadMs(int ms) {
    lookaheadNs_.store(static_cast<std::int64_t>(std::max(0, ms)) * 1'000'000, std::memory_order_relaxed);
}
int GameBoyLink::lookaheadMs() const {
    return static_cast<int>(lookaheadNs_.load(std::memory_order_relaxed) / 1'000'000);
}

std::string GameBoyLink::lastError() const {
    std::lock_guard<std::mutex> lock(meta_);
    return error_;
}

void GameBoyLink::fail(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(meta_);
        error_ = message;
    }
    errors_.fetch_add(1, std::memory_order_relaxed);
    connected_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);
}

bool GameBoyLink::connect(const std::string& name, GameBoyLinkAdapter adapter, std::uint8_t linkMode) {
    disconnect();
    SerialPortSettings settings;
    settings.baudRate = adapter == GameBoyLinkAdapter::GbLink ? 19200u : 115200u;
    try {
        port_ = factory_(name, settings);
        if (!port_) throw std::runtime_error("serial factory returned no port");
        port_->flushInput();
    } catch (const std::exception& e) {
        fail(e.what());
        port_.reset();
        return false;
    }
    adapter_ = adapter;
    linkMode_ = linkMode;
    { TimedByte b; while (tx_.tryPop(b)) {} }
    { GameBoyLinkRx b; while (rx_.tryPop(b)) {} }
    bytesSent_.store(0, std::memory_order_relaxed);
    bytesReceived_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    errors_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(meta_);
        error_.clear();
    }
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&GameBoyLink::serialLoop, this);
    connected_.store(true, std::memory_order_release);
    return true;
}

void GameBoyLink::disconnect() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    connected_.store(false, std::memory_order_release);
    port_.reset();
}

void GameBoyLink::push(std::uint32_t system, std::uint32_t sampleOffset, std::uint8_t byte, double sampleRate) {
    if (!connected_.load(std::memory_order_acquire)) return;
    TimedByte ev;
    ev.system = system;
    ev.byte = byte;
    const std::int64_t offset = sampleRate > 0.0
        ? static_cast<std::int64_t>(static_cast<double>(sampleOffset) * 1e9 / sampleRate) : 0;
    ev.targetNs = nowNs() + offset + lookaheadNs_.load(std::memory_order_relaxed);
    if (!tx_.tryPush(ev)) dropped_.fetch_add(1, std::memory_order_relaxed);
}

std::vector<GameBoyLinkRx> GameBoyLink::drainReceived() {
    std::vector<GameBoyLinkRx> out;
    GameBoyLinkRx ev;
    while (rx_.tryPop(ev)) out.push_back(ev);
    return out;
}

void GameBoyLink::serialLoop() {
    TimedByte ev;
    while (running_.load(std::memory_order_acquire)) {
        if (!tx_.tryPop(ev)) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }
        while (running_.load(std::memory_order_acquire) && nowNs() < ev.targetNs) {
            const auto remaining = ev.targetNs - nowNs();
            std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<std::int64_t>(remaining, 2'000'000)));
        }
        if (!running_.load(std::memory_order_acquire)) break;
        try {
            if (adapter_ == GameBoyLinkAdapter::Chromatic) {
                char line[32];
                const int n = std::snprintf(line, sizeof line, "rpsync %x %x\n", linkMode_, ev.byte);
                if (n <= 0 || port_->write(reinterpret_cast<const std::uint8_t*>(line), static_cast<std::size_t>(n))
                                  != static_cast<std::size_t>(n))
                    throw std::runtime_error("short Chromatic write");
            } else {
                if (port_->write(&ev.byte, 1) != 1) throw std::runtime_error("short GBLink write");
                std::uint8_t reply = 0;
                if (port_->read(&reply, 1, 100) != 1) throw std::runtime_error("GBLink response timeout");
                if (!rx_.tryPush({ev.system, reply})) dropped_.fetch_add(1, std::memory_order_relaxed);
                else bytesReceived_.fetch_add(1, std::memory_order_relaxed);
            }
            bytesSent_.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception& e) {
            fail(std::string("Game Boy link I/O failed: ") + e.what());
            break;
        }
    }
}

} // namespace retroplug
