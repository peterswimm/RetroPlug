#pragma once

struct JSContext;
namespace retroplug {
class GameBoyLinkHost;
void bindGameBoyLinkHooks(JSContext* ctx, GameBoyLinkHost& host);
}
