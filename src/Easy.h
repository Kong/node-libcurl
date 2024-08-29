/**
 * Copyright (c) Jonathan Cardoso Machado. All Rights Reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#ifndef NODELIBCURL_EASY_H
#define NODELIBCURL_EASY_H

#include "Multi.h"
#include "libcurl_compat.h"

#include <curl/curl.h>
#include <napi.h>
#include <uv.h>
#include <napi.h>
#include <uv.h>
#include <v8.h>

#include <map>
#include <memory>


namespace NodeLibcurl {

class Easy : public Napi::ObjectWrap<Easy> {
  class ToFree;

  Easy();
  explicit Easy(Easy* orig);
  explicit Easy(CURL* easy);

  Easy(const Easy& that);
  Easy& operator=(const Easy& that);

  ~Easy();

  // instance methods
  void Dispose();
  void ResetRequiredHandleOptions();
  void CallSocketEvent(int status, int events);
  void MonitorSockets();
  void UnmonitorSockets();

  size_t OnData(char* data, size_t size, size_t nmemb);
  size_t OnHeader(char* data, size_t size, size_t nmemb);

  // static members
  static uint32_t counter;

  // callbacks
  typedef std::map<CURLoption, std::shared_ptr<Napi::FunctionReference>> CallbacksMap;
  CallbacksMap callbacks = CallbacksMap{};
  std::shared_ptr<Napi::FunctionReference>
      cbOnSocketEvent;  // still required since it's not related to any CURLOption

  // members
  std::vector<v8::NonCopyablePersistentTraits<v8::Object>> hstsReadCache;
  uint32_t wasHstsReadCacheSet = false;
  uv_poll_t* socketPollHandle = nullptr;
  std::shared_ptr<ToFree> toFree = nullptr;

  bool isCbProgressAlreadyAborted =
      false;  // we need this flag because of
              // https://github.com/curl/curl/commit/907520c4b93616bddea15757bbf0bfb45cde8101
  bool isMonitoringSockets = false;

  int32_t readDataFileDescriptor = -1;  // READDATA sets that
  curl_off_t readDataOffset = -1;       // SEEKDATA sets that
  uint32_t id = counter++;
  CURLU* url = nullptr;
  std::vector<char> urlData;
  bool pathAsIs = false;

  // static methods
  template <typename TResultType, typename Tv8MappingType>
  static Napi::Value GetInfoTmpl(const Easy* obj, int infoId);
  static Napi::Object CreateV8ObjectFromCurlFileInfo(curl_fileinfo* fileInfo);
  static Napi::Object CreateV8ObjectFromCurlHstsEntry(struct curl_hstsentry* sts);

  // js available methods
  static Napi::Value New(const Napi::CallbackInfo& info);
  Napi::Value IdGetter(const Napi::CallbackInfo& info);
  Napi::Value IsInsideMultiHandleGetter(const Napi::CallbackInfo& info);
  Napi::Value IsMonitoringSocketsGetter(const Napi::CallbackInfo& info);
  Napi::Value IsOpenGetter(const Napi::CallbackInfo& info);
  static Napi::Value SetOpt(const Napi::CallbackInfo& info);
  static Napi::Value GetInfo(const Napi::CallbackInfo& info);
  static Napi::Value Send(const Napi::CallbackInfo& info);
  static Napi::Value Recv(const Napi::CallbackInfo& info);
  static Napi::Value Perform(const Napi::CallbackInfo& info);
  static Napi::Value Upkeep(const Napi::CallbackInfo& info);
  static Napi::Value Pause(const Napi::CallbackInfo& info);
  static Napi::Value Reset(const Napi::CallbackInfo& info);
  static Napi::Value DupHandle(const Napi::CallbackInfo& info);
  static Napi::Value OnSocketEvent(const Napi::CallbackInfo& info);
  static Napi::Value MonitorSocketEvents(const Napi::CallbackInfo& info);
  static Napi::Value UnmonitorSocketEvents(const Napi::CallbackInfo& info);
  static Napi::Value Close(const Napi::CallbackInfo& info);
  static Napi::Value StrError(const Napi::CallbackInfo& info);

  // cURL callbacks
  static size_t ReadFunction(char* ptr, size_t size, size_t nmemb, void* userdata);
  static size_t SeekFunction(void* userdata, curl_off_t offset, int origin);
  static size_t HeaderFunction(char* ptr, size_t size, size_t nmemb, void* userdata);
  static size_t WriteFunction(char* ptr, size_t size, size_t nmemb, void* userdata);
  static long CbChunkBgn(curl_fileinfo* transferInfo,  // NOLINT(runtime/int)
                         void* ptr, int remains);
  static long CbChunkEnd(void* ptr);  // NOLINT(runtime/int)
  static int CbDebug(CURL* handle, curl_infotype type, char* data, size_t size, void* userptr);
  static int CbFnMatch(void* ptr, const char* pattern, const char* string);
  static int CbHstsRead(CURL* handle, struct curl_hstsentry* sts, void* userdata);
  static int CbHstsWrite(CURL* handle, struct curl_hstsentry* sts, struct curl_index* count,
                         void* userdata);
  static int CbProgress(void* clientp, double dltotal, double dlnow, double ultotal, double ulnow);
  static int CbTrailer(struct curl_slist** list, void* userdata);
  static int CbXferinfo(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                        curl_off_t ulnow);

  // libuv callbacks
  static void OnSocket(uv_poll_t* handle, int status, int events);
  static void OnSocketClose(uv_handle_t* handle);

 public:
  bool SetUrlOpts();
  static CURLcode SslCtxFunction(CURL* curl, void* sslctx, void* userdata);

  // operators
  bool operator==(const Easy& easy) const;
  bool operator!=(const Easy& other) const;

  static Napi::Object FromCURLHandle(CURL* handle);

  // js object constructor template
  static Napi::FunctionReference constructor;

  // members
  CURL* ch;
  bool isInsideMultiHandle = false;
  bool isOpen = true;

  // used to return callback errors when inside Multi interface
  v8::Persistent<v8::Value> callbackError;

  // static members
  static uint32_t currentOpenedHandles;

  // export Easy to js
  static Napi::Object Initialize(Napi::Env env, Napi::Object exports);
};
}  // namespace NodeLibcurl
#endif
