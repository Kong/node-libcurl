const path = require('path')
const fs = require('fs')

const nanDir = path.join(__dirname, '..', 'node_modules', 'nan')

if (!fs.existsSync(nanDir)) {
  console.log('[nan-patch] nan not found, skipping')
  process.exit(0)
}

// https://github.com/nodejs/nan/issues/978
const nanH = path.join(nanDir, 'nan.h')
if (fs.existsSync(nanH)) {
  const content = fs.readFileSync(nanH, 'utf8')
  if (content.includes('#include "nan_scriptorigin.h"')) {
    fs.writeFileSync(
      nanH,
      content.replace(
        /#include "nan_scriptorigin.h"/,
        '// #include "nan_scriptorigin.h"',
      ),
      'utf8',
    )
    console.log(
      '[nan-patch] commented out #include "nan_scriptorigin.h" (nodejs/nan#978)',
    )
  }
}

// V8 >= 12.5 (Electron 41 / V8 14.6) removed This() and Holder() from
// v8::PropertyCallbackInfo. Use HolderV2() which is available in V8 >= 12.5.
const callbacksInl = path.join(nanDir, 'nan_callbacks_12_inl.h')
if (fs.existsSync(callbacksInl)) {
  const src = fs.readFileSync(callbacksInl, 'utf8')

  const V8_GUARD = [
    '#if defined(V8_MAJOR_VERSION) && \\',
    '    (V8_MAJOR_VERSION > 12 ||    \\',
    '     (V8_MAJOR_VERSION == 12 && defined(V8_MINOR_VERSION) && V8_MINOR_VERSION > 4))',
  ].join('\n')

  const original = [
    '  inline v8::Local<v8::Object> This() const { return info_.This(); }',
    '  inline v8::Local<v8::Object> Holder() const { return info_.Holder(); }',
  ].join('\n')

  const patched = [
    '  inline v8::Local<v8::Object> This() const {',
    V8_GUARD,
    '    return info_.HolderV2();',
    '#else',
    '    return info_.This();',
    '#endif',
    '  }',
    '  inline v8::Local<v8::Object> Holder() const {',
    V8_GUARD,
    '    return info_.HolderV2();',
    '#else',
    '    return info_.Holder();',
    '#endif',
    '  }',
  ].join('\n')

  if (src.includes(patched)) {
    console.log('[nan-patch] PropertyCallbackInfo already patched, skipping')
  } else if (!src.includes(original)) {
    console.warn(
      '[nan-patch] could not find expected code in nan_callbacks_12_inl.h — nan may have been updated',
    )
  } else {
    fs.writeFileSync(callbacksInl, src.replace(original, patched), 'utf8')
    console.log(
      '[nan-patch] patched PropertyCallbackInfo::This() and Holder() for V8 >= 12.5',
    )
  }
}
