#ifndef NOMINMAX
# define NOMINMAX // To remove conflicts with recent v8 code std::numeric_limits<int>::max()
#endif
/**
 * Copyright (c) Jonathan Cardoso Machado. All Rights Reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "CurlVersionInfo.h"

#include <iostream>

namespace NodeLibcurl {
namespace {
template <typename TValue>
void SetObjPropertyToNullOrValue(Napi::Object obj, std::string key, TValue value) {
  (obj).Set(Napi::New(env, key), Napi::New(env, value));
}

template <>
void SetObjPropertyToNullOrValue<v8::Local<v8::Primitive>>(Napi::Object obj,
                                                           std::string key,
                                                           v8::Local<v8::Primitive> value) {
  (obj).Set(Napi::New(env, key), value);
}

template <>
void SetObjPropertyToNullOrValue<const char*>(Napi::Object obj, std::string key,
                                              const char* value) {
  if (value == nullptr) {
    (obj).Set(Napi::New(env, key), env.Null());
  } else {
    (obj).Set(Napi::New(env, key), Napi::New(env, value));
  }
}
}  // namespace

const std::vector<CurlVersionInfo::feature> CurlVersionInfo::features = {
    {"AsynchDNS", CURL_VERSION_ASYNCHDNS},
    {"Debug", CURL_VERSION_DEBUG},
    {"TrackMemory", CURL_VERSION_CURLDEBUG},
    {"IDN", CURL_VERSION_IDN},
    {"IPv6", CURL_VERSION_IPV6},
    {"Largefile", CURL_VERSION_LARGEFILE},
    {"SSPI", CURL_VERSION_SSPI},
#if NODE_LIBCURL_VER_GE(7, 38, 0)
    {"GSS-API", CURL_VERSION_GSSAPI},
#endif
#if NODE_LIBCURL_VER_GE(7, 40, 0)
    {"Kerberos", CURL_VERSION_KERBEROS5},
#else
    {"Kerberos", CURL_VERSION_KERBEROS4},
#endif
    {"SPNEGO", CURL_VERSION_SPNEGO},
    {"NTLM", CURL_VERSION_NTLM},
    {"NTLM_WB", CURL_VERSION_NTLM_WB},
    {"SSL", CURL_VERSION_SSL},
    {"libz", CURL_VERSION_LIBZ},
#if NODE_LIBCURL_VER_GE(7, 57, 0)
    {"brotli", CURL_VERSION_BROTLI},
#endif
    {"CharConv", CURL_VERSION_CONV},
    {"TLS-SRP", CURL_VERSION_TLSAUTH_SRP},
    {"HTTP2", CURL_VERSION_HTTP2},
#if NODE_LIBCURL_VER_GE(7, 40, 0)
    {"UnixSockets", CURL_VERSION_UNIX_SOCKETS},
#endif
#if NODE_LIBCURL_VER_GE(7, 52, 0)
    {"HTTPS-proxy", CURL_VERSION_HTTPS_PROXY},
#endif
#if NODE_LIBCURL_VER_GE(7, 56, 0)
    {"MultiSSL", CURL_VERSION_MULTI_SSL},
#endif
#if NODE_LIBCURL_VER_GE(7, 47, 0)
    {"PSL", CURL_VERSION_PSL},
#endif
#if NODE_LIBCURL_VER_GE(7, 64, 1)
    {"alt-svc", CURL_VERSION_ALTSVC},
#endif
};

const curl_version_info_data* CurlVersionInfo::versionInfo = curl_version_info(CURLVERSION_NOW);

Napi::Object CurlVersionInfo::Initialize(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  if (!versionInfo) {
    Napi::Error::New(env, "Failed to retrieve libcurl information using curl_version_info").ThrowAsJavaScriptException();
    return env.Null();
  }

  v8::PropertyAttribute attributes =
      static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete);

  Napi::Object obj = Napi::Object::New(env);

  Napi::SetAccessor(obj, Napi::String::New(env, "protocols"), GetterProtocols, 0,
                   Napi::Value(), v8::DEFAULT, attributes);
  Napi::SetAccessor(obj, Napi::String::New(env, "features"), GetterFeatures, 0,
                   Napi::Value(), v8::DEFAULT, attributes);
  SetObjPropertyToNullOrValue(obj, "rawFeatures", versionInfo->features);

  SetObjPropertyToNullOrValue(obj, "version", versionInfo->version);
  SetObjPropertyToNullOrValue(obj, "versionNumber", versionInfo->version_num);

  SetObjPropertyToNullOrValue(obj, "sslVersion", versionInfo->ssl_version);
  SetObjPropertyToNullOrValue(obj, "sslVersionNum", 0);
  SetObjPropertyToNullOrValue(obj, "libzVersion", versionInfo->libz_version);
  SetObjPropertyToNullOrValue(obj, "aresVersion", versionInfo->ares);
  SetObjPropertyToNullOrValue(obj, "aresVersionNumber", versionInfo->ares_num);
  SetObjPropertyToNullOrValue(obj, "libidnVersion", versionInfo->libidn);
  SetObjPropertyToNullOrValue(obj, "iconvVersionNumber", versionInfo->iconv_ver_num);
  SetObjPropertyToNullOrValue(obj, "libsshVersion", versionInfo->libssh_version);
#if NODE_LIBCURL_VER_GE(7, 57, 0)
  SetObjPropertyToNullOrValue(obj, "brotliVersionNumber", versionInfo->brotli_ver_num);
  SetObjPropertyToNullOrValue(obj, "brotliVersion", versionInfo->brotli_version);
#else
  SetObjPropertyToNullOrValue(obj, "brotliVersionNumber", 0);
  SetObjPropertyToNullOrValue(obj, "brotliVersion", env.Null());
#endif

  (target).Set(Napi::String::New(env, "CurlVersionInfo"), obj);
}

Napi::Value CurlVersionInfo::GetterProtocols(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  // const pointer to const char pointer
  const char* const* protocols = versionInfo->protocols;
  unsigned int i = 0;

  std::vector<const char*> vec;

  Napi::Array protocolsResult = Napi::Array::New(env);

  for (i = 0; *(protocols + i); i++) {
    Napi::String protocol = Napi::String::New(env, *(protocols + i));
    (protocolsResult).Set(i, protocol);
  }

  return protocolsResult;
}

// basically a copy of https://github.com/curl/curl/blob/05a131eb7740e/src/tool_help.c#L579
Napi::Value CurlVersionInfo::GetterFeatures(const Napi::CallbackInfo& info) {
  Napi::HandleScope scope(env);

  Napi::Array featuresResult = Napi::Array::New(env);

  unsigned int currentFeature = 0;
  for (auto const& feat : CurlVersionInfo::features) {
    if (versionInfo->features & feat.bitmask) {
      Napi::String featureString = Napi::String::New(env, feat.name);
      (featuresResult).Set(currentFeature++, featureString);
    }
  }

  return featuresResult;
}
}  // namespace NodeLibcurl
