#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "host/n8/Edio.hpp"
#include "host/n8/WjwwoodSerialPort.hpp"
#include "transport/SpscRing.hpp"

namespace retroplug {

enum class GameBoyLinkAdapter : std::uint8_t { Chromatic, GbLink };

struct GameBoyLinkRx {
    std::uint32_t system = 0;
    std::uint8_t  byte   = 0;
};

// RT-safe producer + dedicated UART consumer for a physical Game Boy link adapter. Chromatic accepts
// textual `rpsync` commands; vaguilar/gblink is a raw 19200/8N1 byte exchange and always drives the GB as
// serial master. The returned GBLink byte is diagnostic in rev 1; it is never fed back into a core.
class GameBoyLink {
public:
    using PortFactory = std::function<std::unique_ptr<ISerialPort>(const std::string&, SerialPortSettings)>;

    explicit GameBoyLink(PortFactory factory);
    ~GameBoyLink();
    GameBoyLink(const GameBoyLink&) = delete;
    GameBoyLink& operator=(const GameBoyLink&) = delete;

    bool connect(const std::string& port, GameBoyLinkAdapter adapter, std::uint8_t linkMode);
    void disconnect();
    bool isConnected() const { return connected_.load(std::memory_order_acquire); }

    void setLookaheadMs(int ms);
    int  lookaheadMs() const;
    void push(std::uint32_t system, std::uint32_t sampleOffset, std::uint8_t byte, double sampleRate);
    std::vector<GameBoyLinkRx> drainReceived();

    std::uint64_t bytesSent() const { return bytesSent_.load(std::memory_order_relaxed); }
    std::uint64_t bytesReceived() const { return bytesReceived_.load(std::memory_order_relaxed); }
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t errors() const { return errors_.load(std::memory_order_relaxed); }
    std::string lastError() const;

private:
    struct TimedByte {
        std::int64_t  targetNs = 0;
        std::uint32_t system   = 0;
        std::uint8_t  byte     = 0;
    };

    void serialLoop();
    void fail(const std::string& message);

    PortFactory factory_;
    SpscRing<TimedByte, 2048> tx_;
    SpscRing<GameBoyLinkRx, 2048> rx_;
    std::atomic<std::int64_t> lookaheadNs_{10'000'000};
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> bytesSent_{0}, bytesReceived_{0}, dropped_{0}, errors_{0};
    GameBoyLinkAdapter adapter_ = GameBoyLinkAdapter::Chromatic;
    std::uint8_t linkMode_ = 1;
    mutable std::mutex meta_;
    std::string error_;
    std::unique_ptr<ISerialPort> port_;
    std::thread thread_;
};

} // namespace retroplug
