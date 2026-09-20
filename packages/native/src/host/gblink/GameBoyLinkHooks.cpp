#include "host/gblink/GameBoyLinkHooks.hpp"

#include <cstdint>
#include <cstring>

#include "quickjs.h"
#include "host/gblink/GameBoyLinkHost.hpp"

namespace retroplug {
namespace {
GameBoyLinkHost* hostFromData(JSContext* ctx, JSValue* data) {
    std::size_t len = 0;
    auto* raw = JS_GetArrayBuffer(ctx, &len, data[0]);
    if (!raw || len != sizeof(GameBoyLinkHost*)) return nullptr;
    GameBoyLinkHost* host = nullptr;
    std::memcpy(&host, raw, sizeof(host));
    return host;
}

JSValue getConfig(JSContext* ctx, JSValueConst, int, JSValueConst*, int, JSValue* data) {
    JSValue o = JS_NewObject(ctx);
    if (auto* h = hostFromData(ctx, data)) {
        const auto c = h->getConfig();
        JSValue ports = JS_NewArray(ctx);
        std::uint32_t i = 0;
        for (const auto& p : c.ports) {
            JSValue e = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, e, "port", JS_NewString(ctx, p.port.c_str()));
            JS_SetPropertyUint32(ctx, ports, i++, e);
        }
        JS_SetPropertyStr(ctx, o, "ports", ports);
        JS_SetPropertyStr(ctx, o, "selectedPort", JS_NewString(ctx, c.selectedPort.c_str()));
        JS_SetPropertyStr(ctx, o, "adapter", JS_NewString(ctx, c.adapter.c_str()));
        JS_SetPropertyStr(ctx, o, "connected", JS_NewBool(ctx, c.connected));
        JS_SetPropertyStr(ctx, o, "enabled", JS_NewBool(ctx, c.enabled));
        JS_SetPropertyStr(ctx, o, "lookaheadMs", JS_NewInt32(ctx, c.lookaheadMs));
        JS_SetPropertyStr(ctx, o, "linkMode", JS_NewInt32(ctx, c.linkMode));
        JS_SetPropertyStr(ctx, o, "baudRate", JS_NewUint32(ctx, c.baudRate));
        JS_SetPropertyStr(ctx, o, "dataBits", JS_NewInt32(ctx, c.dataBits));
        JS_SetPropertyStr(ctx, o, "parity", JS_NewString(ctx, c.parity.c_str()));
        JS_SetPropertyStr(ctx, o, "stopBits", JS_NewInt32(ctx, c.stopBits));
        JS_SetPropertyStr(ctx, o, "bytesSent", JS_NewInt64(ctx, static_cast<std::int64_t>(c.bytesSent)));
        JS_SetPropertyStr(ctx, o, "bytesReceived", JS_NewInt64(ctx, static_cast<std::int64_t>(c.bytesReceived)));
        JS_SetPropertyStr(ctx, o, "dropped", JS_NewInt64(ctx, static_cast<std::int64_t>(c.dropped)));
        JS_SetPropertyStr(ctx, o, "errors", JS_NewInt64(ctx, static_cast<std::int64_t>(c.errors)));
        JS_SetPropertyStr(ctx, o, "error", JS_NewString(ctx, c.error.c_str()));
    }
    return o;
}

template <void (GameBoyLinkHost::*Setter)(const std::string&)>
JSValue setString(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int, JSValue* data) {
    if (auto* h = hostFromData(ctx, data); h && argc > 0) {
        if (const char* s = JS_ToCString(ctx, argv[0])) { (h->*Setter)(s); JS_FreeCString(ctx, s); }
    }
    return JS_UNDEFINED;
}
JSValue connect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int, JSValue* data) {
    if (auto* h = hostFromData(ctx, data); h && argc > 0) h->connect(JS_ToBool(ctx, argv[0]) != 0);
    return JS_UNDEFINED;
}
template <void (GameBoyLinkHost::*Setter)(int)>
JSValue setInt(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int, JSValue* data) {
    if (auto* h = hostFromData(ctx, data); h && argc > 0) {
        std::int32_t n = 0; JS_ToInt32(ctx, &n, argv[0]); (h->*Setter)(n);
    }
    return JS_UNDEFINED;
}
JSValue drainRx(JSContext* ctx, JSValueConst, int, JSValueConst*, int, JSValue* data) {
    JSValue arr = JS_NewArray(ctx);
    if (auto* h = hostFromData(ctx, data)) {
        std::uint32_t i = 0;
        for (const auto& rx : h->drainReceived()) {
            JSValue e = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, e, "system", JS_NewUint32(ctx, rx.system));
            JS_SetPropertyStr(ctx, e, "byte", JS_NewInt32(ctx, rx.byte));
            JS_SetPropertyUint32(ctx, arr, i++, e);
        }
    }
    return arr;
}
}

void bindGameBoyLinkHooks(JSContext* ctx, GameBoyLinkHost& host) {
    if (!ctx) return;
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue old = JS_GetPropertyStr(ctx, g, "__rp_getGameBoyLinkConfig");
    const bool already = JS_IsFunction(ctx, old);
    JS_FreeValue(ctx, old);
    if (already) { JS_FreeValue(ctx, g); return; }
    GameBoyLinkHost* h = &host;
    auto bind = [&](const char* name, JSCFunctionData* fn, int length) {
        JSValue data = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const std::uint8_t*>(&h), sizeof(h));
        JS_SetPropertyStr(ctx, g, name, JS_NewCFunctionData(ctx, fn, length, 0, 1, &data));
        JS_FreeValue(ctx, data);
    };
    bind("__rp_getGameBoyLinkConfig", getConfig, 0);
    bind("__rp_setGameBoyLinkPort", setString<&GameBoyLinkHost::setPort>, 1);
    bind("__rp_setGameBoyLinkAdapter", setString<&GameBoyLinkHost::setAdapter>, 1);
    bind("__rp_connectGameBoyLink", connect, 1);
    bind("__rp_setGameBoyLinkLookahead", setInt<&GameBoyLinkHost::setLookahead>, 1);
    bind("__rp_setGameBoyLinkMode", setInt<&GameBoyLinkHost::setLinkMode>, 1);
    bind("__rp_drainGameBoyLinkRx", drainRx, 0);
    JS_FreeValue(ctx, g);
}

} // namespace retroplug
