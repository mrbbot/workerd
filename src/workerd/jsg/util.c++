// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg.h"  // can't include util.h directly due to weird cyclic dependency...
#include "setup.h"
#include <kj/debug.h>
#include <stdlib.h>

#ifndef _WIN32
#include <cxxabi.h>
#endif

#include <workerd/util/sentry.h>

namespace workerd::jsg {

bool getCaptureThrowsAsRejections(v8::Isolate* isolate) {
  auto& jsgIsolate = *reinterpret_cast<IsolateBase*>(isolate->GetData(0));
  return jsgIsolate.getCaptureThrowsAsRejections();
}

bool getCommonJsExportDefault(v8::Isolate* isolate) {
  auto& jsgIsolate = *reinterpret_cast<IsolateBase*>(isolate->GetData(0));
  return jsgIsolate.getCommonJsExportDefault();
}

#ifdef _WIN32
kj::String fullyQualifiedTypeName(const std::type_info& type) {
  // type.name() returns a human-readable name on Windows:
  // https://learn.microsoft.com/en-us/cpp/cpp/type-info-class?view=msvc-170
  kj::StringPtr name = type.name();

  // Remove struct prefix
  if (name.startsWith("struct ")) {
    name = name.slice(7);
  }
  // Remove class prefix
  if (name.startsWith("class ")) {
    name = name.slice(6);
  }

  kj::String result = kj::str(name);

  // Replace instances of `anonymous namespace' with (anonymous namespace)
  for (auto& c: result.asArray()) {
    if (c == '`') c = '(';
    else if (c == '\'') c = ')';
  }

  return kj::mv(result);
}
#else
kj::String fullyQualifiedTypeName(const std::type_info& type) {
  int status;
  char* buf = abi::__cxa_demangle(type.name(), nullptr, nullptr, &status);
  kj::String result = kj::str(buf == nullptr ? type.name() : buf);
  free(buf);

  return kj::mv(result);
}
#endif

kj::String typeName(const std::type_info& type) {
  auto result = fullyQualifiedTypeName(type);

  // Strip namespace, if any.
  KJ_IF_MAYBE(pos, result.findLast(':')) {
    result = kj::str(result.slice(*pos + 1));
  }

  // Strip template args, if any.
  //
  // TODO(someday): Maybe just strip namespaces from each arg?
  KJ_IF_MAYBE(pos, result.findFirst('<')) {
    result = kj::str(result.slice(0, *pos));
  }

  return kj::mv(result);
}

v8::Local<v8::Value> makeInternalError(v8::Isolate* isolate, kj::StringPtr internalMessage) {
  KJ_LOG(ERROR, internalMessage);
  return v8::Exception::Error(v8StrIntern(isolate, "internal error"));
}

namespace {

kj::StringPtr trimErrorMessage(kj::StringPtr errorString) {
  // For strings beginning with ':' OWS, returns everything after the OWS. Otherwise returns the
  // empty string.
  if (errorString.startsWith(":")) {
    errorString = errorString.slice(1);
    while (errorString.startsWith(" ")) {
      errorString = errorString.slice(1);
    }
    return errorString;
  }
  return "";
}

kj::Maybe<v8::Local<v8::Value>> tryMakeDomException(v8::Isolate* isolate,
                                                    v8::Local<v8::String> message,
                                                    kj::StringPtr errorName) {
  // If the global scope object has a "DOMException" object that is a constructor, construct a new
  // DOMException with the passed parameters. Note that this information is available at
  // compile-time via TypeWrapper, but threading TypeWrapper up from liftKj() call sites all the
  // way up here would be a readability nerf and lock users into our version of DOMException.

  auto context = isolate->GetCurrentContext();
  auto global = context->Global();

  const auto toObject = [context](v8::Local<v8::Value> value) {
    return check(value->ToObject(context));
  };
  const auto getInterned = [isolate, context](v8::Local<v8::Object> object, const char* s) {
    auto name = v8StrIntern(isolate, s);
    return check(object->Get(context, name));
  };

  if (auto domException = getInterned(global, "DOMException"); domException->IsObject()) {
    if (auto domExceptionCtor = toObject(domException); domExceptionCtor->IsConstructor()) {
      v8::Local<v8::Value> args[2] = {
        message, v8Str(isolate, errorName)
      };
      return check(domExceptionCtor->CallAsConstructor(context, 2, args));
    }
  }

  return nullptr;
}

v8::Local<v8::Value> tryMakeDomExceptionOrDefaultError(
    v8::Isolate* isolate, v8::Local<v8::String> message, kj::StringPtr errorName) {
  return tryMakeDomException(isolate, message, errorName).orDefault([&]() {
    return v8::Exception::Error(message);
  });
}

v8::Local<v8::Value> tryMakeDomExceptionOrDefaultError(
    v8::Isolate* isolate, kj::StringPtr message, kj::StringPtr errorName) {
  return tryMakeDomExceptionOrDefaultError(isolate, v8Str(isolate, message), errorName);
}

bool setRemoteError(v8::Isolate* isolate, v8::Local<v8::Value>& exception) {
  // If an exception was tunneled, we add a property `.remote` to the Javascript error.
  KJ_ASSERT(exception->IsObject());
  auto obj = exception.As<v8::Object>();
  return jsg::check(
    obj->Set(
      isolate->GetCurrentContext(),
      jsg::v8StrIntern(isolate, "remote"_kj),
      v8::True(isolate)));
}

bool setDurableObjectResetError(v8::Isolate* isolate, v8::Local<v8::Value>& exception) {
  KJ_ASSERT(exception->IsObject());
  auto obj = exception.As<v8::Object>();
  return jsg::check(
    obj->Set(
      isolate->GetCurrentContext(),
      jsg::v8StrIntern(isolate, "durableObjectReset"_kj),
      v8::True(isolate)));
}

struct TunneledErrorType {
  kj::StringPtr type;
  // What type of JavaScript error to throw.

  bool isInternal;
  // Is this error internal? If so, the error message should be logged to syslog and hidden from
  // the app.

  bool isFromRemote;
  // Was the error tunneled from either a worker or an actor?

  bool isDurableObjectReset;
  // Was the error created because a durable object is broken?
};

namespace {
  constexpr auto ERROR_PREFIX_DELIM = "; "_kj;
  constexpr auto ERROR_REMOTE_PREFIX = "remote."_kj;
  constexpr auto ERROR_TUNNELED_PREFIX_CFJS = "cfjs."_kj;
  constexpr auto ERROR_TUNNELED_PREFIX_JSG = "jsg."_kj;
  constexpr auto ERROR_INTERNAL_SOURCE_PREFIX_CFJS = "cfjs-internal."_kj;
  constexpr auto ERROR_INTERNAL_SOURCE_PREFIX_JSG = "jsg-internal."_kj;
}
TunneledErrorType tunneledErrorType(kj::StringPtr internalMessage) {
  // A tunneled error in an internal message is prefixed by one of the following patterns,
  // anchored at the beginning of the message:
  //   jsg.
  //   expected <...>; jsg.
  //   broken.<...>; jsg.
  // where <...> is some failed expectation from e.g. a KJ_REQUIRE.
  //
  // A tunneled error might have a prefix "remote.". This indicates it was tunneled from an actor or
  // from one worker to another. If this prefix is present, we set `isFromRemote` to true, remove
  // the "remote." prefix, and continue processing the rest of the error.
  //
  // Additionally, a prefix of `jsg-internal.` instead of `jsg.` means "throw a specific
  // JavaScript error type, but still hide the message text from the app".

  internalMessage = stripRemoteExceptionPrefix(internalMessage);

  struct Properties {
    bool isFromRemote = false;
    bool isDurableObjectReset = false;
  };
  Properties properties;

  // Remove `remote.` (if present). Note that there are cases where we return a tunneled error
  // through multiple workers, so let's be paranoid and allow for multiple "remote." prefxies.
  while (internalMessage.startsWith(ERROR_REMOTE_PREFIX)) {
    properties.isFromRemote = true;
    internalMessage = internalMessage.slice(ERROR_REMOTE_PREFIX.size());
  }

  auto findDelim = [](kj::StringPtr msg) -> size_t {
    // Either return 0 if no matches or the index past the first delim if there are.
    auto match = strstr(msg.cStr(), ERROR_PREFIX_DELIM.cStr());
    if (!match) {
      return 0;
    } else {
      return (match - msg.cStr()) + ERROR_PREFIX_DELIM.size();
    }
  };

  auto tryExtractError = [](kj::StringPtr msg, Properties properties)
      -> kj::Maybe<TunneledErrorType> {
    if (msg.startsWith(ERROR_TUNNELED_PREFIX_CFJS)) {
      return TunneledErrorType{
        .type = msg.slice(ERROR_TUNNELED_PREFIX_CFJS.size()),
        .isInternal = false,
        .isFromRemote = properties.isFromRemote,
        .isDurableObjectReset = properties.isDurableObjectReset,
      };
    }
    if (msg.startsWith(ERROR_TUNNELED_PREFIX_JSG)) {
      return TunneledErrorType{
        .type = msg.slice(ERROR_TUNNELED_PREFIX_JSG.size()),
        .isInternal = false,
        .isFromRemote = properties.isFromRemote,
        .isDurableObjectReset = properties.isDurableObjectReset,
      };
    }
    if (msg.startsWith(ERROR_INTERNAL_SOURCE_PREFIX_CFJS)) {
      return TunneledErrorType{
        .type = msg.slice(ERROR_INTERNAL_SOURCE_PREFIX_CFJS.size()),
        .isInternal = true,
        .isFromRemote = properties.isFromRemote,
        .isDurableObjectReset = properties.isDurableObjectReset,
      };
    }
    if (msg.startsWith(ERROR_INTERNAL_SOURCE_PREFIX_JSG)) {
      return TunneledErrorType{
        .type = msg.slice(ERROR_INTERNAL_SOURCE_PREFIX_JSG.size()),
        .isInternal = true,
        .isFromRemote = properties.isFromRemote,
        .isDurableObjectReset = properties.isDurableObjectReset,
      };
    }

    return nullptr;
  };

  auto makeDefaultError = [](Properties properties) {
    return TunneledErrorType{
      .type = "Error",
      .isInternal = true,
      .isFromRemote = properties.isFromRemote,
      .isDurableObjectReset = properties.isDurableObjectReset,
    };
  };

  if (internalMessage.startsWith("expected ")) {
    // This was a test assertion, peel away delimiters until either we find an error or there are
    // none left.
    auto idx = findDelim(internalMessage);
    while(idx) {
      internalMessage = internalMessage.slice(idx);
      KJ_IF_MAYBE(e, tryExtractError(internalMessage, properties)) {
        return kj::mv(*e);
      }
      idx = findDelim(internalMessage);
    }

    // We failed to extract an expected error, make a default one.
    return makeDefaultError(properties);
  }

  while (internalMessage.startsWith("broken.")) {
    properties.isDurableObjectReset = true;

    // Trim away all broken prefixes, they are not allowed to have internal delimiters.
    internalMessage = internalMessage.slice(findDelim(internalMessage));
  }

  // There are no prefixes left, just try to extract the error.
  KJ_IF_MAYBE(e, tryExtractError(internalMessage, properties)) {
    return kj::mv(*e);
  } else {
    return makeDefaultError(properties);
  }
}
struct DecodedException {
  v8::Local<v8::Value> handle;
  bool isInternal;
  bool isFromRemote;
  bool isDurableObjectReset;
};

DecodedException decodeTunneledException(v8::Isolate* isolate,
                                         kj::StringPtr internalMessage) {
  // We currently support tunneling the following error types:
  //
  // - Error:        While the Web IDL spec claims this is reserved for use by program authors, this
  //                 is broadly useful as a general-purpose error type.
  // - RangeError:   Commonly thrown by web API implementations.
  // - TypeError:    Commonly thrown by web API implementations.
  // - SyntaxError:  Especially from JSON parsing.
  // - ReferenceError: Not thrown by our APIs, but could be tunneled from user code.
  // - DOMException: Commonly thrown by web API implementations.
  //
  // ECMA-262 additionally defines EvalError and URIError, but V8 doesn't provide any API to
  // construct them.
  //
  // Note that this list is also present below in `tunneledErrorPrefixes()`.
  //
  // https://heycam.github.io/webidl/#idl-exceptions
  //
  // TODO(someday): Support arbitrary user-defined error types, not just Error?
  auto tunneledInfo = tunneledErrorType(internalMessage);

  auto errorType = tunneledInfo.type;
  auto appMessage = [&](kj::StringPtr errorString) -> kj::StringPtr {
    if (tunneledInfo.isInternal) {
      return "internal error"_kj;
    } else {
      return trimErrorMessage(errorString);
    }
  };
  DecodedException result;
  result.isInternal = tunneledInfo.isInternal;
  result.isFromRemote = tunneledInfo.isFromRemote;
  result.isDurableObjectReset = tunneledInfo.isDurableObjectReset;

#define HANDLE_V8_ERROR(error_name, error_type) \
  if (errorType.startsWith(error_name)) { \
    auto message = appMessage(errorType.slice(strlen(error_name))); \
    result.handle = v8::Exception::error_type(v8Str(isolate, message)); \
    break; \
  }

  do {
    HANDLE_V8_ERROR("Error", Error);
    HANDLE_V8_ERROR("RangeError", RangeError);
    HANDLE_V8_ERROR("TypeError", TypeError);
    HANDLE_V8_ERROR("SyntaxError", SyntaxError);
    HANDLE_V8_ERROR("ReferenceError", ReferenceError);
    HANDLE_V8_ERROR("CompileError", WasmCompileError);
    HANDLE_V8_ERROR("LinkError", WasmCompileError);
    HANDLE_V8_ERROR("RuntimeError", WasmCompileError);

    // DOMExceptions require a parenthesized error name argument, like DOMException(SyntaxError).
    if (errorType.startsWith("DOMException(")) {
      errorType = errorType.slice(strlen("DOMException("));
      // Check for closing brace
      KJ_IF_MAYBE(closeParen, errorType.findFirst(')')) {
        auto errorName = kj::str(errorType.slice(0, *closeParen));
        auto message = appMessage(errorType.slice(1 + *closeParen));
        result.handle = tryMakeDomExceptionOrDefaultError(isolate, message, errorName);
        break;
      }
    }
    // unrecognized exception type
    result.handle = v8::Exception::Error(v8StrIntern(isolate, "internal error"));
    result.isInternal = true;
  } while (false);
#undef HANDLE_V8_ERROR

  if (result.isFromRemote) {
    setRemoteError(isolate, result.handle);
  }

  if (result.isDurableObjectReset) {
    setDurableObjectResetError(isolate, result.handle);
  }

  return result;
}

}  // namespace

kj::String extractTunneledExceptionDescription(kj::StringPtr message) {
  return kj::str(tunneledErrorType(message).type);
}

kj::String annotateBroken(kj::StringPtr internalMessage, kj::StringPtr brokenessReason) {
  // TODO(soon) Once we support multiple brokenness reasons, we can make this much simpler.

  auto tunneledInfo = tunneledErrorType(internalMessage);

  kj::StringPtr remotePrefix;
  if (tunneledInfo.isFromRemote) {
    remotePrefix = ERROR_REMOTE_PREFIX;
  }

  kj::StringPtr prefixType = ERROR_TUNNELED_PREFIX_JSG;
  if (tunneledInfo.isInternal) {
    prefixType = ERROR_INTERNAL_SOURCE_PREFIX_JSG;
  }

  return kj::str(
      remotePrefix, brokenessReason, ERROR_PREFIX_DELIM, prefixType, tunneledInfo.type);
}

v8::Local<v8::Value> makeInternalError(v8::Isolate* isolate, kj::Exception&& exception) {
  auto desc = exception.getDescription();
  auto tunneledException = decodeTunneledException(isolate, desc);

  if (tunneledException.isInternal) {
    // Don't log exceptions that have been explicitly marked with worker_do_not_log or are
    // DISCONNECTED exceptions as these are unlikely to represent bugs worth tracking.
    if (exception.getType() != kj::Exception::Type::DISCONNECTED &&
        !isDoNotLogException(exception.getDescription())) {
      LOG_EXCEPTION("jsgInternalError", exception);
    } else {
      KJ_LOG(INFO, exception);  // Run with --verbose to see exception logs.
    }

    if (exception.getType() == kj::Exception::Type::DISCONNECTED) {
      auto exception = v8::Exception::Error(v8Str(isolate, "Network connection lost."_kj));
      if (tunneledException.isFromRemote) {
        setRemoteError(isolate, exception);
      }

      if (tunneledException.isDurableObjectReset) {
        setDurableObjectResetError(isolate, exception);
      }

      return exception;
    }
  }

  return tunneledException.handle;
}

Value Lock::exceptionToJs(kj::Exception&& exception) {
  v8::HandleScope scope(v8Isolate);
  return Value(v8Isolate, makeInternalError(v8Isolate, kj::mv(exception)));
}

void Lock::throwException(Value&& exception) {
  v8::HandleScope scope(v8Isolate);
  v8Isolate->ThrowException(exception.getHandle(v8Isolate));
  throw JsExceptionThrown();
}

void throwInternalError(v8::Isolate* isolate, kj::StringPtr internalMessage) {
  isolate->ThrowException(makeInternalError(isolate, internalMessage));
}

void throwInternalError(v8::Isolate* isolate, kj::Exception&& exception) {
  KJ_IF_MAYBE(renderingError, kj::runCatchingExceptions([&]() {
    isolate->ThrowException(makeInternalError(isolate, kj::mv(exception)));
  })) {
    KJ_LOG(ERROR, "error rendering exception", renderingError);
    KJ_LOG(ERROR, exception);
    throwInternalError(isolate, "error rendering exception");
  }
}

static kj::String typeErrorMessage(TypeErrorContext c, const char* expectedType) {
  kj::String type;

  KJ_IF_MAYBE(t, c.type) {
    type = typeName(*t);
  }

  switch (c.kind) {
  case TypeErrorContext::METHOD_ARGUMENT:
    return kj::str(
        "Failed to execute '", c.memberName, "' on '", type, "': parameter ", c.argumentIndex + 1,
        " is not of type '", expectedType, "'.");
  case TypeErrorContext::CONSTRUCTOR_ARGUMENT:
    return kj::str(
        "Failed to construct '", type, "': constructor parameter ", c.argumentIndex + 1,
        " is not of type '", expectedType, "'.");
  case TypeErrorContext::SETTER_ARGUMENT:
    return kj::str(
        "Failed to set the '", c.memberName, "' property on '", type,
        "': the provided value is not of type '", expectedType, "'.");
  case TypeErrorContext::STRUCT_FIELD:
    return kj::str(
        "Incorrect type for the '", c.memberName, "' field on '", type,
        "': the provided value is not of type '", expectedType, "'.");
  case TypeErrorContext::ARRAY_ELEMENT:
    return kj::str(
        "Incorrect type for array element ", c.argumentIndex,
        ": the provided value is not of type '", expectedType, "'.");
  case TypeErrorContext::CALLBACK_ARGUMENT:
    return kj::str(
        "Failed to execute function: parameter ", c.argumentIndex + 1, " is not of type '",
        expectedType, "'.");
  case TypeErrorContext::CALLBACK_RETURN:
    return kj::str("Callback returned incorrect type; expected '", expectedType, "'");
  case TypeErrorContext::DICT_KEY:
    return kj::str(
        "Incorrect type for map entry '", c.memberName,
        "': the provided key is not of type '", expectedType, "'.");
  case TypeErrorContext::DICT_FIELD:
    return kj::str(
        "Incorrect type for map entry '", c.memberName,
        "': the provided value is not of type '", expectedType, "'.");
  case TypeErrorContext::PROMISE_RESOLUTION:
    return kj::str(
        "Incorrect type for Promise: the Promise did not resolve to '", expectedType, "'.");
  case TypeErrorContext::OTHER:
    return kj::str(
        "Incorrect type: the provided value is not of type '", expectedType, "'.");
  };

  KJ_UNREACHABLE;
}

static kj::String unimplementedErrorMessage(TypeErrorContext c) {
  kj::String type;

  KJ_IF_MAYBE(t, c.type) {
    type = typeName(*t);
  }

  switch (c.kind) {
  case TypeErrorContext::METHOD_ARGUMENT:
    return kj::str(
        "Failed to execute '", c.memberName, "' on '", type, "': parameter ", c.argumentIndex + 1,
        " is not implemented.");
  case TypeErrorContext::CONSTRUCTOR_ARGUMENT:
    return kj::str(
        "Failed to construct '", type, "': constructor parameter ", c.argumentIndex + 1,
        " is not implemented.");
  case TypeErrorContext::SETTER_ARGUMENT:
    return kj::str(
        "Failed to set the '", c.memberName, "' property on '", type,
        "': the ability to set this property is not implemented.");
  case TypeErrorContext::STRUCT_FIELD:
    return kj::str(
        "The '", c.memberName, "' field on '", type, "' is not implemented.");
  case TypeErrorContext::ARRAY_ELEMENT:
    KJ_UNREACHABLE;
  case TypeErrorContext::CALLBACK_ARGUMENT:
    return kj::str(
        "Failed to execute function: parameter ", c.argumentIndex + 1, " is not implemented.");
  case TypeErrorContext::CALLBACK_RETURN:
    KJ_UNREACHABLE;
  case TypeErrorContext::DICT_KEY:
    KJ_UNREACHABLE;
  case TypeErrorContext::DICT_FIELD:
    KJ_UNREACHABLE;
  case TypeErrorContext::PROMISE_RESOLUTION:
    KJ_UNREACHABLE;
  case TypeErrorContext::OTHER:
    KJ_UNREACHABLE;
  };

  KJ_UNREACHABLE;
}

void throwTypeError(v8::Isolate* isolate, kj::StringPtr message) {
  isolate->ThrowException(v8::Exception::TypeError(v8Str(isolate, message)));
  throw JsExceptionThrown();
}

void throwTypeError(v8::Isolate* isolate,
    TypeErrorContext errorContext, kj::String expectedType) {
  kj::String message = typeErrorMessage(errorContext, expectedType.cStr());
  throwTypeError(isolate, message);
}

void throwTypeError(v8::Isolate* isolate,
    TypeErrorContext errorContext, const char* expectedType) {
  kj::String message = typeErrorMessage(errorContext, expectedType);
  throwTypeError(isolate, message);
}

void throwTypeError(v8::Isolate* isolate,
    TypeErrorContext errorContext, const std::type_info& expectedType) {
  if (expectedType == typeid(Unimplemented)) {
    isolate->ThrowException(v8::Exception::Error(
        v8Str(isolate, unimplementedErrorMessage(errorContext))));
    throw JsExceptionThrown();
  } else {
    throwTypeError(isolate, errorContext, typeName(expectedType).cStr());
  }
}

void throwIllegalConstructor(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto isolate = args.GetIsolate();
  kj::StringPtr message = "Illegal constructor";
  isolate->ThrowException(v8::Exception::TypeError(v8Str(isolate, message)));
}

void throwTunneledException(v8::Isolate* isolate, v8::Local<v8::Value> exception) {
  kj::throwFatalException(createTunneledException(isolate, exception));
}

kj::Exception createTunneledException(v8::Isolate* isolate, v8::Local<v8::Value> exception) {
  auto& jsgIsolate = *reinterpret_cast<IsolateBase*>(isolate->GetData(0));
  return jsgIsolate.unwrapException(isolate->GetCurrentContext(), exception);
}

kj::Exception Lock::exceptionToKj(Value&& exception) {
  v8::HandleScope scope(v8Isolate);
  return createTunneledException(v8Isolate, exception.getHandle(v8Isolate));
}

kj::StringPtr stripRemoteExceptionPrefix(kj::StringPtr internalMessage) {
  while (internalMessage.startsWith("remote exception: "_kj)) {
    // Exception was passed over RPC.
    internalMessage = internalMessage.slice("remote exception: "_kj.size());
  }
  return internalMessage;
}

bool isTunneledException(kj::StringPtr internalMessage) {
  return !tunneledErrorType(internalMessage).isInternal;
}

bool isDoNotLogException(kj::StringPtr internalMessage) {
  return strstr(internalMessage.cStr(), "worker_do_not_log") != nullptr;
}

static kj::byte DUMMY = 0;
static kj::Array<kj::byte> getEmptyArray() {
  // An older version of asBytes(), when given an empty ArrayBuffer, would often return an array
  // with zero size but non-empty start address. Meanwhile, it turns out that some code,
  // particularly in BoringSSL, does not like receiving a null pointer even when the length is
  // zero -- it will spuriously produce an error. We could carefully find all the places where
  // this is an issue and adjust the specific calls to avoid passing null pointers, but it is
  // easier to change `asBytes()` so that it never produces a null start address in the first
  // place.
  return kj::Array<kj::byte>(&DUMMY, 0, kj::NullArrayDisposer::instance);
}

kj::Array<kj::byte> asBytes(v8::Local<v8::ArrayBuffer> arrayBuffer) {
  auto backing = arrayBuffer->GetBackingStore();
  kj::ArrayPtr bytes(static_cast<kj::byte*>(backing->Data()), backing->ByteLength());
  if (bytes == nullptr) {
    return getEmptyArray();
  } else {
    return bytes.attach(kj::mv(backing));
  }
}
kj::Array<kj::byte> asBytes(v8::Local<v8::ArrayBufferView> arrayBufferView) {
  auto backing = arrayBufferView->Buffer()->GetBackingStore();
  kj::ArrayPtr buffer(static_cast<kj::byte*>(backing->Data()), backing->ByteLength());
  auto sliceStart = arrayBufferView->ByteOffset();
  auto sliceEnd = sliceStart + arrayBufferView->ByteLength();
  KJ_ASSERT(buffer.size() >= sliceEnd);
  auto bytes = buffer.slice(sliceStart, sliceEnd);
  if (bytes == nullptr) {
    return getEmptyArray();
  } else {
    return bytes.attach(kj::mv(backing));
  }
}

void recursivelyFreeze(v8::Local<v8::Context> context, v8::Local<v8::Value> value) {
  if (value->IsArray()) {
    // Optimize array freezing (Array is a subclass of Object, but we can iterate it faster).
    v8::HandleScope scope(context->GetIsolate());
    auto arr = value.As<v8::Array>();

    for (auto i: kj::zeroTo(arr->Length())) {
      recursivelyFreeze(context, check(arr->Get(context, i)));
    }

    check(arr->SetIntegrityLevel(context, v8::IntegrityLevel::kFrozen));
  } else if (value->IsObject()) {
    v8::HandleScope scope(context->GetIsolate());
    auto obj = value.As<v8::Object>();
    auto names = check(obj->GetPropertyNames(context,
        v8::KeyCollectionMode::kOwnOnly,
        v8::ALL_PROPERTIES,
        v8::IndexFilter::kIncludeIndices));

    for (auto i: kj::zeroTo(names->Length())) {
      recursivelyFreeze(context,
          check(obj->Get(context, check(names->Get(context, i)))));
    }

    check(obj->SetIntegrityLevel(context, v8::IntegrityLevel::kFrozen));
  } else {
    // Primitive type, nothing to do.
  }
}

v8::Local<v8::Value> deepClone(v8::Local<v8::Context> context, v8::Local<v8::Value> value) {
  // This is implemented in the classic JSON restringification way.
  auto serialized = check(v8::JSON::Stringify(context, value));
  return check(v8::JSON::Parse(context, serialized));
}

v8::Local<v8::Value> makeDOMException(
    v8::Isolate* isolate,
    v8::Local<v8::String> message,
    kj::StringPtr name) {
  return KJ_ASSERT_NONNULL(tryMakeDomException(isolate, message, name));
}

namespace {
v8::MaybeLocal<v8::Value> makeRejectedPromise(
    v8::Isolate* isolate,
    v8::Local<v8::Value> exception) {
  v8::Local<v8::Promise::Resolver> resolver;
  auto context = isolate->GetCurrentContext();
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver) ||
      resolver->Reject(context, exception).IsNothing()) {
    return v8::MaybeLocal<v8::Value>();
  }

  return resolver->GetPromise();
};

void returnRejectedPromiseImpl(
    auto info,
    v8::Local<v8::Value> exception,
    v8::TryCatch& tryCatch) {
  v8::Local<v8::Value> promise;
  if (!makeRejectedPromise(info.GetIsolate(), exception).ToLocal(&promise)) {
    // If makeRejectedPromise fails, the tryCatch should have caught the error.
    // Let's rethrow it if it isn't terminal.
    if (tryCatch.CanContinue()) tryCatch.ReThrow();
  }
  info.GetReturnValue().Set(promise);
}
}  // namespace

void returnRejectedPromise(
    const v8::FunctionCallbackInfo<v8::Value>& info,
    v8::Local<v8::Value> exception,
    v8::TryCatch& tryCatch) {
  return returnRejectedPromiseImpl(info, exception, tryCatch);
}

void returnRejectedPromise(
    const v8::PropertyCallbackInfo<v8::Value>& info,
    v8::Local<v8::Value> exception,
    v8::TryCatch& tryCatch) {
  return returnRejectedPromiseImpl(info, exception, tryCatch);
}

// ======================================================================================

template <typename Type, typename Data>
class ExternString: public Type {
  // The implementation of ExternString here is very closely after the implementation of the same
  // class in Node.js, with modifications to fit our conventions. It is distributed under the
  // same MIT license that Node.js uses. The appropriate copyright attribution is included here:
  //
  // Copyright Node.js contributors. All rights reserved.

  // Permission is hereby granted, free of charge, to any person obtaining a copy
  // of this software and associated documentation files (the "Software"), to
  // deal in the Software without restriction, including without limitation the
  // rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
  // sell copies of the Software, and to permit persons to whom the Software is
  // furnished to do so, subject to the following conditions:

  // The above copyright notice and this permission notice shall be included in
  // all copies or substantial portions of the Software.

  // THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  // IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  // FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  // AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  // LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  // FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
  // IN THE SOFTWARE.

public:
  inline const Data* data() const override { return buf.begin(); }

  inline size_t length() const override { return buf.size(); }

  inline uint64_t byteLength() const { return length() * sizeof(Data); }

  static v8::MaybeLocal<v8::String> createExtern(v8::Isolate* isolate,
                                                 kj::ArrayPtr<const Data>& buf) {
    if (buf.size() == 0) {
      return v8::String::Empty(isolate);
    }

    // TODO(now): In Node.js impl, we check to see if length is less than a specified
    // minimum. If it is, it's likely more efficient to just copy and use a regular
    // heap allocated string than an external. We're not doing that here currently, but
    // we might?

    // We typically don't use the new keyword in workerd/Workers but in this case we
    // have to.
    auto resource = new ExternString<Type, Data>(isolate, buf);

    v8::MaybeLocal<v8::String> str;
    if constexpr (kj::isSameType<Type, v8::String::ExternalOneByteStringResource>()) {
      str = v8::String::NewExternalOneByte(isolate, resource);
    } else {
      // resource here must be a v8::String::ExternalStringResource.
      str = v8::String::NewExternalTwoByte(isolate, resource);
    }
    if (str.IsEmpty()) {
      // This should happen only if the string is too long
      delete resource;
      return v8::MaybeLocal<v8::String>();
    }

    return str;
  }

private:
  v8::Isolate* isolate;
  kj::ArrayPtr<const Data> buf;

  inline ExternString(v8::Isolate* isolate, kj::ArrayPtr<const Data>& buf)
      : isolate(isolate), buf(buf) {}
};

using ExternOneByteString = ExternString<v8::String::ExternalOneByteStringResource, char>;
using ExternTwoByteString = ExternString<v8::String::ExternalStringResource, uint16_t>;

v8::MaybeLocal<v8::String> newExternalOneByteString(Lock& js, kj::ArrayPtr<const char> buf) {
  return ExternOneByteString::createExtern(js.v8Isolate, buf);
}

v8::MaybeLocal<v8::String> newExternalTwoByteString(Lock& js, kj::ArrayPtr<const uint16_t> buf) {
  return ExternTwoByteString::createExtern(js.v8Isolate, buf);
}

}  // namespace workerd::jsg
