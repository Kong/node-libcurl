/**
 * Copyright (c) Jonathan Cardoso Machado. All Rights Reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#ifndef NODELIBCURL_HTTP_2_PUSH_FRAME_HEADERS_H
#define NODELIBCURL_HTTP_2_PUSH_FRAME_HEADERS_H

#include <curl/curl.h>
#include <napi.h>
#include <uv.h>
#include <napi.h>
#include <uv.h>

namespace NodeLibcurl {

class Http2PushFrameHeaders : public Napi::ObjectWrap<Http2PushFrameHeaders> {
  // Private as this can only be created using NewInstance
  Http2PushFrameHeaders(struct curl_pushheaders* headers, size_t numberOfHeaders);
  // Copy constructors cannot be used.
  Http2PushFrameHeaders(const Http2PushFrameHeaders& that);
  Http2PushFrameHeaders& operator=(const Http2PushFrameHeaders& that);

  struct curl_pushheaders* headers;
  size_t numberOfHeaders;

  // js object template
  static v8::Persistent<v8::ObjectTemplate> objectTemplate;

  // js available Methods
  static Napi::Value GetByIndex(const Napi::CallbackInfo& info);
  static Napi::Value GetByName(const Napi::CallbackInfo& info);
  Napi::Value GetterNumberOfHeaders(const Napi::CallbackInfo& info);

 public:
  static Napi::Object NewInstance(struct curl_pushheaders* headers,
                                           size_t numberOfHeaders);

  static Napi::Object Initialize(Napi::Env env, Napi::Object exports);
};

}  // namespace NodeLibcurl
#endif
