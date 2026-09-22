#include "AppleQuickJsHost.hpp"

#include <cstring>

namespace {
const char kBootstrap[] = R"JS(
(function () {
  globalThis.window = globalThis.global = globalThis.self = globalThis;
  if (typeof globalThis.console === 'undefined') {
    var noop = function () {};
    globalThis.console = { log: noop, info: noop, debug: noop, warn: noop, error: noop, trace: noop };
  }
  if (typeof globalThis.TextEncoder === 'undefined') {
    globalThis.TextEncoder = class TextEncoder {
      get encoding() { return 'utf-8'; }
      encode(str) {
        str = String(str === undefined ? '' : str);
        var out = [];
        for (var i = 0; i < str.length; i++) {
          var c = str.charCodeAt(i);
          if (c < 0x80) out.push(c);
          else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
          else if (c >= 0xd800 && c <= 0xdbff) {
            var c2 = str.charCodeAt(i + 1);
            if (c2 >= 0xdc00 && c2 <= 0xdfff) {
              c = 0x10000 + ((c & 0x3ff) << 10) + (c2 & 0x3ff); i++;
              out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 0x3f), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
            } else out.push(0xef, 0xbf, 0xbd);
          } else if (c >= 0xdc00 && c <= 0xdfff) out.push(0xef, 0xbf, 0xbd);
          else out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
        }
        return new Uint8Array(out);
      }
    };
  }
  if (typeof globalThis.TextDecoder === 'undefined') {
    globalThis.TextDecoder = class TextDecoder {
      constructor(label) { this._label = label || 'utf-8'; }
      get encoding() { return 'utf-8'; }
      decode(buf) {
        if (buf === undefined) return '';
        var b = buf instanceof Uint8Array ? buf : new Uint8Array(buf.buffer || buf);
        var out = '';
        for (var i = 0; i < b.length;) {
          var c = b[i++];
          if (c < 0x80) out += String.fromCharCode(c);
          else if (c < 0xe0) out += String.fromCharCode(((c & 0x1f) << 6) | (b[i++] & 0x3f));
          else if (c < 0xf0) out += String.fromCharCode(((c & 0x0f) << 12) | ((b[i++] & 0x3f) << 6) | (b[i++] & 0x3f));
          else {
            var cp = ((c & 0x07) << 18) | ((b[i++] & 0x3f) << 12) | ((b[i++] & 0x3f) << 6) | (b[i++] & 0x3f);
            cp -= 0x10000;
            out += String.fromCharCode(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
          }
        }
        return out;
      }
    };
  }
})();
)JS";

std::string describeJsValue(JSContext* context, JSValueConst value) {
    JSValue stack = JS_GetPropertyStr(context, value, "stack");
    JSValueConst printable = JS_IsUndefined(stack) ? value : stack;
    const char* text = JS_ToCString(context, printable);
    std::string result = text ? text : "JavaScript evaluation failed";
    if (text) JS_FreeCString(context, text);
    JS_FreeValue(context, stack);
    return result;
}
}

AppleQuickJsHost::~AppleQuickJsHost() {
    if (context_) JS_FreeContext(context_);
    if (runtime_) JS_FreeRuntime(runtime_);
}

bool AppleQuickJsHost::init() {
    if (context_) return true;
    runtime_ = JS_NewRuntime();
    context_ = runtime_ ? JS_NewContext(runtime_) : nullptr;
    if (!context_) return false;
    JSValue result = JS_Eval(context_, kBootstrap, sizeof(kBootstrap)-1,
                             "<apple-bootstrap>", JS_EVAL_TYPE_GLOBAL);
    const bool ok = !JS_IsException(result);
    JS_FreeValue(context_, result);
    return ok;
}

int AppleQuickJsHost::evalModuleBytecode(const std::uint8_t* bytes, std::size_t size) {
    lastError_.clear();
    JSValue object = JS_ReadObject(context_, bytes, size, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(object)) {
        JSValue exception = JS_GetException(context_);
        lastError_ = describeJsValue(context_, exception);
        JS_FreeValue(context_, exception);
        return -1;
    }
    if (JS_VALUE_GET_TAG(object) == JS_TAG_MODULE && JS_ResolveModule(context_, object) < 0) {
        JSValue exception = JS_GetException(context_);
        lastError_ = describeJsValue(context_, exception);
        JS_FreeValue(context_, exception);
        JS_FreeValue(context_, object);
        return -1;
    }
    JSValue result = JS_EvalFunction(context_, object);
    if (JS_IsException(result)) {
        JSValue exception = JS_GetException(context_);
        lastError_ = describeJsValue(context_, exception);
        JS_FreeValue(context_, exception);
        JS_FreeValue(context_, result);
        return -1;
    }
    // ES modules evaluate to a promise. Runtime exceptions reject that promise instead of making
    // JS_EvalFunction return JS_EXCEPTION, so the rejection must be inspected after draining jobs.
    // Without this check Apple hosts only saw `__rp_ready == false`, hiding the actionable error.
    pump();
    const auto promiseState = JS_PromiseState(context_, result);
    if (promiseState == JS_PROMISE_REJECTED) {
        JSValue rejection = JS_PromiseResult(context_, result);
        lastError_ = describeJsValue(context_, rejection);
        JS_FreeValue(context_, rejection);
        JS_FreeValue(context_, result);
        return -1;
    }
    if (promiseState == JS_PROMISE_PENDING) {
        lastError_ = "JavaScript module evaluation did not settle";
        JS_FreeValue(context_, result);
        return -1;
    }
    JS_FreeValue(context_, result);
    return 0;
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
