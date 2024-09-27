#ifndef NOMINMAX
# define NOMINMAX // To remove conflicts with recent v8 code std::numeric_limits<int>::max()
#endif
/**
 * Copyright (c) Jonathan Cardoso Machado. All Rights Reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Share.h"

#include <iostream>

// 464 was allocated on Win64
//  Value too small to bother letting v8 know about it
#define MEMORY_PER_HANDLE 464

namespace NodeLibcurl {

Napi::FunctionReference Share::constructor;

Share::Share() : isOpen(true) {
  this->sh = curl_share_init();

  assert(this->sh);
}

Share::~Share(void) {
  if (this->isOpen) {
    this->Dispose();
  }
}

void Share::Dispose() {
  assert(this->isOpen && "This handle was already closed.");
  assert(this->sh && "The share handle ran away.");

  CURLSHcode code = curl_share_cleanup(this->sh);
  assert(code == CURLSHE_OK);

  this->isOpen = false;
}

Napi::Object Share::Initialize(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  // Easy js "class" function template initialization
  Napi::FunctionReference tmpl = Napi::Function::New(env, Share::New);
  tmpl->SetClassName(Napi::String::New(env, "Share"));


  // prototype methods
  Napi::SetPrototypeMethod(tmpl, "setOpt", Share::SetOpt);
  Napi::SetPrototypeMethod(tmpl, "close", Share::Close);

  // static methods
  Napi::SetMethod(tmpl, "strError", Share::StrError);

  Share::constructor.Reset(tmpl);

  (target).Set(Napi::String::New(env, "Share"), Napi::GetFunction(tmpl));
}

Napi::Value Share::New(const Napi::CallbackInfo& info) {
  if (!info.IsConstructCall()) {
    Napi::Error::New(env, "You must use \"new\" to instantiate this object.").ThrowAsJavaScriptException();

  }

  Share* obj = new Share();

  obj->Wrap(info.This());
  return info.This();
}

Napi::Value Share::SetOpt(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  Share* obj = this;

  if (!obj->isOpen) {
    Napi::Error::New(env, "Share handle is closed.").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Value opt = info[0];
  Napi::Value value = info[1];

  CURLSHcode setOptRetCode = CURLSHE_BAD_OPTION;
  int32_t optionId = -1;

  if (!value.IsNumber()) {
    Napi::Error::New(env, "Option value must be an integer.").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (opt.IsNumber()) {
    optionId = opt.As<Napi::Number>().Int32Value();
  } else if (opt.IsString()) {
    std::string option = opt.As<Napi::String>();

    std::string optionString(*option);

    if (optionString == "SHARE") {
      optionId = static_cast<int>(CURLSHOPT_SHARE);
    } else if (optionString == "UNSHARE") {
      optionId = static_cast<int>(CURLSHOPT_UNSHARE);
    }
  }

  setOptRetCode = curl_share_setopt(obj->sh, static_cast<CURLSHoption>(optionId),
                                    value.As<Napi::Number>().Int32Value());

  return setOptRetCode;
}

Napi::Value Share::Close(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  Share* obj = this;

  if (!obj->isOpen) {
    Napi::Error::New(env, "Share handle already closed.").ThrowAsJavaScriptException();
    return env.Null();
  }

  obj->Dispose();

  return;
}

Napi::Value Share::StrError(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  Napi::Value errCode = info[0];

  if (!errCode.IsNumber()) {
    Napi::TypeError::New(env, "Invalid errCode passed to Share.strError.").ThrowAsJavaScriptException();
    return env.Null();
  }

  const char* errorMsg =
      curl_share_strerror(static_cast<CURLSHcode>(errCode.As<Napi::Number>().Int32Value()));

  Napi::String ret = Napi::New(env, errorMsg);

  return ret;
}

}  // namespace NodeLibcurl
