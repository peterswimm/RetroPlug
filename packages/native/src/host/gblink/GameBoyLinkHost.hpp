#pragma once

#include <functional>
#include <string>
#include <vector>

#include "host/gblink/GameBoyLink.hpp"

namespace retroplug {

struct GameBoyLinkPortDto { std::string port; };

struct GameBoyLinkConfigDto {
    std::vector<GameBoyLinkPortDto> ports;
    std::string selectedPort;
    std::string adapter = "chromatic";
    bool connected = false;
    bool enabled = false;
    int lookaheadMs = 10;
    int linkMode = 1;
    std::uint32_t baudRate = 115200;
    std::uint8_t dataBits = 8;
    std::string parity = "none";
    std::uint8_t stopBits = 1;
    std::uint64_t bytesSent = 0;
    std::uint64_t bytesReceived = 0;
    std::uint64_t dropped = 0;
    std::uint64_t errors = 0;
    std::string error;
};

class GameBoyLinkHost {
public:
    using PortLister = std::function<std::vector<GameBoyLinkPortDto>()>;
    GameBoyLinkHost(GameBoyLink::PortFactory factory, PortLister lister, std::string configDir);

    GameBoyLink& link() { return link_; }
    GameBoyLinkConfigDto getConfig();
    void setPort(const std::string& port);
    void setAdapter(const std::string& adapter);
    void setLookahead(int ms);
    void setLinkMode(int mode);
    void connect(bool enabled);
    std::vector<GameBoyLinkRx> drainReceived() { return link_.drainReceived(); }
    void restore();

private:
    void reconnect();
    void save();
    GameBoyLink link_;
    PortLister lister_;
    std::string configDir_, port_, adapter_ = "chromatic";
    bool enabled_ = false;
    int linkMode_ = 1;
};

} // namespace retroplug
