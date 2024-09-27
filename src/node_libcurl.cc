#ifndef NOMINMAX
# define NOMINMAX // To remove conflicts with recent v8 code std::numeric_limits<int>::max()
#endif
/**
 * Copyright (c) Jonathan Cardoso Machado. All Rights Reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "Curl.h"
#include "CurlVersionInfo.h"
#include "Easy.h"
#include "Http2PushFrameHeaders.h"
#include "Multi.h"
#include "Share.h"

#include <curl/curl.h>
#include <napi.h>
#include <uv.h>
#include <node.h>
#include <v8.h>
#include <node_api.h>
#include <iostream>

namespace NodeLibcurl {

static void AtExitCallback(void* arg) {
  (void)arg;

  curl_global_cleanup();
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  // Initialize(target);
  Easy::Initialize(env, exports);
  Multi::Initialize(env, exports);
  Share::Initialize(env, exports);
  CurlVersionInfo::Initialize(env, exports);
  Http2PushFrameHeaders::Initialize(env, exports);

#if NODE_VERSION_AT_LEAST(11, 0, 0)
  auto context = v8::Isolate::GetCurrent()->GetCurrentContext();
  node::AtExit(node::GetCurrentEnvironment(context), AtExitCallback, NULL);
#else
// this will stay until Node.js v10 support is dropped
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  node::AtExit(AtExitCallback, NULL);
#pragma GCC diagnostic pop
#endif
}

NODE_API_MODULE(node_libcurl, Init);
}  // namespace NodeLibcurl
