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
  if (content.includes('\n#include "nan_scriptorigin.h"')) {
    fs.writeFileSync(
      nanH,
      content.replace(
        /\n#include "nan_scriptorigin.h"/,
        '\n// #include "nan_scriptorigin.h"',
      ),
      'utf8',
    )
    console.log(
      '[nan-patch] commented out #include "nan_scriptorigin.h" (nodejs/nan#978)',
    )
  }
}
