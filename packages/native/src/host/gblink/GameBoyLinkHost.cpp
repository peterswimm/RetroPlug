#include "host/gblink/GameBoyLinkHost.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace retroplug {

GameBoyLinkHost::GameBoyLinkHost(GameBoyLink::PortFactory factory, PortLister lister, std::string configDir)
    : link_(std::move(factory)), lister_(std::move(lister)), configDir_(std::move(configDir)) {}

GameBoyLinkConfigDto GameBoyLinkHost::getConfig() {
    GameBoyLinkConfigDto c;
    c.ports = lister_ ? lister_() : std::vector<GameBoyLinkPortDto>{};
    c.selectedPort = port_;
    c.adapter = adapter_;
    c.connected = link_.isConnected();
    c.enabled = enabled_;
    c.lookaheadMs = link_.lookaheadMs();
    c.linkMode = linkMode_;
    c.baudRate = adapter_ == "gblink" ? 19200u : 115200u;
    c.bytesSent = link_.bytesSent();
    c.bytesReceived = link_.bytesReceived();
    c.dropped = link_.dropped();
    c.errors = link_.errors();
    c.error = link_.lastError();
    return c;
}

void GameBoyLinkHost::reconnect() {
    link_.disconnect();
    if (enabled_ && !port_.empty())
        link_.connect(port_, adapter_ == "gblink" ? GameBoyLinkAdapter::GbLink : GameBoyLinkAdapter::Chromatic,
                      static_cast<std::uint8_t>(std::clamp(linkMode_, 0, 8)));
}

void GameBoyLinkHost::setPort(const std::string& port) { port_ = port; reconnect(); save(); }
void GameBoyLinkHost::setAdapter(const std::string& adapter) {
    adapter_ = adapter == "gblink" ? "gblink" : "chromatic";
    reconnect(); save();
}
void GameBoyLinkHost::setLookahead(int ms) { link_.setLookaheadMs(std::max(0, ms)); save(); }
void GameBoyLinkHost::setLinkMode(int mode) { linkMode_ = std::clamp(mode, 0, 8); reconnect(); save(); }

void GameBoyLinkHost::connect(bool enabled) {
    enabled_ = enabled;
    if (enabled_ && port_.empty()) {
        const auto ports = lister_ ? lister_() : std::vector<GameBoyLinkPortDto>{};
        if (!ports.empty()) port_ = ports.front().port;
    }
    reconnect(); save();
}

void GameBoyLinkHost::restore() {
    if (FILE* f = std::fopen((configDir_ + "/gblink.cfg").c_str(), "r")) {
        char line[512];
        auto readLine = [&]() -> std::string {
            if (!std::fgets(line, sizeof line, f)) return {};
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            return s;
        };
        port_ = readLine();
        adapter_ = readLine() == "gblink" ? "gblink" : "chromatic";
        link_.setLookaheadMs(std::max(0, std::atoi(readLine().c_str())));
        enabled_ = std::atoi(readLine().c_str()) != 0;
        linkMode_ = std::clamp(std::atoi(readLine().c_str()), 0, 8);
        std::fclose(f);
    }
    reconnect();
}

void GameBoyLinkHost::save() {
    if (FILE* f = std::fopen((configDir_ + "/gblink.cfg").c_str(), "w")) {
        std::fprintf(f, "%s\n%s\n%d\n%d\n%d\n", port_.c_str(), adapter_.c_str(), link_.lookaheadMs(),
                     enabled_ ? 1 : 0, linkMode_);
        std::fclose(f);
    }
}

} // namespace retroplug
