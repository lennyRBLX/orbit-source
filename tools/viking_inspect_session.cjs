// Private JSON-lines transport for viking_rar_inspect.py. No UI automation,
// cookie export, credential logging or full-file downloads.
const { app, session } = require('electron');
const fs = require('node:fs');
const readline = require('node:readline');
const path = require('node:path');
const [credentialFile, providerPath, runtimePath] = process.argv.slice(2);
app.setName('Orbit Zero');
app.setPath('userData', runtimePath);
app.commandLine.appendSwitch('disable-http-cache');
const emit = value => process.stdout.write(JSON.stringify(value) + '\n');
const vikingHost = host => host === 'vikingfile.com' || host.endsWith('.vikingfile.com');
// Observed on authenticated Vikingfile download redirects. Keep this to the
// provider's exact bucket; other R2 tenants must not receive range requests.
const vikingR2Host = 'vikingfile.04b3d96d52475741e6b10f97f0a84a16.r2.cloudflarestorage.com';
const allowed = (value, content = false) => {
  try {
    const u = new URL(value);
    return u.protocol === 'https:' && !u.username && !u.password && !u.port &&
      (vikingHost(u.hostname) || (content && (u.hostname === vikingR2Host ||
        /^[-a-z0-9]+\.s3\.(?:bhs|sbg)\.io\.cloud\.ovh\.net$/.test(u.hostname))));
  } catch { return false; }
};
const attribute = (tag, name) => new RegExp(`\\b${name}\\s*=\\s*(["'])(.*?)\\1`, 'i').exec(tag)?.[2] || '';
function matchesAccount(html, expected) {
  const tags = html.match(/<input\b[^>]*>/gi) || [];
  const users = tags.filter(t => attribute(t, 'id') === 'user' && attribute(t, 'name') === 'user' && attribute(t, 'type') === 'hidden');
  const links = (html.match(/<a\b[^>]*>/gi) || []).map(t => attribute(t, 'href'));
  return users.length === 1 && attribute(users[0], 'value') === expected &&
    links.some(h => h === '/logout' || h === 'https://vikingfile.com/logout') &&
    !links.some(h => h === '/login' || h === 'https://vikingfile.com/login');
}
async function boundedBody(response, cap) {
  const reader = response.body?.getReader();
  if (!reader) return Buffer.alloc(0);
  const chunks = []; let count = 0;
  try {
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      count += value.length;
      if (count > cap) throw new Error('response_limit');
      chunks.push(value);
    }
    return Buffer.concat(chunks);
  } finally { await reader.cancel().catch(() => {}); }
}
let provider, stopped = false, transport;
let sessionDiagnostics;
async function fetchWithSession(url, options) {
  // Electron's Chromium fetch can abort a premium file-page redirect as a
  // browser download. Node fetch keeps redirect handling explicit. Session
  // values remain inside this process and are scoped anew for every hop.
  const providerHost = vikingHost(new URL(url).hostname);
  const cookies = providerHost ? await provider.cookies.get({url}) : [];
  const cookie = cookies.filter(c => /^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/.test(c.name) &&
    /^[\x21-\x7e]*$/.test(c.value) && !/[;"\\,]/.test(c.value)).map(c => `${c.name}=${c.value}`).join('; ');
  if (cookie.length > 32768) throw new Error('session_cookie_limit');
  if (sessionDiagnostics) sessionDiagnostics.outgoingCookie ||= Boolean(cookie);
  const r = await fetch(url, {...options, headers:{...options.headers, ...(cookie ? {Cookie:cookie} : {})}});
  try {
    for (const raw of providerHost ? r.headers.getSetCookie() : []) {
      const parts = raw.split(';').map(x => x.trim()); const first = parts.shift(); const eq = first.indexOf('=');
      if (eq < 1) continue;
      const attributes = Object.fromEntries(parts.map(p => {const i=p.indexOf('=');return i<0?[p.toLowerCase(),true]:[p.slice(0,i).toLowerCase(),p.slice(i+1)];}));
      const origin = new URL(url); const domain = attributes.domain;
      if (domain && !(origin.hostname === domain.replace(/^\./,'') || origin.hostname.endsWith('.'+domain.replace(/^\./,'')))) continue;
      const p = typeof attributes.path === 'string' && attributes.path.startsWith('/') ? attributes.path : origin.pathname.slice(0,origin.pathname.lastIndexOf('/')+1);
      const details = {url, name:first.slice(0,eq), value:first.slice(eq+1), path:p,
        secure:Boolean(attributes.secure), httpOnly:Boolean(attributes.httponly),
        ...(domain ? {domain} : {})};
      if (attributes['max-age'] !== undefined && /^-?\d+$/.test(attributes['max-age'])) details.expirationDate=Date.now()/1000+Number(attributes['max-age']);
      else if (attributes.expires && Number.isFinite(Date.parse(attributes.expires))) details.expirationDate=Date.parse(attributes.expires)/1000;
      const sameSite = String(attributes.samesite || '').toLowerCase();
      if (['lax','strict','none'].includes(sameSite)) details.sameSite = sameSite === 'none' ? 'no_restriction' : sameSite;
      await provider.cookies.set(details);
    }
    return r;
  } catch (e) { await r.body?.cancel().catch(()=>{}); throw e; }
}
async function loadVikingCookies(directory, target) {
  // Read only Vikingfile rows. Opening the partition with a different Electron
  // version can return an empty jar; never migrate or write Zero's live store.
  const { DatabaseSync } = require('node:sqlite');
  const cookiePath = [path.join(directory, 'Cookies'), path.join(directory, 'Network', 'Cookies')].find(p => fs.existsSync(p));
  if (!cookiePath) throw new Error('missing_session_cookies');
  const db = new DatabaseSync(cookiePath, {readOnly: true});
  let rows;
  try {
    rows = db.prepare(`SELECT host_key, name, value, path, is_secure, is_httponly,
      CAST(expires_utc AS TEXT) AS expires_utc, has_expires, samesite,
      length(encrypted_value) AS encrypted_length, top_frame_site_key
      FROM cookies WHERE host_key IN ('vikingfile.com', '.vikingfile.com') LIMIT 33`).all();
  } finally { db.close(); }
  if (rows.length > 32) throw new Error('session_cookie_limit');
  for (const c of rows) {
    if (c.encrypted_length && !c.value) throw new Error('saved_session_encrypted');
    if (!c.value || c.top_frame_site_key) continue;
    if (!/^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/.test(c.name) || !/^[\x21-\x7e]*$/.test(c.value) ||
        /[;"\\,]/.test(c.value) || !c.path.startsWith('/') || /[\r\n]/.test(c.path)) throw new Error('invalid_session_cookie');
    const expirationDate = Number(c.expires_utc) / 1e6 - 11644473600;
    if (c.has_expires && expirationDate <= Date.now()/1000) continue;
    await target.cookies.set({url: `https://vikingfile.com${c.path}`, name:c.name, value:c.value,
      path:c.path, secure:Boolean(c.is_secure), httpOnly:Boolean(c.is_httponly),
      ...(c.host_key.startsWith('.') ? {domain:c.host_key} : {}),
      ...(c.has_expires ? {expirationDate} : {}),
      sameSite: ({'-1':'unspecified',0:'no_restriction',1:'lax',2:'strict'})[c.samesite] || 'unspecified'});
  }
}
async function requestOne(q) {
  if (stopped) throw new Error('provider_stopped');
  if (!allowed(q.url, Boolean(q.range))) throw new Error('unapproved_host');
  if (!Number.isSafeInteger(q.cap) || q.cap < 1 || q.cap > 1024 * 1024) throw new Error('invalid_cap');
  let url = q.url;
  for (let hop = 0; hop < 5; hop++) {
    if (!allowed(url, Boolean(q.range))) throw new Error('unapproved_redirect');
    const headers = { 'Accept-Encoding': 'identity' };
    let method = 'GET', body;
    if (q.metadataHash) {
      if (!/^[A-Za-z0-9]{10}$/.test(q.metadataHash) || url !== 'https://vikingfile.com/api/check-file') throw new Error('invalid_metadata_request');
      method = 'POST'; body = new URLSearchParams({ hash: q.metadataHash }).toString();
      headers['Content-Type'] = 'application/x-www-form-urlencoded';
    }
    if (q.range) {
      if (!Array.isArray(q.range) || q.range.length !== 2 || !q.range.every(Number.isSafeInteger) ||
          q.range[0] < 0 || q.range[1] < q.range[0] || q.range[1] - q.range[0] + 1 > q.cap ||
          !Number.isSafeInteger(q.size) || q.range[1] >= q.size || q.range[1] - q.range[0] + 1 >= q.size)
        throw new Error('invalid_range');
      headers.Range = `bytes=${q.range[0]}-${q.range[1]}`;
      if (q.etag && /^"[^\r\n]{1,512}"$/.test(q.etag)) headers['If-Range'] = q.etag;
    }
    let r;
    try {
      r = await transport(url, { method, body, redirect: 'manual', credentials: vikingHost(new URL(url).hostname) ? 'include' : 'omit', cache: 'no-store',
        headers, signal: AbortSignal.timeout(25000) });
    } catch (error) { error.requestHost = new URL(url).hostname; throw error; }
    try {
      if ([401, 403, 429, 503].includes(r.status)) { stopped = true; throw new Error(`provider_http_${r.status}`); }
      if ([301, 302, 303, 307, 308].includes(r.status)) {
        const next = new URL(r.headers.get('location') || '', url).href;
        if (next === url || !allowed(next, Boolean(q.range))) {
          const error = new Error('unapproved_redirect'); error.redirectHost = new URL(next).hostname; throw error;
        }
        if (q.page && new URL(next).pathname.startsWith('/d/')) return { status: r.status, location: next };
        url = next; continue;
      }
      const type = r.headers.get('content-type') || '';
      if (q.range) {
        const expected = `bytes ${q.range[0]}-${q.range[1]}/${q.size}`;
        if (r.status !== 206 || r.headers.get('content-range') !== expected ||
            !['identity', null].includes(r.headers.get('content-encoding')) || /text\/|json|xml/i.test(type) ||
            (r.headers.has('content-length') && Number(r.headers.get('content-length')) !== q.range[1] - q.range[0] + 1))
          throw new Error('exact_range_refused'); // Do not read an ignored range's body.
      } else if (r.status !== 200 || !/text\/html|application\/json/i.test(type)) throw new Error(`page_http_${r.status}`);
      const body = await boundedBody(r, q.cap);
      if (q.range && body.length !== q.range[1] - q.range[0] + 1) throw new Error('short_range');
      if (!q.range && /<title>\s*(?:Just a moment|Attention Required)|cf-chl-|captcha-download|cf-turnstile/i.test(body.toString())) {
        stopped = true; throw new Error('human_verification_required');
      }
      const safeHeaders = {};
      for (const name of ['etag', 'last-modified', 'content-range', 'content-length', 'content-type'])
        if (r.headers.has(name)) safeHeaders[name] = r.headers.get(name);
      return { status: r.status, headers: safeHeaders, body: body.toString('base64'), ...(q.range ? {finalUrl: url} : {}) };
    } finally { if (!r.body?.locked) await r.body?.cancel().catch(() => {}); }
  }
  throw new Error('redirect_limit');
}
async function request(q) {
  if (!q.ranges) return requestOne(q);
  if (stopped) throw new Error('provider_stopped');
  if (!allowed(q.url, true)) throw new Error('unapproved_host');
  if (q.range || q.page || q.metadataHash || !Array.isArray(q.ranges) ||
      q.ranges.length < 1 || q.ranges.length > 8 ||
      !Number.isSafeInteger(q.size) || q.size < 2 ||
      !Number.isSafeInteger(q.cap) || q.cap < 1 || q.cap > 1024 * 1024) throw new Error('invalid_range_batch');
  let total = 0, end = -1;
  for (const r of q.ranges) {
    if (!Array.isArray(r) || r.length !== 2 || !r.every(Number.isSafeInteger) ||
        r[0] <= end || r[1] < r[0] || r[1] >= q.size ||
        r[1] - r[0] + 1 > 65536 || r[1] - r[0] + 1 >= q.size) throw new Error('invalid_range_batch');
    total += r[1] - r[0] + 1; end = r[1];
  }
  if (total > q.cap) throw new Error('invalid_range_batch');
  const results = new Array(q.ranges.length);
  let next = 0, failure;
  async function worker() {
    while (!failure && !stopped && next < q.ranges.length) {
      const i = next++, range = q.ranges[i];
      try {
        results[i] = await requestOne({url:q.url, range, size:q.size,
          cap:range[1]-range[0]+1, etag:q.etag});
      } catch (e) { failure ||= e; }
    }
  }
  // At most four requests in flight; two catalogue workers share eight.
  await Promise.all(Array.from({length:Math.min(4,q.ranges.length)}, worker));
  if (failure) throw failure;
  if (results.some(r => !r) || next !== q.ranges.length) throw new Error('provider_stopped');
  return {ranges:results};
}
async function main() {
  // Prefer IPv4 for the provider's dual-stack storage endpoints: Electron's
  // family racing timed out on these hosts while explicit IPv4 reads passed.
  // This changes connection selection only; normal HTTPS checks remain on.
  if (process.env.ORBIT_VIKING_INSPECT_IPV4 !== '0') {
    require('node:dns').setDefaultResultOrder('ipv4first');
    require('node:net').setDefaultAutoSelectFamily(false);
  }
  if (!credentialFile || !providerPath || !runtimePath || !path.isAbsolute(providerPath) || !fs.existsSync(providerPath))
    throw new Error('missing_session_path');
  const hash = fs.readFileSync(credentialFile, 'utf8').trim();
  if (!/^[A-Za-z0-9_-]{8,256}$/.test(hash)) throw new Error('invalid_account_hash_file');
  await app.whenReady();
  // Cookies are copied only into this process's memory, never to another file.
  provider = session.fromPartition('orbit-viking-range-inspector', { cache: false });
  await loadVikingCookies(providerPath, provider);
  transport = fetchWithSession;
  provider.setPermissionRequestHandler((_wc, _p, callback) => callback(false));
  if (process.env.ORBIT_VIKING_INSPECT_DIAGNOSTICS === '1') {
    const cookies = await provider.cookies.get({url: 'https://vikingfile.com/'});
    sessionDiagnostics = { matchingCookieCount: cookies.length, outgoingCookie: false };
    provider.webRequest.onBeforeSendHeaders({urls: ['https://vikingfile.com/*']}, (details, callback) => {
      sessionDiagnostics.outgoingCookie ||= Object.keys(details.requestHeaders).some(k => k.toLowerCase() === 'cookie' && Boolean(details.requestHeaders[k]));
      callback({requestHeaders: details.requestHeaders});
    });
  }
  const r = await request({ url: 'https://vikingfile.com/', cap: 256 * 1024 });
  const accountPage = Buffer.from(r.body || '', 'base64').toString();
  if (!matchesAccount(accountPage, hash)) {
    const hasLogout = /href=["'](?:https:\/\/vikingfile\.com)?\/logout["']/.test(accountPage);
    throw new Error(hasLogout ? 'different_account_session' : 'sign_in_required');
  }
  emit({ ready: true, account: 'matched_saved_hash' });
  const lines = readline.createInterface({ input: process.stdin });
  for await (const line of lines) {
    try {
      const q = JSON.parse(line);
      if (q.close) break;
      emit({ ok: true, ...await request(q) });
    } catch (e) {
      // Never echo URLs, provider response bodies, hashes, cookies or raw errors.
      const code = /^[a-z_]+(?:_\d+)?$/.test(e.message) ? e.message : 'network_request_failed';
      emit({ ok: false, error: code, ...(process.env.ORBIT_VIKING_INSPECT_DIAGNOSTICS === '1' ? {
        diagnostics: { name: e.name, code: /^[A-Z_0-9]+$/.test(e.code || '') ? e.code : null,
          causeCode: /^[A-Z_0-9]+$/.test(e.cause?.code || '') ? e.cause.code : null,
          requestHost: e.requestHost || null,
          redirectHost: e.redirectHost || null,
          frame: e.stack?.split('\n').find(line => line.includes('viking_inspect_session.cjs:'))?.trim() || null }
      } : {}) });
    }
  }
}
module.exports = { allowed, matchesAccount, loadVikingCookies, fetchWithSession, request, setProviderForTest(value) { provider = value; transport = (...args) => value.fetch(...args); stopped = false; } };
if (process.versions?.electron) main()
  .catch(e => emit({ ready: false, error: /^[a-z_]+$/.test(e.message) ? e.message : 'session_unavailable', ...(sessionDiagnostics ? { diagnostics: sessionDiagnostics } : {}) }))
  .finally(() => app.exit());
