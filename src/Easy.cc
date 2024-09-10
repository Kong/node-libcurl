#ifndef NOMINMAX
#define NOMINMAX  // To remove conflicts with recent v8 code std::numeric_limits<int>::max()
#endif
/**
 * Copyright (c) Jonathan Cardoso Machado. All Rights Reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "Easy.h"

#include "Curl.h"
#include "CurlHttpPost.h"
#include "Share.h"
#include "make_unique.h"
#include "napi.h"
#include "uv.h"

#include <curl/curl.h>
#include <curl/urlapi.h>

#include <cctype>
#include <iostream>
#include <string>

extern "C" {

void node_libcurl_ssl_ctx_set_legacy_opts(void* sslctx);
}

// 36055 was allocated on Win64
#define MEMORY_PER_HANDLE 30000

#define TIME_IN_THE_FUTURE "30001231 23:59:59"

namespace NodeLibcurl {

class Easy::ToFree {
 public:
  std::vector<std::vector<char>> str;
  std::vector<curl_slist*> slist;
  std::vector<std::unique_ptr<CurlHttpPost>> post;

  ~ToFree() {
    for (unsigned int i = 0; i < slist.size(); i++) {
      curl_slist_free_all(slist[i]);
    }
  }
};

void assert(bool condition, const std::string& message = "Assertion failed!") {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Napi::FunctionReference Easy::constructor;

uint32_t Easy::counter = 0;
uint32_t Easy::currentOpenedHandles = 0;

Easy::Easy() {
  this->ch = curl_easy_init();
  assert(this->ch && "Could not initialize libcurl easy handle.");

  NODE_LIBCURL_ADJUST_MEM(MEMORY_PER_HANDLE);

  this->toFree = std::make_shared<Easy::ToFree>();
  this->url = curl_url();

  this->ResetRequiredHandleOptions();

  ++Easy::currentOpenedHandles;
}

Easy::Easy(Easy* orig) {
  assert(orig);
  assert(orig != this);  // should not duplicate itself

  this->ch = curl_easy_duphandle(orig->ch);
  assert(this->ch && "Could not duplicate libcurl easy handle.");

  NODE_LIBCURL_ADJUST_MEM(MEMORY_PER_HANDLE);

  // copy the orig callbacks and async resources to the current handle
  this->callbacks.insert(orig->callbacks.begin(), orig->callbacks.end());

  if (orig->cbOnSocketEvent) {
    this->cbOnSocketEvent = orig->cbOnSocketEvent;
  }

  // make sure to reset the *DATA options when duplicating a handle. We are
  // setting all of them, even if they are not set.
  curl_easy_setopt(this->ch, CURLOPT_CHUNK_DATA, this);
  curl_easy_setopt(this->ch, CURLOPT_DEBUGDATA, this);
  curl_easy_setopt(this->ch, CURLOPT_FNMATCH_DATA, this);
  curl_easy_setopt(this->ch, CURLOPT_PROGRESSDATA, this);
#if NODE_LIBCURL_VER_GE(7, 32, 0)
  curl_easy_setopt(this->ch, CURLOPT_XFERINFODATA, this);
#endif
#if NODE_LIBCURL_VER_GE(7, 64, 0)
  curl_easy_setopt(this->ch, CURLOPT_TRAILERDATA, this);
#endif
#if NODE_LIBCURL_VER_GE(7, 74, 0)
  curl_easy_setopt(this->ch, CURLOPT_HSTSREADDATA, this);
  curl_easy_setopt(this->ch, CURLOPT_HSTSWRITEDATA, this);
#endif
  // no need to reset the _DATA option for the READ, SEEK and WRITE callbacks,
  // since they are reset on ResetRequiredHandleOptions()

  this->toFree = orig->toFree;
  this->url = curl_url();
  this->urlData = orig->urlData;
  this->pathAsIs = orig->pathAsIs;

  this->ResetRequiredHandleOptions();

  ++Easy::currentOpenedHandles;
}

// Create a new Easy instance using an existing curl handle
// This is the only constructor that is not private
//  because it's used inside Multi
Easy::Easy(CURL* easy) {
  this->ch = easy;

  char* origEasyPtr = nullptr;

  CURLcode code = curl_easy_getinfo(easy, CURLINFO_PRIVATE, &origEasyPtr);
  // This cannot fail
  assert(code == CURLE_OK);

  NODE_LIBCURL_ADJUST_MEM(MEMORY_PER_HANDLE);

  // We are creating a new Easy instance based in a easy curl handle
  //  that must be being used by another Easy instance.
  // This is basically a copy - just like we have above
  // If origEasyPtr is still null here, it means this is a new easy curl handle
  //  and this scenario should currently never happen
  assert(origEasyPtr != nullptr && "CURLINFO_PRIVATE returned a nullptr which is invalid");

  Easy* orig = reinterpret_cast<Easy*>(origEasyPtr);

  // copy the orig callbacks and async resources to the current handle
  this->callbacks.insert(orig->callbacks.begin(), orig->callbacks.end());

  if (orig->cbOnSocketEvent) {
    this->cbOnSocketEvent = orig->cbOnSocketEvent;
  }

  // make sure to reset the *DATA options when duplicating a handle. We are
  // setting all of them, even if they are not set.
  curl_easy_setopt(this->ch, CURLOPT_CHUNK_DATA, this);
  curl_easy_setopt(this->ch, CURLOPT_DEBUGDATA, this);
  curl_easy_setopt(this->ch, CURLOPT_FNMATCH_DATA, this);
  curl_easy_setopt(this->ch, CURLOPT_PROGRESSDATA, this);
#if NODE_LIBCURL_VER_GE(7, 32, 0)
  curl_easy_setopt(this->ch, CURLOPT_XFERINFODATA, this);
#endif
#if NODE_LIBCURL_VER_GE(7, 64, 0)
  curl_easy_setopt(this->ch, CURLOPT_TRAILERDATA, this);
#endif
#if NODE_LIBCURL_VER_GE(7, 74, 0)
  curl_easy_setopt(this->ch, CURLOPT_HSTSREADDATA, this);
  curl_easy_setopt(this->ch, CURLOPT_HSTSWRITEDATA, this);
#endif
  // no need to reset the _DATA option for the READ, SEEK and WRITE callbacks,
  // since they are reset on ResetRequiredHandleOptions()

  this->toFree = orig->toFree;
  this->url = curl_url();

  this->ResetRequiredHandleOptions();

  ++Easy::currentOpenedHandles;
}

Napi::Object Easy::FromCURLHandle(CURL* handle) {
  Napi::EscapableHandleScope scope(env);

  // create a new js object using this one as the argument for the constructor.
  const int argc = 1;
  Napi::External curlEasyHandle = Napi::External::New(env, reinterpret_cast<void*>(handle));

  Napi::Value argv[argc] = {curlEasyHandle};
  Napi::Function cons = Napi::GetFunction(Napi::New(env, Easy::constructor));

  Napi::Object newInstance = Napi::NewInstance(cons, argc, argv);

  return scope.Escape(newInstance);
}

// Implementation of equality operator overload.
bool Easy::operator==(const Easy& other) const { return this->ch == other.ch; }

bool Easy::operator!=(const Easy& other) const { return !(*this == other); }

Easy::~Easy(void) {
  if (this->isOpen) {
    this->Dispose(env);
  }

  if (this->url) {
    curl_url_cleanup(this->url);
  }
}

void Easy::ResetRequiredHandleOptions() {
  curl_easy_setopt(this->ch, CURLOPT_PRIVATE,
                   this);  // to be used with Multi handle

  curl_easy_setopt(this->ch, CURLOPT_HEADERFUNCTION, Easy::HeaderFunction);
  curl_easy_setopt(this->ch, CURLOPT_HEADERDATA, this);

  curl_easy_setopt(this->ch, CURLOPT_READFUNCTION, Easy::ReadFunction);
  curl_easy_setopt(this->ch, CURLOPT_READDATA, this);

  curl_easy_setopt(this->ch, CURLOPT_SEEKFUNCTION, Easy::SeekFunction);
  curl_easy_setopt(this->ch, CURLOPT_SEEKDATA, this);

  curl_easy_setopt(this->ch, CURLOPT_WRITEFUNCTION, Easy::WriteFunction);
  curl_easy_setopt(this->ch, CURLOPT_WRITEDATA, this);

#if NODE_LIBCURL_VER_GE(7, 11, 0)
  curl_easy_setopt(this->ch, CURLOPT_SSL_CTX_FUNCTION, Easy::SslCtxFunction);
  curl_easy_setopt(this->ch, CURLOPT_SSL_CTX_DATA, this);
#endif
}

bool Easy::SetUrlOpts() {
  if (this->urlData.empty()) {
    return true;
  }

  unsigned int flags = CURLU_GUESS_SCHEME | CURLU_NON_SUPPORT_SCHEME;

  flags |= this->pathAsIs ? CURLU_PATH_AS_IS : 0;

#if NODE_LIBCURL_VER_GE(7, 78, 0)
  flags |= CURLU_ALLOW_SPACE;
#endif

  CURLUcode status;
  if ((status = curl_url_set(this->url, CURLUPART_URL, &this->urlData[0], flags)) != CURLUE_OK) {
    return false;
  }

  curl_easy_setopt(this->ch, CURLOPT_CURLU, this->url);
  return true;
}

CURLcode Easy::SslCtxFunction(CURL* curl, void* sslctx, void* userdata) {
  Easy* obj = static_cast<Easy*>(userdata);
  (void)obj;

  node_libcurl_ssl_ctx_set_legacy_opts(sslctx);

  return CURLE_OK;
}

// Dispose persistent objects and references stored during the life of this obj.
void Easy::Dispose(Napi::Env env) {
  // this call should only be done when the handle is still open
  assert(this->isOpen && "This handle was already closed.");
  assert(this->ch && "The curl handle ran away.");

  curl_easy_cleanup(this->ch);

  NODE_LIBCURL_ADJUST_MEM(-MEMORY_PER_HANDLE);

  if (this->isMonitoringSockets) {
    this->UnmonitorSockets(env);
  }

  this->isOpen = false;

  this->callbackError.Reset();

  --Easy::currentOpenedHandles;
}

void Easy::MonitorSockets(Napi::Env env) {
  int retUv;
  CURLcode retCurl;
  int events = 0 | UV_READABLE | UV_WRITABLE;

  if (this->socketPollHandle) {
    throw Napi::Error::New(env, "Already monitoring sockets!");
  }

#if NODE_LIBCURL_VER_GE(7, 45, 0)
  curl_socket_t socket;
  retCurl = curl_easy_getinfo(this->ch, CURLINFO_ACTIVESOCKET, &socket);

  if (socket == CURL_SOCKET_BAD) {
    throw Napi::Error::New(env, "Received invalid socket from the current connection!");
  }
#else
  long socket;  // NOLINT(runtime/int)
  retCurl = curl_easy_getinfo(this->ch, CURLINFO_LASTSOCKET, &socket);
#endif

  if (retCurl != CURLE_OK) {
    std::string errorMsg;

    errorMsg += std::string("Failed to receive socket. Reason: ") + curl_easy_strerror(retCurl);

    throw Napi::Error::New(env, errorMsg.c_str());
  }

  this->socketPollHandle = new uv_poll_t;

  retUv = uv_poll_init_socket(uv_default_loop(), this->socketPollHandle, socket);

  if (retUv < 0) {
    std::string errorMsg;

    errorMsg +=
        std::string("Failed to poll on connection socket. Reason:") + UV_ERROR_STRING(retUv);

    throw Napi::Error::New(env, errorMsg.c_str());
  }

  this->socketPollHandle->data = this;

  retUv = uv_poll_start(this->socketPollHandle, events, Easy::OnSocket);
  this->isMonitoringSockets = true;
}

void Easy::UnmonitorSockets(Napi::Env env) {
  int retUv;
  retUv = uv_poll_stop(this->socketPollHandle);

  if (retUv < 0) {
    std::string errorMsg;

    errorMsg += std::string("Failed to stop polling on socket. Reason: ") + UV_ERROR_STRING(retUv);

    throw Napi::Error::New(env, errorMsg.c_str());
  }

  uv_close(reinterpret_cast<uv_handle_t*>(this->socketPollHandle), Easy::OnSocketClose);
  this->isMonitoringSockets = false;
}

void Easy::OnSocket(uv_poll_t* handle, int status, int events) {
  Easy* obj = static_cast<Easy*>(handle->data);

  assert(obj);

  obj->CallSocketEvent(status, events);
}

void Easy::OnSocketClose(uv_handle_t* handle) { delete handle; }

void Easy::CallSocketEvent(int status, int events) {
  if (this->cbOnSocketEvent == nullptr) {
    return;
  }
  
  Napi::HandleScope scope(env);

  Napi::Value err = env.Null();

  if (status < 0) {
    err = Napi::Error::New(env, UV_ERROR_STRING(status));
  }

  const int argc = 2;
  Napi::Value argv[argc] = {err, Napi::Number::New(env, events)};

  // **(this->cbOnSocketEvent.get()) is the same than this->cbOnSocketEvent->GetFunction()
  Napi::AsyncResource asyncResource("Easy::CallSocketEvent");
  asyncResource.runInAsyncScope(this->handle(), this->cbOnSocketEvent->GetFunction(), argc, argv);
}

// Called by libcurl when some chunk of data (from body) is available
size_t Easy::WriteFunction(char* ptr, size_t size, size_t nmemb, void* userdata) {
  Easy* obj = static_cast<Easy*>(userdata);
  return obj->OnData(ptr, size, nmemb);
}

// Called by libcurl when some chunk of data (from headers) is available
size_t Easy::HeaderFunction(char* ptr, size_t size, size_t nmemb, void* userdata) {
  Easy* obj = static_cast<Easy*>(userdata);
  return obj->OnHeader(ptr, size, nmemb);
}

// Called by libcurl as soon as it needs to read data in order to send it to the
// peer
size_t Easy::ReadFunction(char* ptr, size_t size, size_t nmemb, void* userdata) {
  uv_fs_t readReq;

  int32_t returnValue = CURL_READFUNC_ABORT;

  Easy* obj = static_cast<Easy*>(userdata);
  int32_t fd = obj->readDataFileDescriptor;

  size_t n = size * nmemb;

  CallbacksMap::iterator it = obj->callbacks.find(CURLOPT_READFUNCTION);

  // Read callback was set, use it instead
  if (it != obj->callbacks.end()) {
    Napi::HandleScope scope(env);

  Napi::Buffer<char> buf = Napi::Buffer<char>::New(env, static_cast<size_t>(n)); 
  Napi::Number sizeArg = Napi::Number::New(env, static_cast<uint32_t>(size));    
  Napi::Number nmembArg = Napi::Number::New(env, static_cast<uint32_t>(nmemb));
  const int argc = 3;
  Napi::Value argv[argc] = {
      buf,
      sizeArg,
      nmembArg,
  };

    Napi::TryCatch tryCatch;
    Napi::AsyncResource asyncResource("Easy::ReadFunction");
    Napi::MaybeLocal<v8::Value> returnValueCallback =
        asyncResource.runInAsyncScope(obj->handle(), it->second->GetFunction(), argc, argv);

    if (tryCatch.HasCaught()) {
      if (obj->isInsideMultiHandle) {
        obj->callbackError.Reset(tryCatch.Exception());
      } else {
        tryCatch.ReThrow();
      }
      return returnValue;
    }

    if (returnValueCallback.IsEmpty() || !returnValueCallback.IsNumber()) {
      Napi::Value typeError =
          Napi::TypeError::New(env, "Return value from the READ callback must be an integer.");
      if (obj->isInsideMultiHandle) {
        obj->callbackError.Reset(typeError);
      } else {
        throw Napi::Error::New(env, typeError);
      }
      return returnValue;
    } else {
      returnValue = returnValueCallback.As<Napi::Number>();
    }

    char* data = buf.As<Napi::Buffer<char>>().Data();

    bool hasData = !!data && returnValue > 0 && returnValue < CURL_READFUNC_ABORT;

    if (hasData) {
      std::memcpy(ptr, data, returnValue);
    }

    // otherwise use the default read callback
  } else {
    // abort early if we don't have a file descriptor
    if (fd == -1) {
      return CURL_READFUNC_ABORT;
    }

    // get the offset
    curl_off_t offset = obj->readDataOffset;
    if (offset >= 0) {
      // increment it for the next read
      obj->readDataOffset += n;
    }

#if UV_VERSION_MAJOR < 1
    returnValue = uv_fs_read(uv_default_loop(), &readReq, fd, ptr, n, offset, NULL);
#else
    uv_buf_t uvbuf = uv_buf_init(ptr, (unsigned int)(n));

    returnValue = uv_fs_read(uv_default_loop(), &readReq, fd, &uvbuf, 1, offset, NULL);
#endif
  }

  if (returnValue < 0) {
    return CURL_READFUNC_ABORT;
  }

  return static_cast<size_t>(returnValue);
}

size_t Easy::SeekFunction(void* userdata, curl_off_t offset, int origin) {
  Easy* obj = static_cast<Easy*>(userdata);

  int32_t returnValue = CURL_SEEKFUNC_FAIL;

  CallbacksMap::iterator it = obj->callbacks.find(CURLOPT_READFUNCTION);

  // Read callback was set, look for a seek callback
  if (it != obj->callbacks.end()) {
    it = obj->callbacks.find(CURLOPT_SEEKFUNCTION);

    // Seek callback was set, use it instead
    if (it != obj->callbacks.end()) {
      Napi::HandleScope scope(env);

      Napi::Number offsetArg = Napi::Number::New(env, static_cast<uint32_t>(offset));
      Napi::Number originArg = Napi::Number::New(env, static_cast<uint32_t>(origin));
      const int argc = 2;
      Napi::Value argv[argc] = {
          offsetArg,
          originArg,
      };

      Napi::TryCatch tryCatch;
      Napi::AsyncResource asyncResource("Easy::SeekFunction");
      Napi::MaybeLocal<v8::Value> returnValueCallback =
          asyncResource.runInAsyncScope(obj->handle(), it->second->GetFunction(), argc, argv);

      if (tryCatch.HasCaught()) {
        if (obj->isInsideMultiHandle) {
          obj->callbackError.Reset(tryCatch.Exception());
        } else {
          tryCatch.ReThrow();
        }
        return returnValue;
      }

      if (returnValueCallback.IsEmpty() || !returnValueCallback.IsNumber()) {
        Napi::Value typeError =
            Napi::TypeError::New(env, "Return value from the SEEK callback must be an integer.");
        if (obj->isInsideMultiHandle) {
          obj->callbackError.Reset(typeError);
        } else {
          throw Napi::Error::New(env, typeError);
        }
      } else {
        returnValue = returnValueCallback.As<Napi::Number>().Int32Value();
      }

      // otherwise we can't seek directly
    } else {
      returnValue = CURL_SEEKFUNC_CANTSEEK;
    }

    // otherwise use the default seek callback
  } else {
    obj->readDataOffset = offset;
    returnValue = CURL_SEEKFUNC_OK;
  }

  return returnValue;
}

size_t Easy::OnData(char* data, size_t size, size_t nmemb) {
  Napi::HandleScope scope(env);

  size_t dataLength = size * nmemb;

  CallbacksMap::iterator it = this->callbacks.find(CURLOPT_WRITEFUNCTION);

  bool hasWriteCallback = (it != this->callbacks.end());

  // No callback is set
  if (!hasWriteCallback) {
    return dataLength;
  }

  // if this gets returned it will cause a CURLE_WRITE_ERROR
  int32_t returnValue = -1;

  const int argc = 3;
  Napi::Buffer<char> buf = Napi::Buffer<char>::Copy(env, data, static_cast<size_t>(dataLength));
  Napi::Number sizeArg = Napi::Number::New(env, static_cast<uint32_t>(size));
  Napi::Number nmembArg = Napi::Number::New(env, static_cast<uint32_t>(nmemb));

  std::vector<napi_value> argv = { buf, sizeArg, nmembArg };

  Napi::TryCatch tryCatch;
  Napi::AsyncResource asyncResource("Easy::OnData");
  Napi::MaybeLocal<v8::Value> returnValueCallback =
      asyncResource.runInAsyncScope(this->handle(), it->second->GetFunction(), argc, argv);

  if (tryCatch.HasCaught()) {
    if (this->isInsideMultiHandle) {
      this->callbackError.Reset(tryCatch.Exception());
    } else {
      tryCatch.ReThrow();
    }
    return returnValue;
  }

  if (returnValueCallback.IsEmpty() || !returnValueCallback.IsNumber()) {
    Napi::Value typeError =
        Napi::TypeError::New(env, "Return value from the WRITE callback must be an integer.");
    if (this->isInsideMultiHandle) {
      this->callbackError.Reset(typeError);
    } else {
      throw Napi::Error::New(env, typeError);
    }
    return returnValue;
  } else {
    returnValue = returnValueCallback.As<Napi::Number>().Int32Value();
  }

  return returnValue;
}

size_t Easy::OnHeader(char* data, size_t size, size_t nmemb) {
  Napi::HandleScope scope(env);

  size_t dataLength = size * nmemb;

  CallbacksMap::iterator it = this->callbacks.find(CURLOPT_HEADERFUNCTION);

  bool hasHeaderCallback = (it != this->callbacks.end());

  // No callback is set
  if (!hasHeaderCallback) {
    return dataLength;
  }

  // if this gets returned it will cause a CURLE_WRITE_ERROR
  int32_t returnValue = -1;

  const int argc = 3;
  Napi::Buffer<char> buf = Napi::Buffer<char>::Copy(env, data, static_cast<size_t>(dataLength));
  Napi::Number sizeArg = Napi::Number::New(env, static_cast<uint32_t>(size));
  Napi::Number nmembArg = Napi::Number::New(env, static_cast<uint32_t>(nmemb));

  std::vector<napi_value> argv = { buf, sizeArg, nmembArg };


  Napi::TryCatch tryCatch;
  Napi::AsyncResource asyncResource("Easy::OnHeader");
  Napi::MaybeLocal<v8::Value> returnValueCallback =
      asyncResource.runInAsyncScope(this->handle(), it->second->GetFunction(), argc, argv);

  if (tryCatch.HasCaught()) {
    if (this->isInsideMultiHandle) {
      this->callbackError.Reset(tryCatch.Exception());
    } else {
      tryCatch.ReThrow();
    }
    return returnValue;
  }

  if (returnValueCallback.IsEmpty() || !returnValueCallback.IsNumber()) {
    Napi::Value typeError =
        Napi::TypeError::New(env, "Return value from the HEADER callback must be an integer.");
    if (this->isInsideMultiHandle) {
      this->callbackError.Reset(typeError);
    } else {
      throw Napi::Error::New(env, typeError);
    }
    return returnValue;
  } else {
    returnValue = returnValueCallback.As<Napi::Number>().Int32Value();
  }

  return returnValue;
}

Napi::Value NullValueIfInvalidString(char* str) {
  Napi::EscapableHandleScope scope(env);

  Napi::Value ret = env.Null();

  if (str != NULL && str[0] != '\0') {
    ret = Napi::String::New(env, str);
  }

  return scope.Escape(ret);
}

Napi::Object Easy::CreateV8ObjectFromCurlFileInfo(curl_fileinfo* fileInfo) {
  Napi::Env env = Napi::Env();
  Napi::EscapableHandleScope scope(env);

  Napi::String fileName = Napi::String:::New(env, fileInfo->filename);
  Napi::Number fileType = Napi::Number::New(env, fileInfo->filetype);
  Napi::Value time = env.Null();

  if (fileInfo->time != 0)
    time = Napi::Date::New(env, static_cast<double>(fileInfo->time) * 1000).As<Napi::Number>();

  Napi::Number perm = Napi::Number::New(env, fileInfo->perm);
  Napi::Number uid = Napi::Number::New(env, fileInfo->uid);
  Napi::Number gid = Napi::Number::New(env, fileInfo->gid);
  Napi::Number size = Napi::Number::New(env, static_cast<double>(fileInfo->size));
  Napi::Number hardLinks = Napi::Number::New(env, static_cast<int32_t>(fileInfo->hardlinks));

  Napi::Object strings = Napi::Object::New(env);
  (strings).Set(Napi::String::New(env, "time"), NullValueIfInvalidString(fileInfo->strings.time));
  (strings).Set(Napi::String::New(env, "perm"), NullValueIfInvalidString(fileInfo->strings.perm));
  (strings).Set(Napi::String::New(env, "user"), NullValueIfInvalidString(fileInfo->strings.user));
  (strings).Set(Napi::String::New(env, "group"), NullValueIfInvalidString(fileInfo->strings.group));
  (strings).Set(Napi::String::New(env, "target"),
                NullValueIfInvalidString(fileInfo->strings.target));

  Napi::Object obj = Napi::Object::New(env);
  (obj).Set(Napi::String::New(env, "fileName"), fileName);
  (obj).Set(Napi::String::New(env, "fileType"), fileType);
  (obj).Set(Napi::String::New(env, "time"), time);
  (obj).Set(Napi::String::New(env, "perm"), perm);
  (obj).Set(Napi::String::New(env, "uid"), uid);
  (obj).Set(Napi::String::New(env, "gid"), gid);
  (obj).Set(Napi::String::New(env, "size"), size);
  (obj).Set(Napi::String::New(env, "hardLinks"), hardLinks);
  (obj).Set(Napi::String::New(env, "strings"), strings);

  return scope.Escape(obj);
}

Napi::Object Easy::CreateV8ObjectFromCurlHstsEntry(struct curl_hstsentry* sts) {
  Napi::EscapableHandleScope scope(env);

  auto hasExpire = !!sts->expire[0] && !!strcmp(sts->expire, TIME_IN_THE_FUTURE);

  Napi::String host = Napi::Number::New(env, sts->name);
  Napi::Boolean includeSubDomains = Napi::Boolean::New(env, !!sts->includeSubDomains);
  Napi::String expire =
      hasExpire ? Napi::String::New(env, sts->expire) : Napi::String::New(env.Null());

  Napi::Object obj = Napi::Object::New(env);
  (obj).Set(Napi::String::New(env, "host"), host);
  (obj).Set(Napi::String::New(env, "includeSubDomains"), includeSubDomains);
  (obj).Set(Napi::String::New(env, "expire"), expire);

  return scope.Escape(obj);
}

long Easy::CbChunkBgn(curl_fileinfo* transferInfo, void* ptr, int remains) {  // NOLINT(runtime/int)
  Easy* obj = static_cast<Easy*>(ptr);

  assert(obj);

  CallbacksMap::iterator it = obj->callbacks.find(CURLOPT_CHUNK_BGN_FUNCTION);
  assert(it != obj->callbacks.end() && "CHUNK_BGN callback not set.");

  const int argc = 2;
  Napi::Value argv[argc] = {Easy::CreateV8ObjectFromCurlFileInfo(transferInfo),
                            Napi::Number::New(env, remains)};

  int32_t returnValue = CURL_CHUNK_BGN_FUNC_FAIL;

  Napi::TryCatch tryCatch;

  Napi::AsyncResource asyncResource("Easy::CbChunkBgn");
  Napi::MaybeLocal<v8::Value> returnValueCallback =
      asyncResource.runInAsyncScope(obj->handle(), it->second->GetFunction(), argc, argv);

  if (tryCatch.HasCaught()) {
    if (obj->isInsideMultiHandle) {
      obj->callbackError.Reset(tryCatch.Exception());
    } else {
      tryCatch.ReThrow();
    }
    return returnValue;
  }

  if (returnValueCallback.IsEmpty() || !returnValueCallback.IsNumber()) {
    Napi::Value typeError =
        Napi::TypeError::New(env, "Return value from the CHUNK_BGN callback must be an integer.");

    if (obj->isInsideMultiHandle) {
      obj->callbackError.Reset(typeError);
    } else {
      throw Napi::Error::New(env, typeError);
    }
  } else {
    returnValue = returnValueCallback.As<Napi::Number>().Int32Value();
  }

  return returnValue;
}

long Easy::CbChunkEnd(void* ptr) {  // NOLINT(runtime/int)
  Easy* obj = static_cast<Easy*>(ptr);

  assert(obj);

  CallbacksMap::iterator it = obj->callbacks.find(CURLOPT_CHUNK_END_FUNCTION);
  assert(it != obj->callbacks.end() && "CHUNK_END callback not set.");

  int32_t returnValue = CURL_CHUNK_END_FUNC_FAIL;

  Napi::TryCatch tryCatch;

  Napi::AsyncResource asyncResource("Easy::CbChunkEnd");
  Napi::MaybeLocal<v8::Value> returnValueCallback =
      asyncResource.runInAsyncScope(obj->handle(), it->second->GetFunction(), 0, NULL);

  if (tryCatch.HasCaught()) {
    if (obj->isInsideMultiHandle) {
      obj->callbackError.Reset(tryCatch.Exception());
    } else {
      tryCatch.ReThrow();
    }
    return returnValue;
  }

  if (returnValueCallback.IsEmpty() || !returnValueCallback.IsNumber()) {
    Napi::Value typeError =
        Napi::TypeError::New(env, "Return value from the CHUNK_END callback must be an integer.");
    if (obj->isInsideMultiHandle) {
      obj->callbackError.Reset(typeError);
    } else {
      throw Napi::Error::New(env, typeError);
    }
  } else {
    returnValue = returnValueCallback.As<Napi::Number>().Int32Value();
  }

  return returnValue;
}

int Easy::CbDebug(CURL* handle, curl_infotype type, char* data, size_t size, void* userptr) {
  Napi::HandleScope scope(env);

  Easy* obj = static_cast<Easy*>(userptr);

  assert(obj);

  CallbacksMap::iterator it = obj->callbacks.find(CURLOPT_DEBUGFUNCTION);
  assert(it != obj->callbacks.end() && "DEBUG callback not set.");

  const int argc = 2;
  Napi::Buffer<char> buf = Napi::Buffer<char>::Copy(env, data, static_cast<size_t>(size));
  Napi::Value argv[argc] = {
      Napi::Number::New(env, type),  // Assuming 'type' is a number
      buf,
  };

  int32_t returnValue = 1;

  Napi::TryCatch tryCatch;

  Napi::AsyncResource asyncResource("Easy::CbDebug");
  Napi::MaybeLocal<v8::Value> returnValueCallback =
      asyncResource.runInAsyncScope(obj->handle(), it->second->GetFunction(), argc, argv);

  if (tryCatch.HasCaught()) {
    if (obj->isInsideMultiHandle) {
      obj->callbackError.Reset(tryCatch.Exception());
    } else {
      tryCatch.ReThrow();
    }
    return returnValue;
  }

  if (returnValueCallback.IsEmpty() || !returnValueCallback.IsNumber()) {
    Napi::Value typeError =
        Napi::TypeError::New(env, "Return value from the DEBUG callback must be an integer.");
    if (obj->isInsideMultiHandle) {
      obj->callbackError.Reset(typeError);
    } else {
      throw Napi::Error::New(env, typeError)
    }
  } else {
    returnValue = returnValueCallback.As<Napi::Number>().Int32Value();
  }

  return returnValue;
}

int Easy::CbFnMatch(void* ptr, const char* pattern, const char* string) {
  Napi::HandleScope scope(env);

  Easy* obj = static_cast<Easy*>(ptr);

  assert(obj);

  CallbacksMap::iterator it = obj->callbacks.find(CURLOPT_FNMATCH_FUNCTION);
  assert(it != obj->callbacks.end() && "FNMATCH callback not set.");

  const int argc = 2;
  Napi::Value argv[argc] = {Napi::String::New(env, pattern), Napi::String::New(env, string)};

  int32_t returnValue = CURL_FNMATCHFUNC_FAIL;

  Napi::TryCatch tryCatch;

  Napi::AsyncResource asyncResource("Easy::CbFnMatch");
  Napi::MaybeLocal<v8::Value> returnValueCallback =
      asyncResource.runInAsyncScope(obj->handle(), it->second->GetFunction(), argc, argv);

  if (tryCatch.HasCaught()) {
    if (obj->isInsideMultiHandle) {
      obj->callbackError.Reset(tryCatch.Exception());
    } else {
      tryCatch.ReThrow();
    }
    return returnValue;
  }

  if (returnValueCallback.IsEmpty() || !returnValueCallback.IsNumber()) {
    Napi::Value typeError =
        Napi::TypeError::New(env, "Return value from the FNMATCH callback must be an integer.");
    if (obj->isInsideMultiHandle) {
      obj->callbackError.Reset(typeError);
    } else {
      throw Napi::Error::New(env, typeError);
    }
  } else {
    returnValue = returnValueCallback.As<Napi::Number>().Int32Value();
  }

  return returnValue;
}

int Easy::CbHstsRead(CURL* handle, struct curl_hstsentry* sts, void* userdata) {
#if NODE_LIBCURL_VER_GE(7, 74, 0)
  Napi::HandleScope scope(env);

  Easy* obj = static_cast<Easy*>(userdata);

  assert(obj);

  CallbacksMap::iterator it = obj->callbacks.find(CURLOPT_HSTSREADFUNCTION);
  assert(it != obj->callbacks.end() && "HSTSREADFUNCTION callback not set.");

  int32_t returnValue = CURLSTS_FAIL;

  Napi::TryCatch tryCatch;
  Napi::Value cacheEntryObject;

  Napi::Value typeError = Napi::TypeError(
      "Return value from the HSTSREADFUNCTION callback must be one of the following:\n"
      "  - Object matching the type CurlHstsEntry\n"
      "  - An array matching the type CurlHstsEntry[]\n"
      "  - null\n"
      "Libcurl <= 7.79.0 does not stop requests from firing if there are errors in the HSTS "
      "callback, thus you may be receiving an error while the request did in fact work. Please "
      "fix "
      "the HSTS callback to return the correct data to avoid this.");

  if (obj->hstsReadCache.size() > 0) {
    auto persistentValue = obj->hstsReadCache.back();
    cacheEntryObject = Napi::Object::New(env, obj->hstsReadCache.back());

    // reset the persistent handler so we do not leak memory
    persistentValue.Reset();
    // remove it from the stack
    obj->hstsReadCache.pop_back();
  } else {
    // if this is true, this means we got all the entries in the cache provided by the user
    if (obj->wasHstsReadCacheSet) {
      obj->wasHstsReadCacheSet = false;
      return CURLSTS_DONE;
    }

    Napi::AsyncResource asyncResource("Easy::CbHstsRead");
    Napi::MaybeLocal<v8::Value> returnValueFromHstsReadCallback =
        asyncResource.runInAsyncScope(obj->handle(), it->second->GetFunction(), 0, NULL);

    if (tryCatch.HasCaught()) {
      if (obj->isInsideMultiHandle) {
        obj->callbackError.Reset(tryCatch.Exception());
      } else {
        tryCatch.ReThrow();
      }
      return returnValue;
    }

    if (returnValueFromHstsReadCallback.IsEmpty()) {
      THROW_ERROR_OR_SET_MULTI_CALLBACK_ERROR_IF_INSIDE_MULTI(typeError)
      return returnValue;
    }

    cacheEntryObject = returnValueFromHstsReadCallback;
  }

  if (cacheEntryObject.IsNull()) {
    return CURLSTS_DONE;
  } else {
    // returning an array from the callback can be used to avoid multiple
    // context switches between v8 and js
    if (cacheEntryObject.IsArray()) {
      auto cacheArray = cacheEntryObject.As<Napi::Array>();
      auto cacheArrayLength = cacheArray.Length();

      if (cacheArrayLength == 0) {
        return CURLSTS_DONE;
      }

      // inserting in reverse order as we are processing the hstsReadCache stack from back to front
      for (int i = cacheArrayLength - 1; i >= 0; i--) {
        auto idxValue = (cacheArray).Get(i);

        assert(!idxValue.IsEmpty() &&
               "Value inside array could not be found - Process may be running out of memory");

        auto idxValueChecked = idxValue;

        // we check for an array here too to avoid passing a child array here.
        // If that happens, the code would get to this condition again when we
        // process this cache entry in a future iteration
        if (!idxValueChecked.IsObject() || idxValueChecked.IsArray()) {
          THROW_ERROR_OR_SET_MULTI_CALLBACK_ERROR_IF_INSIDE_MULTI(typeError)
          return returnValue;
        }

        auto idxValueAsObject = idxValueChecked.As<Napi::Object>();

        v8::NonCopyablePersistentTraits<v8::Object>::CopyablePersistent persistentValue;

        persistentValue.Reset(Napi::GetCurrentContext()->GetIsolate(), idxValueAsObject);

        obj->hstsReadCache.push_back(persistentValue);
      }

      auto persistentValue = obj->hstsReadCache.back();
      cacheEntryObject = Napi::Object::New(env, obj->hstsReadCache.back());

      persistentValue.Reset();
      obj->hstsReadCache.pop_back();
      obj->wasHstsReadCacheSet = true;
    }

    if (cacheEntryObject.IsObject()) {
      // napi would make this so much cleaner...

      auto cacheEntry = cacheEntryObject.As<Napi::Object>();

      auto hostPropertyStr = Napi::String::New(env, "host");
      auto includeSubDomainsPropertyStr = Napi::String::New(env, "includeSubDomains");
      auto expirePropertyStr = Napi::String::New(env, "expire");

      auto hostPropertyValue = (cacheEntry).Get(hostPropertyStr);
      auto includeSubDomainsPropertyValue = (cacheEntry).Get(includeSubDomainsPropertyStr);
      auto expirePropertyValue = (cacheEntry).Get(expirePropertyStr);

      if (hostPropertyValue.IsEmpty() || includeSubDomainsPropertyValue.IsEmpty() ||
          expirePropertyValue.IsEmpty()) {
        assert("Process ran out of memory - fields returned from HSTSREADFUNCTION were empty");
      }

      auto hostPropertyValueChecked = hostPropertyValue;
      auto includeSubDomainsPropertyValueChecked = includeSubDomainsPropertyValue;
      auto expirePropertyValueChecked = expirePropertyValue;

      // the validation here is pretty basic, and we are not really validating
      // the format of the expire string - libcurl should do that

      // make sure the provided data is valid
      if (!hostPropertyValueChecked.IsString() ||
          (!includeSubDomainsPropertyValueChecked->IsNullOrUndefined() &&
           !includeSubDomainsPropertyValueChecked->IsBoolean()) ||
          (!expirePropertyValueChecked->IsNullOrUndefined() &&
           !expirePropertyValueChecked.IsString())) {
        THROW_ERROR_OR_SET_MULTI_CALLBACK_ERROR_IF_INSIDE_MULTI(typeError)
        return returnValue;
      }

      std::string hostStrValue = hostPropertyValueChecked.As<Napi::String>();

      // make sure str len is inside the given max length
      if (static_cast<size_t>(hostStrValue.length()) > sts->namelen) {
        Napi::Value typeError = Napi::TypeError(
            "The host property value returned from the HSTSREADFUNCTION callback function was "
            "invalid. The host string is too long.\n"
            "Libcurl <= 7.79.0 does not stop requests from firing if there are errors in the HSTS "
            "callback, thus you may be receiving an error while the request did in fact work. "
            "Please fix the HSTS callback to return the correct data to avoid this.");
        THROW_ERROR_OR_SET_MULTI_CALLBACK_ERROR_IF_INSIDE_MULTI(typeError)

        return returnValue;
      }

      sts->name = *hostStrValue;
      sts->includeSubDomains = includeSubDomainsPropertyValueChecked.As<Napi::Boolean>().Value();

      if (expirePropertyValueChecked.IsString()) {
        // make sure expire length is one expected by libcurl
        // YYYYMMDD HH:MM:SS [null-terminated]
        size_t currentSize =
            static_cast<size_t>(expirePropertyValueChecked.As<Napi::String>().Length());
        size_t expectedSize = sizeof(sts->expire) / sizeof(sts->expire[0]) - 1;

        if (currentSize != expectedSize) {
          Napi::Value typeError = Napi::TypeError(
              "The expire property value returned from the HSTSREADFUNCTION callback function was "
              "invalid. String is either too long, or too short.\n"
              "Libcurl <= 7.79.0 does not stop requests from firing if there are errors in the "
              "HSTS "
              "callback, thus you may be receiving an error while the request did in fact work. "
              "Please fix the HSTS callback to return the correct data to avoid this.");
          THROW_ERROR_OR_SET_MULTI_CALLBACK_ERROR_IF_INSIDE_MULTI(typeError)

          return returnValue;
        }

        std::string expireStrValue = expirePropertyValueChecked.As<Napi::String>();
        auto expireCharValue = *expireStrValue;

        strcpy(sts->expire, expireCharValue);
      } else {
        // TODO(jonathan): libcurl <= 7.79 has a bug when expire is not set, see:
        // https://github.com/curl/curl/issues/7720 - to avoid this bug we are setting it manually
        // to a future date here
        strcpy(sts->expire, TIME_IN_THE_FUTURE);
      }
      returnValue = CURLSTS_OK;
    } else {
      THROW_ERROR_OR_SET_MULTI_CALLBACK_ERROR_IF_INSIDE_MULTI(typeError)
    }
  }

  return returnValue;
#else
  return 0;
#endif
}

int Easy::CbHstsWrite(CURL* handle, struct curl_hstsentry* sts, struct curl_index* count,
                      void* userdata) {
#if NODE_LIBCURL_VER_GE(7, 74, 0)
  Napi::HandleScope scope(env);

  Easy* obj = static_cast<Easy*>(userdata);

  assert(obj);

  CallbacksMap::iterator it = obj->callbacks.find(CURLOPT_HSTSWRITEFUNCTION);
  assert(it != obj->callbacks.end() && "HSTSWRITEFUNCTION callback not set.");

  int32_t returnValue = CURLSTS_FAIL;

  Napi::TryCatch tryCatch;
  Napi::Value value;

  Napi::Value typeError = Napi::TypeError::New(
      env, "Return value from the HSTSWRITEFUNCTION callback must be an integer.");

  // TODO(jonathan): give the option to receive an array directly?

  Napi::Object countObj = Napi::Object::New(env);
  Napi::Number index = Napi::Number::New(env, static_cast<uint32_t>(count->index));
  Napi::Number total = Napi::Number::New(env, static_cast<uint32_t>(count->total));
  (countObj).Set(Napi::String::New(env, "index"), index);
  (countObj).Set(Napi::String::New(env, "total"), total);

  const int argc = 2;
  Napi::Value argv[argc] = {Easy::CreateV8ObjectFromCurlHstsEntry(sts), countObj};

  Napi::AsyncResource asyncResource("Easy::CbHstsWrite");
  Napi::MaybeLocal<v8::Value> returnValueCallback =
      asyncResource.runInAsyncScope(obj->handle(), it->second->GetFunction(), argc, argv);

  if (tryCatch.HasCaught()) {
    if (obj->isInsideMultiHandle) {
      obj->callbackError.Reset(tryCatch.Exception());
    } else {
      tryCatch.ReThrow();
    }
    return returnValue;
  }

  if (returnValueCallback.IsEmpty()) {
    THROW_ERROR_OR_SET_MULTI_CALLBACK_ERROR_IF_INSIDE_MULTI(typeError)
    return returnValue;
  }

  value = returnValueCallback;

  if (!value.IsNumber()) {
    THROW_ERROR_OR_SET_MULTI_CALLBACK_ERROR_IF_INSIDE_MULTI(typeError)
    return returnValue;
  }

  returnValue = value.As<Napi::Number>().Int32Value();

  return returnValue;
#else
  return 0;
#endif
}

int Easy::CbProgress(void* clientp, double dltotal, double dlnow, double ultotal, double ulnow) {
  Napi::HandleScope scope(env);

  Easy* obj = static_cast<Easy*>(clientp);

  assert(obj);

  int32_t returnValue = 1;

  // See the thread here for explanation on why this flag is needed
  //  https://curl.haxx.se/mail/lib-2014-06/0062.html
  // This was fixed here
  //  https://github.com/curl/curl/commit/907520c4b93616bddea15757bbf0bfb45cde8101
  if (obj->isCbProgressAlreadyAborted) {
    return returnValue;
  }

  CallbacksMap::iterator it = obj->callbacks.find(CURLOPT_PROGRESSFUNCTION);
  assert(it != obj->callbacks.end() && "PROGRESS callback not set.");

  const int argc = 4;
  Napi::Value argv[argc] = {Napi::Number::New(env, static_cast<double>(dltotal)),
                            Napi::Number::New(env, static_cast<double>(dlnow)),
                            Napi::Number::New(env, static_cast<double>(ultotal)),
                            Napi::Number::New(env, static_cast<double>(ulnow))};

  Napi::TryCatch tryCatch;

  Napi::AsyncResource asyncResource("Easy::CbProgress");
  Napi::MaybeLocal<v8::Value> returnValueCallback =
      asyncResource.runInAsyncScope(obj->handle(), it->second->GetFunction(), argc, argv);

  if (tryCatch.HasCaught()) {
    if (obj->isInsideMultiHandle) {
      obj->callbackError.Reset(tryCatch.Exception());
    } else {
      tryCatch.ReThrow();
    }
    return returnValue;
  }

  if (returnValueCallback.IsEmpty() || !returnValueCallback.IsNumber()) {
    Napi::Value typeError =
        Napi::TypeError::New(env, "Return value from the PROGRESS callback must be an integer.");
    if (obj->isInsideMultiHandle) {
      obj->callbackError.Reset(typeError);
    } else {
      throw Napi::Error::New(env, typeError);
    }
  } else {
    returnValue = returnValueCallback.As<Napi::Number>().Int32Value();
  }

  if (returnValue && returnValue != CURL_PROGRESSFUNC_CONTINUE) {
    obj->isCbProgressAlreadyAborted = true;
  }

  return returnValue;
}

int Easy::CbTrailer(struct curl_slist** headerList, void* userdata) {
#if NODE_LIBCURL_VER_GE(7, 64, 0)
  Napi::HandleScope scope(env);

  Easy* obj = static_cast<Easy*>(userdata);

  assert(obj);

  CallbacksMap::iterator it;

  // make sure the callback was set
  it = obj->callbacks.find(CURLOPT_TRAILERFUNCTION);
  assert(it != obj->callbacks.end() && "Trailer callback not set.");

  Napi::TryCatch tryCatch;

  Napi::AsyncResource asyncResource("Easy::CbTrailer");
  Napi::MaybeLocal<v8::Value> returnValueCallback =
      asyncResource.runInAsyncScope(obj->handle(), it->second->GetFunction(), 0, NULL);

  if (tryCatch.HasCaught()) {
    if (obj->isInsideMultiHandle) {
      obj->callbackError.Reset(tryCatch.Exception());
    } else {
      tryCatch.ReThrow();
    }
    return CURL_TRAILERFUNC_ABORT;
  }

  Napi::Value returnValueCbTypeError = Napi::TypeError(
      "Return value from the Trailer callback must be an array of strings or false.");

  bool isInvalid = returnValueCallback.IsEmpty() ||
                   (!returnValueCallback->IsArray() && !returnValueCallback->IsFalse());

  if (isInvalid) {
    if (obj->isInsideMultiHandle) {
      obj->callbackError.Reset(returnValueCbTypeError);
    } else {
      throw Napi::Error::New(env, returnValueCbTypeError);
    }

    return CURL_TRAILERFUNC_ABORT;
  }

  Napi::Value returnValueCallbackChecked = returnValueCallback;

  if (returnValueCallbackChecked.IsFalse()) {
    return CURL_TRAILERFUNC_ABORT;
  }

  Napi::Array rows = returnValueCallbackChecked.As<Napi::Array>();

  // [headerStr1, headerStr2]
  for (uint32_t i = 0, len = rows.Length(); i < len; ++i) {
    // not an array of objects
    Napi::Value headerStrValue = (rows).Get(i);
    if (!headerStrValue.IsString()) {
      if (obj->isInsideMultiHandle) {
        obj->callbackError.Reset(returnValueCbTypeError);
      } else {
        throw Napi::Error::New(env, returnValueCbTypeError);
      }

      return CURL_TRAILERFUNC_ABORT;
    }

    *headerList =
        curl_slist_append(*headerList, headerStrValue.As<Napi::String>().Utf8Value().c_str());
  }

  return CURL_TRAILERFUNC_OK;
#else
  return 0;
#endif
}

int Easy::CbXferinfo(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                     curl_off_t ulnow) {
  Napi::HandleScope scope(env);

  Easy* obj = static_cast<Easy*>(clientp);

  assert(obj);

  int32_t returnValue = 1;

  // same check than above, see it for comments.
  if (obj->isCbProgressAlreadyAborted) {
    return returnValue;
  }

  CallbacksMap::iterator it;

  // make sure the callback was set
#if NODE_LIBCURL_VER_GE(7, 32, 0)
  it = obj->callbacks.find(CURLOPT_XFERINFOFUNCTION);
#else
  // just to make it compile ¯\_(ツ)_/¯
  it = obj->callbacks.end();
#endif
  assert(it != obj->callbacks.end() && "XFERINFO callback not set.");

  const int argc = 4;
  Napi::Value argv[argc] = {Napi::Number::New(env, static_cast<double>(dltotal)),
                            Napi::Number::New(env, static_cast<double>(dlnow)),
                            Napi::Number::New(env, static_cast<double>(ultotal)),
                            Napi::Number::New(env, static_cast<double>(ulnow))};

  Napi::TryCatch tryCatch;

  Napi::AsyncResource asyncResource("Easy::CbXferinfo");
  Napi::MaybeLocal<v8::Value> returnValueCallback =
      asyncResource.runInAsyncScope(obj->handle(), it->second->GetFunction(), argc, argv);

  if (tryCatch.HasCaught()) {
    if (obj->isInsideMultiHandle) {
      obj->callbackError.Reset(tryCatch.Exception());
    } else {
      tryCatch.ReThrow();
    }
    return returnValue;
  }

  if (returnValueCallback.IsEmpty() || !returnValueCallback.IsNumber()) {
    Napi::Value typeError =
        Napi::TypeError::New(env, "Return value from the XFERINFO callback must be an integer.");
    if (obj->isInsideMultiHandle) {
      obj->callbackError.Reset(typeError);
    } else {
      throw Napi::Error::New(env, typeError);
    }
  } else {
    returnValue = returnValueCallback.Int32Value().As<Napi::Number>();
  }

  if (returnValue && returnValue != CURL_PROGRESSFUNC_CONTINUE) {
    obj->isCbProgressAlreadyAborted = true;
  }

  return returnValue;
}

Napi::Object Easy::Initialize(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  Napi::Function tmpl = DefineClass(env, "Easy", {
    InstanceMethod<&Easy::SetOpt>("setOpt"),
    InstanceMethod<&Easy::GetInfo>("getInfo"),
    InstanceMethod<&Easy::Send>("send"),
    InstanceMethod<&Easy::Recv>("recv"),
    InstanceMethod<&Easy::Perform>("perform"),
    InstanceMethod<&Easy::Upkeep>("upkeep"),
    InstanceMethod<&Easy::Pause>("pause"),
    InstanceMethod<&Easy::Reset>("reset"),
    InstanceMethod<&Easy::DupHandle>("dupHandle"),
    InstanceMethod<&Easy::OnSocketEvent>("onSocketEvent"),
    InstanceMethod<&Easy::MonitorSocketEvents>("monitorSocketEvents"),
    InstanceMethod<&Easy::UnmonitorSocketEvents>("unmonitorSocketEvents"),
    InstanceMethod<&Easy::Close>("close"),
    StaticMethod("strError", &Easy::StrError),

    InstanceAccessor("id", &Easy::IdGetter, nullptr),
    InstanceAccessor("isInsideMultiHandle", &Easy::IsInsideMultiHandleGetter, nullptr),
    InstanceAccessor("isMonitoringSockets", &Easy::IsMonitoringSocketsGetter, nullptr),
    InstanceAccessor("isOpen", &Easy::IsOpenGetter, nullptr)
});

    // Store the class constructor in the persistent reference
  Easy::constructor = Napi::Persistent(tmpl);
  Easy::constructor.SuppressDestruct();

  // Set the class on the exports object
  exports.Set("Easy", tmpl);
}

Napi::Value Easy::New(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!info.IsConstructCall()) {
    throw Napi::Error::New(env, "You must use \"new\" to instantiate this object.");
  }

  Napi::Value jsHandle = info[0];
  Easy* obj = nullptr;

  // Copy constructor, used when duplicating handles.
  if (!jsHandle.IsUndefined()) {
    if (!jsHandle.IsExternal() &&
        (!jsHandle.IsObject() || !Napi::Function::New(env, Easy::constructor)->HasInstance(jsHandle))) {
      throw Napi::Error::New(env, "Argument must be an instance of an Easy handle.");
    }

    // This is the case when calling with a curl easy handle directly
    if (jsHandle.IsExternal()) {
      CURL* curlEasyHandle = reinterpret_cast<CURL*>(info[0].As<Napi::External>()->Value());
      obj = new Easy(curlEasyHandle);
    } else {
      Easy* orig = Napi::ObjectWrap<Easy>::Unwrap(info.This().As<Napi::Object>());
      obj = new Easy(orig);
    }

  } else {
    obj = new Easy();
  }

  if (obj) {
    return info.This();
  }
}

Napi::Value Easy::IdGetter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Easy* obj = Napi::ObjectWrap<Easy>::Unwrap(info.This().As<Napi::Object>());

  return Napi::Number::New(env, obj->id);
}

Napi::Value Easy::IsInsideMultiHandleGetter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Easy* obj = Napi::ObjectWrap<Easy>::Unwrap(info.This().As<Napi::Object>());

  return Napi::Boolean::New(env, obj->isInsideMultiHandle);
}

Napi::Value Easy::IsMonitoringSocketsGetter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Easy* obj = Napi::ObjectWrap<Easy>::Unwrap(info.This().As<Napi::Object>());

  return Napi::Boolean::New(env, obj->isMonitoringSockets);
}

Napi::Value Easy::IsOpenGetter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Easy* obj = Napi::ObjectWrap<Easy>::Unwrap(info.This().As<Napi::Object>());

  return Napi::Boolean::New(env, obj->isOpen);
}

Napi::Value Easy::SetOpt(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  Easy* obj = Napi::ObjectWrap<Easy>::Unwrap(info.This().As<Napi::Object>());

  if (!obj->isOpen) {
    throw Napi::Error::New(env, "Curl handle is closed.");
  }

  Napi::Value opt = info[0];
  Napi::Value value = info[1];

  CURLcode setOptRetCode = CURLE_UNKNOWN_OPTION;

  int optionId;

  // See this: https://daniel.haxx.se/blog/2020/08/28/enabling-better-curl-bindings/
  // we probably could use these here for newer libcurl versions...

  if ((optionId = IsInsideCurlConstantStruct(curlOptionNotImplemented, opt))) {
    throw Napi::Error::New(env, "Unsupported option, probably because it's too complex to implement using javascript or unecessary when using javascript (like the _DATA options).");
  } else if ((optionId = IsInsideCurlConstantStruct(curlOptionSpecific, opt))) {
    if (optionId == CURLOPT_SHARE) {
        if (value.IsNull()) {
          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_SHARE, NULL);
        } else {
          if (!value.IsObject() || !Share::constructor.Value().HasInstance(value)) {
            throw Napi::Error::New(env, "Invalid value for the SHARE option. It must be a Share instance.");
          }

          Share* share = Napi::ObjectWrap<Share>::Unwrap(value.As<Napi::Object>());

          if (!share->isOpen) {
            throw Napi::Error::New(env, "Share handle is already closed.");
          }

          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_SHARE, share->sh);
        }
    }
    // linked list options
  } else if ((optionId = IsInsideCurlConstantStruct(curlOptionLinkedList, opt))) {
    if (value.IsNull()) {
      setOptRetCode = curl_easy_setopt(obj->ch, static_cast<CURLoption>(optionId), NULL);

      // HTTPPOST is a special case, since it's an array of objects.
    } else if (optionId == CURLOPT_HTTPPOST) {
      std::string invalidArrayMsg = "HTTPPOST option value should be an Array of Objects.";

      if (!value.IsArray()) {
        throw Napi::TypeError::New(env, invalidArrayMsg.c_str());
      }

      Napi::Array rows = value.As<Napi::Array>();

      std::unique_ptr<CurlHttpPost> httpPost = std::make_unique<CurlHttpPost>();

      // [{ key : val }]
      for (uint32_t i = 0, len = rows.Length(); i < len; ++i) {
        // not an array of objects
        Napi::Value obj = (rows).Get(i);
        if (!obj.IsObject()) {
          throw Napi::TypeError::New(env, invalidArrayMsg.c_str());

        Napi::Object postData = obj.As<Napi::Object>();

        const Napi::Array props = postData.GetPropertyNames();
        const uint32_t postDataLength = props.Length();

        bool hasFile = false;
        bool hasContentType = false;
        bool hasContent = false;
        bool hasName = false;
        bool hasNewFileName = false;

        // loop through the properties names, making sure they are valid.
        for (uint32_t j = 0; j < postDataLength; ++j) {
          int32_t httpPostId = -1;

          const Napi::Value postDataKey = (props).Get(j);
          const Napi::Value postDataValue = (postData).Get(postDataKey);

          // convert postDataKey to httppost id
          std::string fieldName = postDataKey.As<Napi::String>();
          std::string optionName = std::string(*fieldName);
          std::transform(optionName.begin(), optionName.end(), optionName.begin(), ::toupper);

          for (std::vector<CurlConstant>::const_iterator it = curlOptionHttpPost.begin(),
                                                         end = curlOptionHttpPost.end();
               it != end; ++it) {
            if (it->name == optionName) {
              httpPostId = static_cast<int32_t>(it->value);
            }
          }

          switch (httpPostId) {
            case CurlHttpPost::FILE:
              hasFile = true;
              break;
            case CurlHttpPost::TYPE:
              hasContentType = true;
              break;
            case CurlHttpPost::CONTENTS:
              hasContent = true;
              break;
            case CurlHttpPost::NAME:
              hasName = true;
              break;
            case CurlHttpPost::FILENAME:
              hasNewFileName = true;
              break;
            case -1:  // property not found
              std::string errorMsg;

              errorMsg += std::string("Invalid property given: \"") + optionName +
                          "\". Valid properties are file, type, contents, name "
                          "and filename.";
              throw Napi::Error::New(env, errorMsg.c_str());
          }

          // check if value is a string.
          if (!postDataValue.IsString()) {
            std::string errorMsg;

            errorMsg += std::string("Value for property \"") + optionName + "\" must be a string.";
            throw Napi::TypeError::New(env, errorMsg.c_str());
          }
        }

        if (!hasName) {
          throw Napi::Error::New(env, "Missing field \"name\".");
        }

        std::string fieldName = postData.Get("name").As<Napi::String>();
        CURLFORMcode curlFormCode;

        if (hasFile) {
          std::string file = postData.Get("file").As<Napi::String>();

          if (hasContentType) {
            std::string contentType = postData.Get("type").As<Napi::String>();

            if (hasNewFileName) {
              std::string fileName = postData.Get("filename").As<Napi::String>();
              curlFormCode =
                  httpPost->AddFile(*fieldName, fieldName.length(), *file, *contentType, *fileName);
            } else {
              curlFormCode = httpPost->AddFile(*fieldName, fieldName.length(), *file, *contentType);
            }
          } else {
            curlFormCode = httpPost->AddFile(*fieldName, fieldName.length(), *file);
          }

        } else if (hasContent) {  // if file is not set, the contents field MUST
                                  // be set.

          std::string fieldValue = postData.Get("contents").As<Napi::String>();

          curlFormCode =
              httpPost->AddField(*fieldName, fieldName.length(), *fieldValue, fieldValue.length());

        } else {
          throw Napi::Error::New(env, "Missing field \"contents\".");
        }

        if (curlFormCode != CURL_FORMADD_OK) {
          std::string errorMsg;

          errorMsg += std::string("Error while adding field \"") + *fieldName + "\" to post data.";
          throw Napi::Error::New(env, errorMsg.c_str());
        }
      }

      setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_HTTPPOST, httpPost->first);

      if (setOptRetCode == CURLE_OK) {
        obj->toFree->post.push_back(std::move(httpPost));
      }

    } else {
      if (!value.IsArray()) {
        throw Napi::TypeError::New(env, "Option value must be an Array.");
      }

      // convert value to curl linked list (curl_slist)
      curl_slist* slist = NULL;
      Napi::Array array = value.As<Napi::Array>();

      for (uint32_t i = 0, len = array.Length(); i < len; ++i) {
       Napi::String item = array.Get(i).As<Napi::String>();
       std::string utf8String = item.Utf8Value();
       slist = curl_slist_append(slist, utf8String.c_str());
      }

      setOptRetCode = curl_easy_setopt(obj->ch, static_cast<CURLoption>(optionId), slist);

      if (setOptRetCode == CURLE_OK) {
        obj->toFree->slist.push_back(slist);
      }
    }
    // check if option is string, and the value is correct
  } else if ((optionId = IsInsideCurlConstantStruct(curlOptionString, opt))) {
    if (value.IsNull()) {
      setOptRetCode = curl_easy_setopt(obj->ch, static_cast<CURLoption>(optionId), NULL);
    } else {
      if (!value.IsString()) {
        throw Napi::TypeError::New(env, "Option value must be a string.");
      }

      std::string value = info[1].As<Napi::String>();

      size_t length = static_cast<size_t>(value.length());

      std::string valueStr = std::string(value, length);

      // libcurl makes a copy of the strings after version 7.17, CURLOPT_POSTFIELD
      // is the only exception
      if (static_cast<CURLoption>(optionId) == CURLOPT_POSTFIELDS) {
        std::vector<char> valueChar = std::vector<char>(valueStr.begin(), valueStr.end());
        valueChar.push_back(0);

        setOptRetCode = curl_easy_setopt(obj->ch, static_cast<CURLoption>(optionId), &valueChar[0]);

        if (setOptRetCode == CURLE_OK) {
          obj->toFree->str.push_back(std::move(valueChar));
        }

      } else if (static_cast<CURLoption>(optionId) == CURLOPT_URL) {
        obj->urlData = std::vector<char>(valueStr.begin(), valueStr.end());
        obj->urlData.push_back(0);
        setOptRetCode = CURLE_OK;
      } else {
        setOptRetCode =
            curl_easy_setopt(obj->ch, static_cast<CURLoption>(optionId), valueStr.c_str());
      }
    }

    // check if option is an integer, and the value is correct
  } else if ((optionId = IsInsideCurlConstantStruct(curlOptionInteger, opt))) {
    switch (optionId) {
      case CURLOPT_INFILESIZE_LARGE:
      case CURLOPT_MAXFILESIZE_LARGE:
      case CURLOPT_MAX_RECV_SPEED_LARGE:
      case CURLOPT_MAX_SEND_SPEED_LARGE:
      case CURLOPT_POSTFIELDSIZE_LARGE:
      case CURLOPT_RESUME_FROM_LARGE:
        setOptRetCode =
            curl_easy_setopt(obj->ch, static_cast<CURLoption>(optionId),
                             static_cast<curl_off_t>(value.As<Napi::Number>().DoubleValue()));
        break;
      // special case with READDATA, since we need to store the file descriptor
      // and not overwrite the READDATA already set in the handle.
      case CURLOPT_READDATA:
        obj->readDataFileDescriptor = value.As<Napi::Number>().Int32Value();
        setOptRetCode = CURLE_OK;
        break;
      case CURLOPT_PATH_AS_IS:
        obj->pathAsIs = value.As<Napi::Number>().Int32Value();
        setOptRetCode = CURLE_OK;
        break;
      default:
        setOptRetCode = curl_easy_setopt(
            obj->ch, static_cast<CURLoption>(optionId),
            static_cast<long>(value.As<Napi::Number>().Int32Value()));  // NOLINT(runtime/int)
        break;
    }

    // check if option is a function, and the value is correct
  } else if ((optionId = IsInsideCurlConstantStruct(curlOptionFunction, opt))) {
    bool isNull = value.IsNull();

    if (!value.IsFunction() && !isNull) {
      throw Napi::TypeError::New(env, "Option value must be a null or a function.");
    }

    switch (optionId) {
      case CURLOPT_CHUNK_BGN_FUNCTION:

        if (isNull) {
          // only unset the CHUNK_DATA if CURLOPT_CHUNK_END_FUNCTION is not set.
          if (!obj->callbacks.count(CURLOPT_CHUNK_END_FUNCTION)) {
            curl_easy_setopt(obj->ch, CURLOPT_CHUNK_DATA, NULL);
          }

          obj->callbacks.erase(CURLOPT_CHUNK_BGN_FUNCTION);

          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_CHUNK_BGN_FUNCTION, NULL);
        } else {
          obj->callbacks[CURLOPT_CHUNK_BGN_FUNCTION] = std::make_unique<Napi::FunctionReference>(Napi::Persistent(value.As<Napi::Function>()));

          curl_easy_setopt(obj->ch, CURLOPT_CHUNK_DATA, obj);
          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_CHUNK_BGN_FUNCTION, Easy::CbChunkBgn);
        }

        break;

      case CURLOPT_CHUNK_END_FUNCTION:

        if (isNull) {
          // only unset the CHUNK_DATA if CURLOPT_CHUNK_BGN_FUNCTION is not set.
          if (!obj->callbacks.count(CURLOPT_CHUNK_BGN_FUNCTION)) {
            curl_easy_setopt(obj->ch, CURLOPT_CHUNK_DATA, NULL);
          }

          obj->callbacks.erase(CURLOPT_CHUNK_END_FUNCTION);

          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_CHUNK_END_FUNCTION, NULL);
        } else {
          obj->callbacks[CURLOPT_CHUNK_END_FUNCTION] = std::make_unique<Napi::FunctionReference>(Napi::Persistent(value.As<Napi::Function>()));

          curl_easy_setopt(obj->ch, CURLOPT_CHUNK_DATA, obj);
          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_CHUNK_END_FUNCTION, Easy::CbChunkEnd);
        }

        break;

      case CURLOPT_DEBUGFUNCTION:

        if (isNull) {
          obj->callbacks.erase(CURLOPT_DEBUGFUNCTION);

          curl_easy_setopt(obj->ch, CURLOPT_DEBUGDATA, NULL);
          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_DEBUGFUNCTION, NULL);
        } else {
          obj->callbacks[CURLOPT_DEBUGFUNCTION] = std::make_unique<Napi::FunctionReference>(Napi::Persistent(value.As<Napi::Function>()));

          curl_easy_setopt(obj->ch, CURLOPT_DEBUGDATA, obj);
          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_DEBUGFUNCTION, Easy::CbDebug);
        }

        break;

      case CURLOPT_FNMATCH_FUNCTION:

        if (isNull) {
          obj->callbacks.erase(CURLOPT_FNMATCH_FUNCTION);

          curl_easy_setopt(obj->ch, CURLOPT_FNMATCH_DATA, NULL);
          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_FNMATCH_FUNCTION, NULL);
        } else {
          obj->callbacks[CURLOPT_FNMATCH_FUNCTION] = std::make_unique<Napi::FunctionReference>(Napi::Persistent(value.As<Napi::Function>()));

          curl_easy_setopt(obj->ch, CURLOPT_FNMATCH_DATA, obj);
          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_FNMATCH_FUNCTION, Easy::CbFnMatch);
        }
        break;

      case CURLOPT_HEADERFUNCTION:

        setOptRetCode = CURLE_OK;

        if (isNull) {
          obj->callbacks.erase(CURLOPT_HEADERFUNCTION);
        } else {
          obj->callbacks[CURLOPT_HEADERFUNCTION] = std::make_unique<Napi::FunctionReference>(Napi::Persistent(value.As<Napi::Function>()));
        }

        break;

      case CURLOPT_HSTSREADFUNCTION:
        if (isNull) {
          obj->callbacks.erase(CURLOPT_HSTSREADFUNCTION);

          curl_easy_setopt(obj->ch, CURLOPT_HSTSREADDATA, NULL);
          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_HSTSREADFUNCTION, NULL);
        } else {
          obj->callbacks[CURLOPT_HSTSREADFUNCTION] = std::make_unique<Napi::FunctionReference>(Napi::Persistent(value.As<Napi::Function>()));

          curl_easy_setopt(obj->ch, CURLOPT_HSTSREADDATA, obj);
          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_HSTSREADFUNCTION, Easy::CbHstsRead);
        }

        break;
      case CURLOPT_HSTSWRITEFUNCTION:
        if (isNull) {
          obj->callbacks.erase(CURLOPT_HSTSWRITEFUNCTION);

          curl_easy_setopt(obj->ch, CURLOPT_HSTSWRITEDATA, NULL);
          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_HSTSWRITEFUNCTION, NULL);
        } else {
          obj->callbacks[CURLOPT_HSTSWRITEFUNCTION] = std::make_unique<Napi::FunctionReference>(Napi::Persistent(value.As<Napi::Function>()));

          curl_easy_setopt(obj->ch, CURLOPT_HSTSWRITEDATA, obj);
          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_HSTSWRITEFUNCTION, Easy::CbHstsWrite);
        }

        break;

      case CURLOPT_PROGRESSFUNCTION:

        if (isNull) {
          obj->callbacks.erase(CURLOPT_PROGRESSFUNCTION);

          curl_easy_setopt(obj->ch, CURLOPT_PROGRESSDATA, NULL);
          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_PROGRESSFUNCTION, NULL);
        } else {
          obj->callbacks[CURLOPT_PROGRESSFUNCTION] = std::make_unique<Napi::FunctionReference>(Napi::Persistent(value.As<Napi::Function>()));

          curl_easy_setopt(obj->ch, CURLOPT_PROGRESSDATA, obj);
          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_PROGRESSFUNCTION, Easy::CbProgress);
        }

        break;

      case CURLOPT_READFUNCTION:

        setOptRetCode = CURLE_OK;

        if (isNull) {
          obj->callbacks.erase(CURLOPT_READFUNCTION);
        } else {
          obj->callbacks[CURLOPT_READFUNCTION] = std::make_unique<Napi::FunctionReference>(Napi::Persistent(value.As<Napi::Function>()));
        }

        break;

      case CURLOPT_SEEKFUNCTION:

        setOptRetCode = CURLE_OK;

        if (isNull) {
          obj->callbacks.erase(CURLOPT_SEEKFUNCTION);
        } else {
          obj->callbacks[CURLOPT_SEEKFUNCTION] = std::make_unique<Napi::FunctionReference>(Napi::Persistent(value.As<Napi::Function>()));
        }

        break;

      case CURLOPT_TRAILERFUNCTION:

        if (isNull) {
          obj->callbacks.erase(CURLOPT_TRAILERFUNCTION);

          curl_easy_setopt(obj->ch, CURLOPT_TRAILERDATA, NULL);
          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_TRAILERFUNCTION, NULL);
        } else {
          obj->callbacks[CURLOPT_TRAILERFUNCTION] = std::make_unique<Napi::FunctionReference>(Napi::Persistent(value.As<Napi::Function>()));

          curl_easy_setopt(obj->ch, CURLOPT_TRAILERDATA, obj);
          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_TRAILERFUNCTION, Easy::CbTrailer);
        }

        break;

      /* xferinfo was introduced in 7.32.0.
         New libcurls will prefer the new callback and instead use that one even
         if both callbacks are set. */
      case CURLOPT_XFERINFOFUNCTION:

        if (isNull) {
          obj->callbacks.erase(CURLOPT_XFERINFOFUNCTION);

          curl_easy_setopt(obj->ch, CURLOPT_XFERINFODATA, NULL);
          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_XFERINFOFUNCTION, NULL);
        } else {
          obj->callbacks[CURLOPT_XFERINFOFUNCTION] = std::make_unique<Napi::FunctionReference>(Napi::Persistent(value.As<Napi::Function>()));

          curl_easy_setopt(obj->ch, CURLOPT_XFERINFODATA, obj);
          setOptRetCode = curl_easy_setopt(obj->ch, CURLOPT_XFERINFOFUNCTION, Easy::CbXferinfo);
        }

        break;

      case CURLOPT_WRITEFUNCTION:

        setOptRetCode = CURLE_OK;

        if (isNull) {
          obj->callbacks.erase(CURLOPT_WRITEFUNCTION);
        } else {
          obj->callbacks[CURLOPT_WRITEFUNCTION] = std::make_unique<Napi::FunctionReference>(Napi::Persistent(value.As<Napi::Function>()));
        }

        break;
    }

    // check if option is a blob, and the value is correct
  } else if ((optionId = IsInsideCurlConstantStruct(curlOptionBlob, opt))) {
    if (value.IsNull()) {
      setOptRetCode = curl_easy_setopt(obj->ch, static_cast<CURLoption>(optionId), NULL);
    } else if (value.IsString()) {
      std::string utf8StringValue = value.As<Napi::String>();

      size_t length = static_cast<size_t>(utf8StringValue.length());

      struct curl_blob blob;
      blob.data = *utf8StringValue;
      blob.len = length;
      blob.flags = CURL_BLOB_COPY;

      setOptRetCode = curl_easy_setopt(obj->ch, static_cast<CURLoption>(optionId), &blob);
    } else if (value.IsBuffer()) {
      struct curl_blob blob;
      blob.data = value.As<Napi::Buffer<char>>().Data();
      blob.len = value.As<Napi::Buffer<char>>().Length();
      blob.flags = CURL_BLOB_COPY;

      setOptRetCode = curl_easy_setopt(obj->ch, static_cast<CURLoption>(optionId), &blob);
    } else {
      throw Napi::TypeError::New(env, "Option value must be a string or Buffer.");
    }

  }

  return Napi::Number::New(info.Env(), static_cast<int>(setOptRetCode));
}

// traits class to determine if we need to check for null pointer first
template <typename>
struct ResultTypeIsChar : std::false_type {};
template <>
struct ResultTypeIsChar<char*> : std::true_type {};

template <typename TResultType, typename Tv8MappingType>
Napi::Value Easy::GetInfoTmpl(const Easy* obj, int infoId) {
  Napi::EscapableHandleScope scope(env);

  TResultType result;

  CURLINFO info = static_cast<CURLINFO>(infoId);
  CURLcode code = curl_easy_getinfo(obj->ch, info, &result);

  Napi::Value retVal = env.Undefined();

  if (code != CURLE_OK) {
    std::string str = std::to_string(static_cast<int>(code));

    throw Napi::Error::New(env, str.c_str())

  } else {
    // is string
    if (ResultTypeIsChar<TResultType>::value && !result) {
      retVal = Napi::MakeMaybe(Napi::EmptyString());
    } else {
      retVal = Napi::MakeMaybe(Napi::Tv8MappingType::New(env, result));
    }
  }

  return scope.Escape(retVal);
}

Napi::Value Easy::GetInfo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  Easy* obj = Napi::ObjectWrap<Easy>::Unwrap(info.This().As<Napi::Object>());

  if (!obj->isOpen) {
    throw Napi::Error::New(env, "Curl handle is closed.");
  }

  Napi::Value infoVal = info[0];

  Napi::Value retVal = env.Undefined();

  int infoId;

  CURLINFO curlInfo;
  CURLcode code = CURLE_OK;

  // Special case for unsupported info
  if ((infoId = IsInsideCurlConstantStruct(curlInfoNotImplemented, infoVal))) {
    throw Napi::Error::New(env, "Unsupported info, probably because it's too complex to implement using javascript or unecessary when using javascript.");
  }

  Napi::TryCatch tryCatch;

  // String
  if ((infoId = IsInsideCurlConstantStruct(curlInfoString, infoVal))) {
    retVal = Easy::GetInfoTmpl<char*, v8::String>(obj, infoId);
    // curl_off_t
  } else if ((infoId = IsInsideCurlConstantStruct(curlInfoOffT, infoVal))) {
    retVal = Easy::GetInfoTmpl<curl_off_t, v8::Number>(obj, infoId);
    // Double
  } else if ((infoId = IsInsideCurlConstantStruct(curlInfoDouble, infoVal))) {
    retVal = Easy::GetInfoTmpl<double, v8::Number>(obj, infoId);
    // Integer
  } else if ((infoId = IsInsideCurlConstantStruct(curlInfoInteger, infoVal))) {
    retVal = Easy::GetInfoTmpl<long, v8::Number>(obj, infoId);  // NOLINT(runtime/int)
    // ACTIVESOCKET and alike
  } else if ((infoId = IsInsideCurlConstantStruct(curlInfoSocket, infoVal))) {
    curl_socket_t socket;

    code = curl_easy_getinfo(obj->ch, static_cast<CURLINFO>(infoId), &socket);

    if (code == CURLE_OK) {
      // curl_socket_t is of type SOCKET on Windows,
      //  casting it to int32_t can be dangerous, only if Microsoft ever decides
      //  to change the underlying architecture behind it.
      // https://stackoverflow.com/a/26496808/710693
      retVal = Napi::Number::New(env, static_cast<int32_t>(socket));
    }

    // Linked list
  } else if ((infoId = IsInsideCurlConstantStruct(curlInfoLinkedList, infoVal))) {
    curl_slist* linkedList;
    curl_slist* curr;

    curlInfo = static_cast<CURLINFO>(infoId);
    if (curlInfo == CURLINFO_CERTINFO) {
      curl_certinfo* ci = NULL;
      code = curl_easy_getinfo(obj->ch, curlInfo, &ci);

      if (code == CURLE_OK) {
        Napi::Array arr = Napi::Array::New(env);
        for (int i = 0; i < ci->num_of_certs; i++) {
          linkedList = ci->certinfo[i];

          if (linkedList) {
            curr = linkedList;

            while (curr) {
              arr.Set(arr.Length(), Napi::String::New(env, curr->data));
              curr = curr->next;
            }
          }
        }
      }
    } else {
      code = curl_easy_getinfo(obj->ch, curlInfo, &linkedList);

      if (code == CURLE_OK) {
        Napi::Array arr = Napi::Array::New(env);
        if (linkedList) {
          curr = linkedList;

          while (curr) {
            arr.Set(arr.Length(), Napi::String::New(env, curr->data));
            curr = curr->next;
          }

          curl_slist_free_all(linkedList);
        }
      }
    }
  }

  if (tryCatch.HasCaught()) {
    std::string msg = tryCatch.Message(.As<Napi::String>()->Get());

    std::string errCode = std::string(*msg);
    // based on this interesting answer
    // https://stackoverflow.com/a/27538478/710693
    errCode.erase(std::remove_if(errCode.begin(), errCode.end(),
                                 [](unsigned char c) { return !std::isdigit(c); }),
                  errCode.end());

    // 43 is CURLE_BAD_FUNCTION_ARGUMENT
    code = static_cast<CURLcode>(std::stoi(errCode.Length() > 0 ? errCode : "43"));
  }

  Napi::Object ret = Napi::Object::New(env);
  (ret).Set(Napi::String::New(env, "code"), Napi::Number::New(env, static_cast<int32_t>(code)));
  (ret).Set(Napi::String::New(env, "data"), retVal);

  return ret;
}

Napi::Value Easy::Send(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  Easy* obj = Napi::ObjectWrap<Easy>::Unwrap(info.This().As<Napi::Object>());

  if (!obj->isOpen) {
    throw Napi::Error::New(env, "Curl handle is closed.");
  }

  if (info.Length() == 0) {
    throw Napi::Error::New(env, "Missing buffer argument.");
  }

  Napi::Value buf = info[0];

  if (!buf.IsObject() || !buf.IsBuffer()) {
    throw Napi::Error::New(env, "Invalid Buffer instance given.");
  }

  const char* bufContent = buf.As<Napi::Buffer<char>>().Data();
  size_t bufLength = buf.As<Napi::Buffer<char>>().Length();

  size_t n = 0;
  CURLcode curlRet = curl_easy_send(obj->ch, bufContent, bufLength, &n);

  Napi::Object ret = Napi::Object::New(env);
  (ret).Set(Napi::String::New(env, "code"), Napi::Number::New(env, static_cast<int32_t>(curlRet)));
  (ret).Set(Napi::String::New(env, "bytesSent"), Napi::Number::New(env, static_cast<int32_t>(n)));

  return ret;
}

Napi::Value Easy::Recv(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  Easy* obj = Napi::ObjectWrap<Easy>::Unwrap(info.This().As<Napi::Object>());

  if (!obj->isOpen) {
    throw Napi::Error::New(env, "Curl handle is closed.");
  }

  if (info.Length() == 0) {
    throw Napi::Error::New(env, "Missing buffer argument.");
  }

  Napi::Value buf = info[0];

  if (!buf.IsObject() || !buf.IsBuffer()) {
    throw Napi::Error::New(env, "Invalid Buffer instance given.");
  }

  char* bufContent = buf.As<Napi::Buffer<char>>().Data();
  size_t bufLength = buf.As<Napi::Buffer<char>>().Length();

  size_t n = 0;
  CURLcode curlRet = curl_easy_recv(obj->ch, bufContent, bufLength, &n);

  Napi::Object ret = Napi::Object::New(env);
  (ret).Set(Napi::String::New(env, "code"), Napi::Number::New(env, static_cast<int32_t>(curlRet)));
  (ret).Set(Napi::String::New(env, "bytesReceived"), Napi::Number::New(env, static_cast<int32_t>(n)));

  return ret;
}

// exec this handle
Napi::Value Easy::Perform(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  Easy* obj = Napi::ObjectWrap<Easy>::Unwrap(info.This().As<Napi::Object>());

  if (!obj->isOpen) {
    throw Napi::Error::New(env, "Curl handle is closed.")
  }

  if (!obj->SetUrlOpts()) {
    v8::Local<v8::Integer> ret = Napi::Number::New(env, static_cast<int32_t>(CURLE_URL_MALFORMAT));
    return ret;
  }

  SETLOCALE_WRAPPER(CURLcode code = curl_easy_perform(obj->ch););

  v8::Local<v8::Integer> ret = Napi::Number::New(env, static_cast<int32_t>(code));

  return ret;
}

Napi::Value Easy::Upkeep(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  Easy* obj = Napi::ObjectWrap<Easy>::Unwrap(info.This().As<Napi::Object>());

  if (!obj->isOpen) {
    throw Napi::Error::New(env, "Curl handle is closed.")
  }

  CURLcode code = curl_easy_upkeep(obj->ch);

  v8::Local<v8::Integer> ret = Napi::Number::New(env, static_cast<int32_t>(code));

  return ret;
}

Napi::Value Easy::Pause(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  Easy* obj = Napi::ObjectWrap<Easy>::Unwrap(info.This().As<Napi::Object>());

  if (!obj->isOpen) {
    throw Napi::Error::New(env, "Curl handle is closed.")
  }

  if (!info[0].IsNumber()) {
    throw Napi::TypeError::New(env, "Bitmask value must be an integer.")
  }

  uint32_t bitmask = info[0].As<Napi::Number>().Uint32Value();

  CURLcode code = curl_easy_pause(obj->ch, static_cast<int>(bitmask));

  return static_cast<int32_t>(code);
}

Napi::Value Easy::Reset(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  Easy* obj = Napi::ObjectWrap<Easy>::Unwrap(info.This().As<Napi::Object>());

  if (!obj->isOpen) {
    throw Napi::Error::New(env, "Curl handle closed.")
  }

  curl_easy_reset(obj->ch);

  curl_easy_setopt(obj->ch, CURLOPT_CURLU, nullptr);
  curl_url_cleanup(obj->url);
  obj->url = curl_url();
  obj->urlData.clear();
  obj->pathAsIs = false;

  obj->callbacks.clear();
  obj->ResetRequiredHandleOptions();

  obj->toFree = nullptr;
  obj->toFree = std::make_shared<Easy::ToFree>();

  obj->readDataFileDescriptor = -1;
  obj->readDataOffset = -1;

  return info.This();
}

Napi::Value Easy::DupHandle(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  // create a new js object using this one as the argument for the constructor.
  const int argc = 1;
  Napi::Value argv[argc] = {info.This()};
  Napi::Function cons = Napi::GetFunction(Napi::New(env, Easy::constructor));

  Napi::Object newInstance = Napi::NewInstance(cons, argc, argv);

  return newInstance;
}

Napi::Value Easy::OnSocketEvent(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  Easy* obj = Napi::ObjectWrap<Easy>::Unwrap(info.This().As<Napi::Object>());

  if (!info.Length()) {
    throw Napi::Error::New(env, "You must specify the callback function.")
  }

  Napi::Value arg = info[0];

  if (arg.IsNull()) {
    obj->cbOnSocketEvent = nullptr;

    return info.This();
  }

  if (!arg.IsFunction()) {
    Napi::TypeError::New(env, "Invalid callback given.")
    return env.Null();
  }

  Napi::Function callback = arg.As<Napi::Function>();

  obj->cbOnSocketEvent.reset(new Napi::FunctionReference(callback));

  return info.This();
}

Napi::Value Easy::MonitorSocketEvents(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  Easy* obj = Napi::ObjectWrap<Easy>::Unwrap(info.This().As<Napi::Object>());

  try{
    obj->MonitorSockets(env);
  } catch (const std::exception& e) {
    throw Napi::Error::New(env, e.what());
  }

  return info.This();
}

Napi::Value Easy::UnmonitorSocketEvents(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  Easy* obj = Napi::ObjectWrap<Easy>::Unwrap(info.This().As<Napi::Object>());

  try{
    obj->UnmonitorSockets(env);
  } catch (const std::exception& e) {
    throw Napi::Error::New(env, e.what());
  }

  return info.This();
}

Napi::Value Easy::Close(const Napi::CallbackInfo& info) {
  // check https://github.com/php/php-src/blob/master/ext/curl/interface.c#L3196
  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  Easy* obj = Napi::ObjectWrap<Easy>::Unwrap(info.This().As<Napi::Object>());

  if (!obj->isOpen) {
    throw Napi::Error::New(env, "Curl handle already closed.")
  }

  if (obj->isInsideMultiHandle) {
    throw Napi::Error::New(env, "Curl handle is inside a Multi instance, you must remove it first.")
        
  }

  obj->Dispose(env);

  return;
}

Napi::Value Easy::StrError(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  Napi::Value errCode = info[0];

  if (!errCode.IsNumber()) {
    throw Napi::TypeError::New(env, "Invalid errCode passed to Easy.strError.")
  }

  const char* errorMsg =
      curl_easy_strerror(static_cast<CURLcode>(errCode.As<Napi::Number>().Int32Value()));

  Napi::String ret = Napi::String::New(env, errorMsg);

  return ret;
}

}  // namespace NodeLibcurl
