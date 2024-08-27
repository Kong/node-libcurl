/**
 * Copyright (c) Jonathan Cardoso Machado. All Rights Reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#ifndef NODELIBCURL_H
#define NODELIBCURL_H

#include "macros.h"

#include <curl/curl.h>
#include <napi.h>
#include <uv.h>
#include <napi.h>
#include <uv.h>

#include <functional>
#include <memory>
#include <vector>

using Napi::ObjectWrap;

namespace NodeLibcurl {

// https://github.com/nodejs/nan/issues/461#issuecomment-140370028
#define NODE_LIBCURL_ADJUST_MEM(size) \
  if (!isLibcurlBuiltWithThreadedResolver) AdjustMemory(size)

// store mapping from the CURL[*] constants that can be used in js
struct CurlConstant {
  const char* name;
  int64_t value;
};

extern ssize_t addonAllocatedMemory;
extern bool isLibcurlBuiltWithThreadedResolver;

template <typename T>
using deleted_unique_ptr = std::unique_ptr<T, std::function<void(T*)>>;

extern const std::vector<CurlConstant> curlOptionNotImplemented;
extern const std::vector<CurlConstant> curlOptionInteger;
extern const std::vector<CurlConstant> curlOptionString;
extern const std::vector<CurlConstant> curlOptionFunction;
extern const std::vector<CurlConstant> curlOptionLinkedList;
extern const std::vector<CurlConstant> curlOptionHttpPost;
extern const std::vector<CurlConstant> curlOptionSpecific;
extern const std::vector<CurlConstant> curlOptionBlob;

extern const std::vector<CurlConstant> curlInfoNotImplemented;
extern const std::vector<CurlConstant> curlInfoString;
extern const std::vector<CurlConstant> curlInfoOffT;
extern const std::vector<CurlConstant> curlInfoDouble;
extern const std::vector<CurlConstant> curlInfoInteger;
extern const std::vector<CurlConstant> curlInfoSocket;
extern const std::vector<CurlConstant> curlInfoLinkedList;

extern const std::vector<CurlConstant> curlMultiOptionNotImplemented;
extern const std::vector<CurlConstant> curlMultiOptionInteger;
extern const std::vector<CurlConstant> curlMultiOptionStringArray;
extern const std::vector<CurlConstant> curlMultiOptionFunction;

// export Curl to js
Napi::Object Initialize(Napi::Env env, Napi::Object exports);

// js exported Methods
Napi::Value GlobalInit(const Napi::CallbackInfo& info);
Napi::Value GlobalCleanup(const Napi::CallbackInfo& info);
Napi::Value GetVersion(const Napi::CallbackInfo& info);
Napi::Value GetCount(const Napi::CallbackInfo& info);
Napi::Value GetterVersionNum(const Napi::CallbackInfo& info);

// helper methods
int32_t IsInsideCurlConstantStruct(const std::vector<CurlConstant>& curlConstants,
                                   const Napi::Value& searchFor);
void ThrowError(const char* message, const char* reason = nullptr);
void AdjustMemory(ssize_t size);

}  // namespace NodeLibcurl
#endif
