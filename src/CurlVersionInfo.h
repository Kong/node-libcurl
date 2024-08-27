/**
 * Copyright (c) Jonathan Cardoso Machado. All Rights Reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#ifndef NODELIBCURL_CURLVERSIONINFO_H
#define NODELIBCURL_CURLVERSIONINFO_H

#include "Curl.h"

#include <curl/curl.h>
#include <napi.h>
#include <uv.h>
#include <napi.h>
#include <uv.h>

namespace NodeLibcurl {

class CurlVersionInfo {
  // instance methods
  CurlVersionInfo();
  ~CurlVersionInfo();

  CurlVersionInfo(const CurlVersionInfo& that);
  CurlVersionInfo& operator=(const CurlVersionInfo& that);

  struct feature {
    const char* name;
    int bitmask;
  };

  static const std::vector<feature> features;

  static const curl_version_info_data* versionInfo;

 public:
  static Napi::Object Initialize(Napi::Env env, Napi::Object exports);

  Napi::Value GetterProtocols(const Napi::CallbackInfo& info);
  Napi::Value GetterFeatures(const Napi::CallbackInfo& info);
};
}  // namespace NodeLibcurl
#endif
