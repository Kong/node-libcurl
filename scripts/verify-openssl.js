#!/usr/bin/env node
// Verifies the OpenSSL version linked into the native binding and exercises the
// TLS paths that changed between OpenSSL 3.0 and 3.5:
//   - TLS 1.2 and TLS 1.3 handshakes against local servers
//   - mutual TLS (client certificate from PEM files)
//   - a 1024-bit RSA server certificate, accepted only at security level 1
// Only public, throwaway certificates are generated, in a temp directory.

'use strict';

const { execFileSync, spawn } = require('child_process');
const fs = require('fs');
const https = require('https');
const os = require('os');
const path = require('path');

const EXPECTED_MAJOR_MINOR = '3.5';
const PORT_BASE = 18440;

const results = [];
const record = (name, ok, detail) => {
  results.push({ name, ok, detail });
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? `  (${detail})` : ''}`);
};
const skip = (name, detail) => {
  results.push({ name, ok: true, skipped: true, detail });
  console.log(`SKIP  ${name}${detail ? `  (${detail})` : ''}`);
};

function loadBinding() {
  const candidates = [
    path.join(__dirname, '..', 'dist', 'index.js'),
    path.join(__dirname, '..', 'build', 'Release', 'node_libcurl.node'),
  ];
  for (const candidate of candidates) {
    try {
      return { mod: require(candidate), source: candidate };
    } catch (err) {
      // try the next candidate
    }
  }
  throw new Error(`native binding not found, tried:\n  ${candidates.join('\n  ')}`);
}

function findOpenssl() {
  const candidates = [
    'openssl',
    'C:\\Program Files\\Git\\usr\\bin\\openssl.exe',
    'C:\\Program Files\\OpenSSL-Win64\\bin\\openssl.exe',
    'C:\\Program Files\\OpenSSL\\bin\\openssl.exe',
  ];
  for (const candidate of candidates) {
    try {
      execFileSync(candidate, ['version'], { stdio: 'ignore' });
      return candidate;
    } catch (err) {
      // keep looking
    }
  }
  return null;
}

function makeCerts(openssl, dir) {
  const run = args => execFileSync(openssl, args, { cwd: dir, stdio: 'pipe' });
  const p = name => path.join(dir, name);
  const san = 'subjectAltName=DNS:localhost,IP:127.0.0.1';

  run(['req', '-x509', '-newkey', 'rsa:2048', '-keyout', 'ca.key', '-out', 'ca.crt', '-days', '1', '-nodes', '-subj', '/CN=verify-ca']);
  run(['req', '-newkey', 'rsa:2048', '-keyout', 'server.key', '-out', 'server.csr', '-nodes', '-subj', '/CN=localhost']);
  fs.writeFileSync(p('server.ext'), `basicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n${san}\n`);
  run(['x509', '-req', '-in', 'server.csr', '-CA', 'ca.crt', '-CAkey', 'ca.key', '-CAcreateserial', '-out', 'server.crt', '-days', '1', '-extfile', 'server.ext']);

  run(['req', '-newkey', 'rsa:2048', '-keyout', 'client.key', '-out', 'client.csr', '-nodes', '-subj', '/CN=verify-client']);
  fs.writeFileSync(p('client.ext'), 'basicConstraints=CA:FALSE\nkeyUsage=digitalSignature\nextendedKeyUsage=clientAuth\n');
  run(['x509', '-req', '-in', 'client.csr', '-CA', 'ca.crt', '-CAkey', 'ca.key', '-CAcreateserial', '-out', 'client.crt', '-days', '1', '-extfile', 'client.ext']);

  run(['req', '-x509', '-newkey', 'rsa:1024', '-keyout', 'weak.key', '-out', 'weak.crt', '-days', '1', '-nodes', '-subj', '/CN=localhost', '-addext', san]);

  return {
    ca: p('ca.crt'),
    serverKey: fs.readFileSync(p('server.key')),
    serverCert: fs.readFileSync(p('server.crt')),
    clientKey: p('client.key'),
    clientCert: p('client.crt'),
    weakKey: p('weak.key'),
    weakCert: p('weak.crt'),
  };
}

function startServer(options) {
  return new Promise(resolve => {
    const server = https.createServer(options, (req, res) => {
      res.writeHead(200, { 'content-type': 'text/plain' });
      res.end('ok');
    });
    server.on('error', () => resolve(null));
    server.listen(0, '127.0.0.1', () => resolve(server));
  });
}

function startWeakServer(openssl, dir) {
  return new Promise(resolve => {
    const port = PORT_BASE + 9;
    const child = spawn(openssl, ['s_server', '-accept', String(port), '-cert', 'weak.crt', '-key', 'weak.key', '-www', '-quiet', '-cipher', 'DEFAULT:@SECLEVEL=0'], { cwd: dir, stdio: 'ignore' });
    const timer = setTimeout(() => resolve({ port, child }), 1500);
    child.on('error', () => {
      clearTimeout(timer);
      resolve(null);
    });
    child.on('exit', () => {
      clearTimeout(timer);
      resolve(null);
    });
  });
}

function curlGet(nativeCurl, url, opts, expectBody = true) {
  return new Promise(resolve => {
    const curl = new nativeCurl();
    let body = '';
    let settled = false;
    const done = (ok, detail) => {
      if (settled) return;
      settled = true;
      try {
        curl.close();
      } catch (err) {
        // already closed
      }
      resolve({ ok, detail });
    };
    curl.setOpt('URL', url);
    curl.setOpt('TIMEOUT', 20);
    curl.setOpt('WRITEFUNCTION', data => {
      body += data.toString();
      return data.length;
    });
    for (const [key, value] of Object.entries(opts || {})) {
      curl.setOpt(key, value);
    }
    curl.on('end', () => done(expectBody ? body.trim() === 'ok' : true, `transferred ${body.length} bytes`));
    curl.on('error', (err, code) => done(false, `code=${code} ${err}`));
    curl.perform();
  });
}

async function main() {
  const { mod, source } = loadBinding();
  const Curl = mod.Curl;
  const info = Curl.getVersionInfoString();
  console.log(`binding: ${source}`);
  console.log(info);
  console.log('');

  const opensslVersion = (info.match(/OpenSSL\/(\d+\.\d+\.\d+)/) || [])[1];
  record('reports an OpenSSL version', Boolean(opensslVersion), opensslVersion || 'not found');
  record(
    `OpenSSL is at least ${EXPECTED_MAJOR_MINOR}`,
    Boolean(opensslVersion) && compareVersions(opensslVersion, EXPECTED_MAJOR_MINOR) >= 0,
    opensslVersion,
  );

  // local servers + certs are only used for the handshake checks below
  const openssl = findOpenssl();
  if (!openssl) {
    skip('TLS handshake checks', 'openssl CLI not available to generate certificates');
    return finish();
  }

  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'verify-openssl-'));
  let certs;
  try {
    certs = makeCerts(openssl, dir);
  } catch (err) {
    skip('TLS handshake checks', `certificate generation failed: ${err.message}`);
    return finish();
  }

  const base = { key: certs.serverKey, cert: certs.serverCert, ca: fs.readFileSync(certs.ca) };
  const curlOpts = { SSL_VERIFYPEER: 1, CAINFO: certs.ca };

  for (const [name, version] of [['TLS 1.2', 'TLSv1.2'], ['TLS 1.3', 'TLSv1.3']]) {
    const server = await startServer({ ...base, minVersion: version, maxVersion: version });
    if (!server) {
      skip(`${name} handshake`, 'local server failed to start');
      continue;
    }
    const port = server.address().port;
    const { ok, detail } = await curlGet(Curl, `https://localhost:${port}/`, curlOpts);
    record(`${name} handshake with CA verification`, ok, detail);
    server.close();
  }

  const mtlsServer = await startServer({ ...base, requestCert: true, rejectUnauthorized: true });
  if (mtlsServer) {
    const port = mtlsServer.address().port;
    const withCert = await curlGet(Curl, `https://localhost:${port}/`, { ...curlOpts, SSLCERT: certs.clientCert, SSLKEY: certs.clientKey });
    record('mutual TLS with client certificate (PEM)', withCert.ok, withCert.detail);
    const withoutCert = await curlGet(Curl, `https://localhost:${port}/`, curlOpts);
    record('mutual TLS without client certificate is rejected', !withoutCert.ok, withoutCert.detail);
    mtlsServer.close();
  } else {
    skip('mutual TLS checks', 'local server failed to start');
  }

  const weak = await startWeakServer(openssl, dir);
  if (weak) {
    const { ok, detail } = await curlGet(Curl, `https://localhost:${weak.port}/`, { SSL_VERIFYPEER: 0 }, false);
    record('1024-bit server certificate accepted (security level 1)', ok, detail);
    weak.child.kill();
  } else {
    skip('1024-bit certificate check', 'openssl s_server failed to start');
  }

  fs.rmSync(dir, { recursive: true, force: true });
  return finish();
}

function compareVersions(a, b) {
  const pa = a.split('.').map(Number);
  const pb = b.split('.').map(Number);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const diff = (pa[i] || 0) - (pb[i] || 0);
    if (diff !== 0) return diff;
  }
  return 0;
}

function finish() {
  const failed = results.filter(r => !r.ok);
  const skipped = results.filter(r => r.skipped);
  console.log('');
  console.log(`${results.length - failed.length - skipped.length} passed, ${failed.length} failed, ${skipped.length} skipped`);
  if (failed.length) {
    console.log(`failed: ${failed.map(r => r.name).join(', ')}`);
    process.exitCode = 1;
  }
}

main().catch(err => {
  console.error(err);
  process.exitCode = 1;
});
