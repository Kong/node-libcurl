/**
 * Copyright (c) Jonathan Cardoso Machado. All Rights Reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef NODELIBCURL_SHARE_H
#define NODELIBCURL_SHARE_H

#include <curl/curl.h>
#include <napi.h>
#include <uv.h>
#include <napi.h>
#include <uv.h>

namespace NodeLibcurl {

class Share : public Napi::ObjectWrap<Share> {
  Share();

  Share(const Share& that);
  Share& operator=(const Share& that);

  ~Share();

  // instance methods
  void Dispose();

 public:
  // js object constructor template
  static Napi::FunctionReference constructor;

  // members
  CURLSH* sh;
  bool isOpen;

  // export Easy to js
  static Napi::Object Initialize(Napi::Env env, Napi::Object exports);

  // js available methods
  static Napi::Value New(const Napi::CallbackInfo& info);
  static Napi::Value SetOpt(const Napi::CallbackInfo& info);
  static Napi::Value Close(const Napi::CallbackInfo& info);
  static Napi::Value StrError(const Napi::CallbackInfo& info);
};
}  // namespace NodeLibcurl
#endif
