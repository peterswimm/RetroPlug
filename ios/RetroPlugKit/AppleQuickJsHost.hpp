#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

extern "C" {
#include "quickjs.h"
}

// Minimal QuickJS host for iOS. The control plane is self-contained bytecode and uses synchronous RPC,
// so it does not need txiki's sockets, libuv, FFI, TLS, or process modules (several are unavailable in
// an app extension). Keeping this boundary bare also makes the archive dependency audit meaningful.
class AppleQuickJsHost final {
public:
    using RpcSendFn = std::function<JSValue(JSContext*, JSValueConst)>;
    AppleQuickJsHost() = default;
    ~AppleQuickJsHost();
    bool init();
    JSContext* context() const { return context_; }
    int evalModuleBytecode(const std::uint8_t* bytes, std::size_t size);
    void pump();
    void bindRpcSend(JSValue object, RpcSendFn callback);

private:
    static JSValue rpcThunk(JSContext*, JSValueConst, int, JSValueConst*, int, JSValue*);
    JSRuntime* runtime_ = nullptr;
    JSContext* context_ = nullptr;
    std::vector<std::unique_ptr<RpcSendFn>> bindings_;
};
