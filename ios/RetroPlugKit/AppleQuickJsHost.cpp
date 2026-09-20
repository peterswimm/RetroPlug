#include "AppleQuickJsHost.hpp"

#include <cstring>

AppleQuickJsHost::~AppleQuickJsHost() {
    if (context_) JS_FreeContext(context_);
    if (runtime_) JS_FreeRuntime(runtime_);
}

bool AppleQuickJsHost::init() {
    if (context_) return true;
    runtime_ = JS_NewRuntime();
    context_ = runtime_ ? JS_NewContext(runtime_) : nullptr;
    if (!context_) return false;
    static constexpr char aliases[] = "globalThis.window=globalThis.global=globalThis.self=globalThis;";
    JSValue result = JS_Eval(context_, aliases, sizeof(aliases)-1, "<apple-globals>", JS_EVAL_TYPE_GLOBAL);
    const bool ok = !JS_IsException(result);
    JS_FreeValue(context_, result);
    return ok;
}

int AppleQuickJsHost::evalModuleBytecode(const std::uint8_t* bytes, std::size_t size) {
    JSValue object = JS_ReadObject(context_, bytes, size, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(object)) return -1;
    if (JS_VALUE_GET_TAG(object) == JS_TAG_MODULE && JS_ResolveModule(context_, object) < 0) {
        JS_FreeValue(context_, object);
        return -1;
    }
    JSValue result = JS_EvalFunction(context_, object);
    const bool ok = !JS_IsException(result);
    JS_FreeValue(context_, result);
    pump();
    return ok ? 0 : -1;
}

void AppleQuickJsHost::pump() {
    JSContext* job = nullptr;
    while (runtime_ && JS_ExecutePendingJob(runtime_, &job) > 0) {}
}

JSValue AppleQuickJsHost::rpcThunk(JSContext* context, JSValueConst, int argc,
                                   JSValueConst* argv, int, JSValue* data) {
    std::size_t size = 0;
    std::uint8_t* holder = JS_GetArrayBuffer(context, &size, data[0]);
    RpcSendFn* callback = nullptr;
    if (holder && size == sizeof(callback)) std::memcpy(&callback, holder, sizeof(callback));
    if (!callback || !*callback) return JS_ThrowInternalError(context, "RPC binding unavailable");
    if (argc < 1) return JS_ThrowTypeError(context, "RPC request missing");
    return (*callback)(context, argv[0]);
}

void AppleQuickJsHost::bindRpcSend(JSValue object, RpcSendFn callback) {
    auto owned = std::make_unique<RpcSendFn>(std::move(callback));
    RpcSendFn* raw = owned.get();
    bindings_.push_back(std::move(owned));
    JSValue holder = JS_NewArrayBufferCopy(context_, reinterpret_cast<const std::uint8_t*>(&raw), sizeof(raw));
    JSValue function = JS_NewCFunctionData(context_, rpcThunk, 1, 0, 1, &holder);
    JS_FreeValue(context_, holder);
    JS_SetPropertyStr(context_, object, "__rpcSend", function);
}
