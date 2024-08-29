#ifndef NOMINMAX
# define NOMINMAX // To remove conflicts with recent v8 code std::numeric_limits<int>::max()
#endif
/**
 * Copyright (c) Jonathan Cardoso Machado. All Rights Reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "Http2PushFrameHeaders.h"

#include <iostream>

namespace NodeLibcurl {

v8::Persistent<v8::ObjectTemplate> Http2PushFrameHeaders::objectTemplate;

Http2PushFrameHeaders::Http2PushFrameHeaders(struct curl_pushheaders* headers,
                                             size_t numberOfHeaders) {
  this->headers = headers;
  this->numberOfHeaders = numberOfHeaders;
}

Napi::Object Http2PushFrameHeaders::NewInstance(struct curl_pushheaders* headers,
                                                         size_t numberOfHeaders) {
  Napi::EscapableHandleScope scope(env);

  Napi::Object jsObj = Napi::NewInstance(Napi::New(env, objectTemplate));

  Http2PushFrameHeaders* cppObj = new Http2PushFrameHeaders(headers, numberOfHeaders);
  cppObj->Wrap(jsObj);

  return scope.Escape(jsObj);
}

Napi::Value Http2PushFrameHeaders::GetByIndex(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  Napi::Value value = info[0];

  if (!value->IsUint32()) {
    Napi::TypeError::New(env, "Index must be a non-negative integer").ThrowAsJavaScriptException();
    return env.Null();
  }

  Http2PushFrameHeaders* obj = this;
  uint32_t val = value.As<Napi::Number>().Uint32Value();

  char* result = curl_pushheader_bynum(obj->headers, static_cast<size_t>(val));

  Napi::Value returnValue =
      result == NULL ? env.Null().As<Napi::Value>()
                     : Napi::String>(result).As<Napi::Value::New(env);

  return returnValue;
}

Napi::Value Http2PushFrameHeaders::GetByName(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  Napi::Value value = info[0];

  if (!value.IsString()) {
    Napi::TypeError::New(env, "Name must be a string").ThrowAsJavaScriptException();
    return env.Null();
  }

  Http2PushFrameHeaders* obj = this;

  std::string utf8String = value.As<Napi::String>();

  char* result = curl_pushheader_byname(obj->headers, *utf8String);

  Napi::Value returnValue =
      result == NULL ? env.Null().As<Napi::Value>()
                     : Napi::String>(result).As<Napi::Value::New(env);

  return returnValue;
}

Napi::Value Http2PushFrameHeaders::GetterNumberOfHeaders(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  Http2PushFrameHeaders* obj = this;

  return Napi::Uint32::New(env, static_cast<uint32_t>(obj->numberOfHeaders));
}

Napi::Object Http2PushFrameHeaders::Initialize(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  v8::Local<v8::ObjectTemplate> objTmpl = Napi::ObjectTemplate::New(env);


  v8::PropertyAttribute attributes =
      static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete);

  Napi::SetAccessor(objTmpl, Napi::String::New(env, "numberOfHeaders"),
                   Http2PushFrameHeaders::GetterNumberOfHeaders, 0, Napi::Value(),
                   v8::DEFAULT, attributes);

  Napi::SetMethod(objTmpl, "getByIndex", Http2PushFrameHeaders::GetByIndex);
  Napi::SetMethod(objTmpl, "getByName", Http2PushFrameHeaders::GetByName);

  Http2PushFrameHeaders::objectTemplate.Reset(objTmpl);

  // this is not exported
}

}  // namespace NodeLibcurl
