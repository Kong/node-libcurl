#ifndef NOMINMAX
# define NOMINMAX // To remove conflicts with recent v8 code std::numeric_limits<int>::max()
#endif
/**
 * Copyright (c) Jonathan Cardoso Machado. All Rights Reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "Multi.h"

#include "Easy.h"
#include "Http2PushFrameHeaders.h"

#include <iostream>
#include <string>

// 85233 was allocated on Win64
#define MEMORY_PER_HANDLE 60000

namespace NodeLibcurl {

Napi::FunctionReference Multi::constructor;

Multi::Multi() {
  // init uv timer to be used with HandleTimeout
  this->timeout = deleted_unique_ptr<uv_timer_t>(new uv_timer_t, [&](uv_timer_t* timerhandl) {
    uv_close(reinterpret_cast<uv_handle_t*>(timerhandl), Multi::OnTimerClose);
  });

  int timerStatus = uv_timer_init(uv_default_loop(), this->timeout.get());
  assert(timerStatus == 0 && "Could not initialize libuv timer");

  this->timeout->data = this;

  this->mh = curl_multi_init();
  assert(this->mh && "Could not initialize libcurl multi handle.");

  NODE_LIBCURL_ADJUST_MEM(MEMORY_PER_HANDLE);

  // set curl_multi cb to use libuv
  curl_multi_setopt(this->mh, CURLMOPT_SOCKETFUNCTION, Multi::HandleSocket);
  curl_multi_setopt(this->mh, CURLMOPT_SOCKETDATA, this);
  curl_multi_setopt(this->mh, CURLMOPT_TIMERFUNCTION, Multi::HandleTimeout);
  curl_multi_setopt(this->mh, CURLMOPT_TIMERDATA, this);
}

Multi::~Multi() {
  if (this->isOpen) {
    this->Dispose();
  }
}

void Multi::Dispose() {
  assert(this->isOpen);

  this->isOpen = false;

  if (this->mh) {
    CURLMcode code = curl_multi_cleanup(this->mh);
    assert(code == CURLM_OK);

    NODE_LIBCURL_ADJUST_MEM(-MEMORY_PER_HANDLE);
  }

  uv_timer_stop(this->timeout.get());
}

// The curl_multi_socket_action(3) function informs the application about
// updates
//  in the socket (file descriptor) status by doing none, one, or multiple calls
//  to this function
int Multi::HandleSocket(CURL* easy, curl_socket_t s, int action, void* userp, void* socketp) {
  CurlSocketContext* ctx = nullptr;
  Multi* obj = static_cast<Multi*>(userp);

  if (action == CURL_POLL_IN || action == CURL_POLL_OUT || action == CURL_POLL_INOUT ||
      action == CURL_POLL_NONE) {
    // create ctx if it doesn't exists and assign it to the current socket,
    if (socketp) {
      ctx = static_cast<Multi::CurlSocketContext*>(socketp);
    } else {
      ctx = Multi::CreateCurlSocketContext(s, obj);
      curl_multi_assign(obj->mh, s, static_cast<void*>(ctx));
    }

    // set event based on the current action
    int events = 0;

    switch (action) {
      case CURL_POLL_IN:
        events |= UV_READABLE;
        break;
      case CURL_POLL_OUT:
        events |= UV_WRITABLE;
        break;
      case CURL_POLL_INOUT:
        events |= UV_READABLE | UV_WRITABLE;
        break;
    }

    // start polling the socket.
    return uv_poll_start(&ctx->pollHandle, events, Multi::OnSocket);
  }

  if (action == CURL_POLL_REMOVE && socketp) {
    ctx = static_cast<CurlSocketContext*>(socketp);

    uv_poll_stop(&ctx->pollHandle);
    Multi::DestroyCurlSocketContext(ctx);

    curl_multi_assign(obj->mh, s, NULL);

    return 0;
  }

  return -1;
}

// This function will be called when the timeout value changes from libcurl.
// The timeout value is at what latest time the application should call one of
// the "performing" functions of the multi interface (curl_multi_socket_action
// and curl_multi_perform) - to allow libcurl to keep timeouts and retries etc
// to work.
int Multi::HandleTimeout(CURLM* multi,
                         long timeoutMs,  // NOLINT(runtime/int)
                         void* userp) {
  Multi* obj = static_cast<Multi*>(userp);

  int uvStop = uv_timer_stop(obj->timeout.get());

  if (uvStop < 0) {
    return uvStop;
  }

  // we should not call libcurl functions directly from this callback
  //  see https://github.com/curl/curl/issues/3537
  if (timeoutMs >= 0) {
    return uv_timer_start(obj->timeout.get(), Multi::OnTimeout, timeoutMs, 0);
  }

  return 0;
}

// called when there is activity in the socket.
void Multi::OnSocket(uv_poll_t* handle, int status, int events) {
  int flags = 0;

  CURLMcode code;

  if (status < 0) flags = CURL_CSELECT_ERR;
  if (events & UV_READABLE) flags |= CURL_CSELECT_IN;
  if (events & UV_WRITABLE) flags |= CURL_CSELECT_OUT;

  Multi::CurlSocketContext* ctx = static_cast<Multi::CurlSocketContext*>(handle->data);

  // Check comment on node_libcurl.cc
  SETLOCALE_WRAPPER(
      // Before version 7.20.0: If you receive CURLM_CALL_MULTI_PERFORM, this
      // basically means that you should call curl_multi_socket_action again
      // before you wait for more actions on libcurl's sockets.
      // You don't have to do it immediately, but the return code means that
      // libcurl
      //  may have more data available to return or that there may be more data
      //  to send off before it is "satisfied".
      do {
        code = curl_multi_socket_action(ctx->multi->mh, ctx->sockfd, flags,
                                        &ctx->multi->runningHandles);
      } while (code == CURLM_CALL_MULTI_PERFORM););  // NOLINT(whitespace/newline)

  if (code != CURLM_OK) {
    std::string errorMsg;

    errorMsg +=
        std::string("curl_multi_socket_action failed. Reason: ") + curl_multi_strerror(code);

    Napi::Error::New(env, errorMsg.c_str()).ThrowAsJavaScriptException();
    return env.Null();
  }

  ctx->multi->ProcessMessages();
}

// function called when the previous timeout set reaches 0
UV_TIMER_CB(Multi::OnTimeout) {
  Multi* obj = static_cast<Multi*>(timer->data);

  // Check comment on node_libcurl.cc
  SETLOCALE_WRAPPER(CURLMcode code = curl_multi_socket_action(
                        obj->mh, CURL_SOCKET_TIMEOUT, 0,
                        &obj->runningHandles););  // NOLINT(whitespace/newline)

  if (code != CURLM_OK) {
    std::string errorMsg;

    errorMsg +=
        std::string("curl_multi_socket_action failed. Reason: ") + curl_multi_strerror(code);

    Napi::Error::New(env, errorMsg.c_str()).ThrowAsJavaScriptException();
    return env.Null();
  }

  obj->ProcessMessages();
}

void Multi::OnTimerClose(uv_handle_t* handle) { delete handle; }

void Multi::ProcessMessages() {
  CURLMsg* msg = NULL;
  int pending = 0;

  while ((msg = curl_multi_info_read(this->mh, &pending))) {
    if (msg->msg == CURLMSG_DONE) {
      CURLcode statusCode = msg->data.result;

      this->CallOnMessageCallback(msg->easy_handle, statusCode);
    }
  }
}

// Creates a Context to be used to store data between events
Multi::CurlSocketContext* Multi::CreateCurlSocketContext(curl_socket_t sockfd, Multi* multi) {
  int r;
  Multi::CurlSocketContext* ctx = NULL;

  ctx = static_cast<Multi::CurlSocketContext*>(malloc(sizeof(*ctx)));
  assert(ctx && "Not enough memory to allocate a new Multi::CurlSocketContext.");

  ctx->sockfd = sockfd;
  ctx->multi = multi;

  // uv_poll simply watches file descriptors using the operating system
  // notification mechanism
  //   whenever the OS notices a change of state in file descriptors being
  //   polled, libuv will invoke the associated callback.
  r = uv_poll_init_socket(uv_default_loop(), &ctx->pollHandle, sockfd);

  assert(r == 0);

  ctx->pollHandle.data = ctx;

  return ctx;
}

// called when libcurl thinks the socket can be destroyed
void Multi::DestroyCurlSocketContext(Multi::CurlSocketContext* ctx) {
  uv_handle_t* handle = reinterpret_cast<uv_handle_t*>(&ctx->pollHandle);

  uv_close(handle, Multi::OnSocketClose);
}

void Multi::OnSocketClose(uv_handle_t* handle) {
  Multi::CurlSocketContext* ctx = static_cast<Multi::CurlSocketContext*>(handle->data);
  free(ctx);
}

void Multi::CallOnMessageCallback(CURL* easy, CURLcode statusCode) {
  Napi::HandleScope scope(env);

  // we don't have an on message callback, just return.
  if (this->cbOnMessage == nullptr) {
    return;
  }

  // From https://curl.haxx.se/libcurl/c/CURLINFO_PRIVATE.html
  // > Please note that for internal reasons, the value is returned as a char
  // pointer, although effectively being a 'void *'.
  char* ptr = nullptr;
  CURLcode code = curl_easy_getinfo(easy, CURLINFO_PRIVATE, &ptr);
  if (code != CURLE_OK) {
    Napi::Error::New(env, "Error retrieving current handle instance.").ThrowAsJavaScriptException();
    return env.Null();
  }

  assert(ptr != nullptr && "Invalid handle returned from CURLINFO_PRIVATE.");
  Easy* obj = reinterpret_cast<Easy*>(ptr);

  bool hasError = !obj->callbackError.IsEmpty();

  Napi::Object easyArg = obj->handle();

  Napi::Value err = env.Null();
  v8::Local<v8::Int32> errCode = Napi::New(env, static_cast<int32_t>(
      statusCode == CURLE_OK && hasError ? CURLE_ABORTED_BY_CALLBACK : statusCode));

  if (statusCode != CURLE_OK || hasError) {
    err = hasError ? Napi::New(env, obj->callbackError) : Napi::Error::New(env, curl_easy_strerror(statusCode));
  }

  Napi::Value argv[] = {err, easyArg, errCode};
  const int argc = 3;

  Napi::AsyncResource asyncResource("Multi::CallOnMessageCallback");
  asyncResource.runInAsyncScope(obj->handle(), this->cbOnMessage->GetFunction(), argc, argv);
}

// User set multi_opt callbacks

int Multi::CbPushFunction(CURL* parent, CURL* child, size_t numberOfHeaders,  // NOLINT(runtime/int)
                          struct curl_pushheaders* headers, void* userPtr) {
  // Note:
  //  We cannot throw js errors inside this callback
  //   as there is no way to signal libcurl to mark this request as failed
  //   and stop calling this callback for this connection (in case there are more pushes)
  //   this means that we must not rethrow errors we catch from user land.
  //   doing so would cause the whole library code to fall apart as it would not be safe to
  //   use other v8 objects.
  Napi::HandleScope scope(env);

  int returnValue = -1;

  Multi* obj = static_cast<Multi*>(userPtr);
  assert(obj);
  assert(obj->isOpen);

  CallbacksMap::iterator it = obj->callbacks.find(CURLMOPT_PUSHFUNCTION);
  assert(it != obj->callbacks.end() && "PUSHFUNCTION callback not set.");

  char* parentEasyPtr = nullptr;
  CURLcode code = curl_easy_getinfo(parent, CURLINFO_PRIVATE, &parentEasyPtr);
  assert(code == CURLE_OK &&
         "It was not possible to retrieve the current Easy instance from the libcurl easy handle");
  assert(parentEasyPtr != nullptr && "Invalid handle returned from CURLINFO_PRIVATE.");

  Easy* parentEasyObj = reinterpret_cast<Easy*>(parentEasyPtr);
  assert(parentEasyObj->isOpen &&
         "The Easy instance doing the current request was closed prematurely");

  Napi::Object parentEasyJsObj = obj->handle();

  // create new Easy instance to be used with the easy curl handle passed
  //  as second parameter
  Napi::Object childEasyJsObj = Easy::FromCURLHandle(child);

  auto http2PushFrameJsObj = Http2PushFrameHeaders::NewInstance(headers, numberOfHeaders);

  const int argc = 3;
  Napi::Value argv[argc] = {
      parentEasyJsObj,
      childEasyJsObj,
      http2PushFrameJsObj,
  };

  Napi::TryCatch tryCatch;

  Napi::AsyncResource asyncResource("Multi::CbPushFunction");
  Napi::MaybeLocal<v8::Value> returnValueCallback =
      asyncResource.runInAsyncScope(obj->handle(), it->second->GetFunction(), argc, argv);

  if (tryCatch.HasCaught()) {
    // See the note at the top of this function, we must not rethrow this error.
    // Show some Debug message?
    return returnValue;
  }

  if (returnValueCallback.IsEmpty() || !returnValueCallback.IsNumber()) {
    // Nothing we can do - Let's just ignore it
    // Napi::Value typeError =
    //     Napi::TypeError::New(env, "Return value from the PUSHFUNCTION callback must be an integer.");
    // Napi::Error::New(env, typeError).ThrowAsJavaScriptException();

  } else {
    returnValue = returnValueCallback.ToLocalChecked(.As<Napi::Number>().Int32Value());
  }

  return returnValue;
}

// Add Curl constructor to the module exports
Napi::Object Multi::Initialize(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  // Multi js "class" function template initialization
  Napi::FunctionReference tmpl = Napi::Function::New(env, Multi::New);
  tmpl->SetClassName(Napi::String::New(env, "Multi"));


  // prototype methods
  Napi::SetPrototypeMethod(tmpl, "setOpt", Multi::SetOpt);
  Napi::SetPrototypeMethod(tmpl, "addHandle", Multi::AddHandle);
  Napi::SetPrototypeMethod(tmpl, "onMessage", Multi::OnMessage);
  Napi::SetPrototypeMethod(tmpl, "removeHandle", Multi::RemoveHandle);
  Napi::SetPrototypeMethod(tmpl, "getCount", Multi::GetCount);
  Napi::SetPrototypeMethod(tmpl, "close", Multi::Close);

  // static methods
  Napi::SetMethod(tmpl, "strError", Multi::StrError);

  Multi::constructor.Reset(tmpl);

  (target).Set(Napi::String::New(env, "Multi"), Napi::GetFunction(tmpl));
}

Napi::Value Multi::New(const Napi::CallbackInfo& info) {
  if (!info.IsConstructCall()) {
    Napi::Error::New(env, "You must use \"new\" to instantiate this object.").ThrowAsJavaScriptException();

  }

  Multi* obj = new Multi();

  obj->Wrap(info.This());

  return info.This();
}

Napi::Value Multi::SetOpt(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  Multi* obj = this;

  if (!obj->isOpen) {
    Napi::Error::New(env, "Multi handle is closed.").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Value opt = info[0];
  Napi::Value value = info[1];

  CURLMcode setOptRetCode = CURLM_UNKNOWN_OPTION;

  int optionId;

  // array of strings option
  if ((optionId = IsInsideCurlConstantStruct(curlMultiOptionNotImplemented, opt))) {
    Napi::ThrowError(
        "Unsupported option, probably because it's too complex to implement "
        "using javascript or unecessary when using javascript.");
    return;
  } else if ((optionId = IsInsideCurlConstantStruct(curlMultiOptionStringArray, opt))) {
    if (value->IsNull()) {
      setOptRetCode = curl_multi_setopt(obj->mh, static_cast<CURLMoption>(optionId), NULL);

    } else {
      if (!value->IsArray()) {
        Napi::TypeError::New(env, "Option value must be an Array.").ThrowAsJavaScriptException();
        return env.Null();
      }

      Napi::Array array = value.As<Napi::Array>();
      uint32_t arrayLength = array->Length();
      std::vector<char*> strings;

      for (uint32_t i = 0; i < arrayLength; ++i) {
        strings.push_back((array).Get(i->As<Napi::String>().Utf8Value().c_str()));
      }

      strings.push_back(NULL);

      setOptRetCode = curl_multi_setopt(obj->mh, static_cast<CURLMoption>(optionId), &strings[0]);
    }

    // check if option is integer, and the value is correct
  } else if ((optionId = IsInsideCurlConstantStruct(curlMultiOptionInteger, opt))) {
    // If not an integer, throw error
    if (!value.IsNumber()) {
      Napi::TypeError::New(env, "Option value must be an integer.").ThrowAsJavaScriptException();
      return env.Null();
    }

    int32_t val = value.As<Napi::Number>().Int32Value();

    setOptRetCode = curl_multi_setopt(obj->mh, static_cast<CURLMoption>(optionId), val);
  } else if ((optionId = IsInsideCurlConstantStruct(curlMultiOptionFunction, opt))) {
    bool isNull = value->IsNull();

    if (!value->IsFunction() && !isNull) {
      Napi::TypeError::New(env, "Option value must be null or a function.").ThrowAsJavaScriptException();
      return env.Null();
    }

    switch (optionId) {
#if NODE_LIBCURL_VER_GE(7, 44, 0)
      case CURLMOPT_PUSHFUNCTION:

        if (isNull) {
          obj->callbacks.erase(CURLMOPT_PUSHFUNCTION);

          curl_multi_setopt(obj->mh, CURLMOPT_PUSHDATA, NULL);
          setOptRetCode = curl_multi_setopt(obj->mh, CURLMOPT_PUSHFUNCTION, NULL);
        } else {
          obj->callbacks[CURLMOPT_PUSHFUNCTION].reset(new Napi::FunctionReference(value.As<Napi::Function>()));

          curl_multi_setopt(obj->mh, CURLMOPT_PUSHDATA, obj);
          setOptRetCode = curl_multi_setopt(obj->mh, CURLMOPT_PUSHFUNCTION, Multi::CbPushFunction);
        }

        break;
#endif
    }
  }

  return setOptRetCode;
}

Napi::Value Multi::OnMessage(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  Multi* obj = this;

  if (!info.Length()) {
    Napi::ThrowError(
        "You must specify the callback function. If you want to remove the "
        "current one you can pass null.");
    return;
  }

  Napi::Value arg = info[0];

  bool isNull = arg->IsNull();

  if (!arg->IsFunction() && !isNull) {
    Napi::ThrowTypeError(
        "Argument must be a Function. If you want to remove the current one "
        "you can pass null.");
    return;
  }

  if (isNull) {
    obj->cbOnMessage = nullptr;
  } else {
    obj->cbOnMessage.reset(new Napi::FunctionReference(arg.As<Napi::Function>()));
  }

  return info.This();
}

Napi::Value Multi::AddHandle(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  Multi* obj = this;

  if (!obj->isOpen) {
    Napi::Error::New(env, "Multi handle is closed.").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Value handle = info[0];

  if (!handle.IsObject() || !Napi::New(env, Easy::constructor)->HasInstance(handle)) {
    Napi::Error::New(env, Napi::TypeError::New(env, "Argument must be an instance of an Easy handle.")).ThrowAsJavaScriptException();
    return env.Null();
  } else {
    Easy* easy = handle.As<Napi::Object>().Unwrap<Easy>();

    if (!easy->isOpen) {
      Napi::Error::New(env, "Cannot add an Easy handle that is closed.").ThrowAsJavaScriptException();
      return env.Null();
    }

    easy->SetUrlOpts();

    // reset callback error in case it is set
    easy->callbackError.Reset();

    // Check comment on node_libcurl.cc
    SETLOCALE_WRAPPER(CURLMcode code =
                          curl_multi_add_handle(obj->mh, easy->ch););  // NOLINT(whitespace/newline)

    if (code != CURLM_OK) {
      Napi::Error::New(env, Napi::TypeError::New(env, "Could not add easy handle to the multi handle.")).ThrowAsJavaScriptException();
      return env.Null();
    }

    ++obj->amountOfHandles;
    easy->isInsideMultiHandle = true;

    v8::Local<v8::Int32> ret = Napi::New(env, static_cast<int32_t>(code));

    return ret;
  }
}

Napi::Value Multi::RemoveHandle(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  Multi* obj = this;

  if (!obj->isOpen) {
    Napi::Error::New(env, "Multi handle is closed.").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Value handle = info[0];

  if (!handle.IsObject() || !Napi::New(env, Easy::constructor)->HasInstance(handle)) {
    Napi::Error::New(env, Napi::TypeError::New(env, "Argument must be an instance of an Easy handle.")).ThrowAsJavaScriptException();
    return env.Null();
  } else {
    Easy* easy = handle.As<Napi::Object>().Unwrap<Easy>();

    CURLMcode code = curl_multi_remove_handle(obj->mh, easy->ch);

    if (code != CURLM_OK) {
      Napi::Error::New(env, Napi::TypeError::New(env, "Could not remove easy handle from multi handle.")).ThrowAsJavaScriptException();
      return env.Null();
    }

    --obj->amountOfHandles;
    easy->isInsideMultiHandle = false;

    v8::Local<v8::Int32> ret = Napi::New(env, static_cast<int32_t>(code));

    return ret;
  }
}

Napi::Value Multi::GetCount(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  Multi* obj = this;

  v8::Local<v8::Uint32> ret = Napi::New(env, static_cast<uint32_t>(obj->amountOfHandles));

  return ret;
}

Napi::Value Multi::Close(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  Multi* obj = this;

  if (!obj->isOpen) {
    Napi::Error::New(env, "Multi handle already closed.").ThrowAsJavaScriptException();
    return env.Null();
  }

  obj->Dispose();
}

Napi::Value Multi::StrError(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  Napi::Value errCode = info[0];

  if (!errCode.IsNumber()) {
    Napi::TypeError::New(env, "Invalid errCode passed to Multi.strError.").ThrowAsJavaScriptException();
    return env.Null();
  }

  const char* errorMsg =
      curl_multi_strerror(static_cast<CURLMcode>(errCode.As<Napi::Number>().Int32Value()));

  Napi::String ret = Napi::New(env, errorMsg);

  return ret;
}
}  // namespace NodeLibcurl
