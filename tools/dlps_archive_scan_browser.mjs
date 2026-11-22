/**
 * Browser transport for dlps_archive_scan.py. Use an already selected, ordinary
 * in-app browser tab through the documented cua browser API. No CDP attachment,
 * raw HTTP fallback, cookie export, CAPTCHA interaction or provider downloads.
 *
 * In cua_repl:
 *   const module = await import('file:///.../tools/dlps_archive_scan_browser.mjs');
 *   const run = await module.createBrowserScan(tab, {
 *     startId: 51750, endId: 51741,
 *     output: '/.../.state/dlps-archive-discovery/run/browser-captures.json'
 *   });
 *   await run.runBatch(5);
 *
 * Repeat bounded batches, then parse with the Python CLI's --browser-captures.
 */
import { readFile, writeFile, mkdir, rename } from 'node:fs/promises';
import { dirname, join, resolve, sep } from 'node:path';
import { createHash } from 'node:crypto';
import { setTimeout as delay } from 'node:timers/promises';

const BASE = 'https://downloadgameps3.net';
const LIMIT = 2_000_000;
const success = new Set(['captured', 'missing']);

export async function createBrowserScanFromPlan(tab, planPath, options) {
  const plan = JSON.parse(await readFile(planPath, 'utf8'));
  return createBrowserScan(tab, { ...options, archiveIds: plan.archiveIds });
}

export async function createBrowserScan(tab, options) {
  const { startId, endId } = options;
  const intervalMs = options.intervalMs ?? 3_000;
  const explicit = options.archiveIds;
  const validId = x => Number.isSafeInteger(x) && x > 0;
  if ((explicit ? !Array.isArray(explicit) || !explicit.length || explicit.length > 200 ||
       !explicit.every(validId) || new Set(explicit).size !== explicit.length :
       ![startId, endId].every(validId) || Math.abs(startId - endId) >= 200) || intervalMs < 3_000) {
    throw new Error('Use positive IDs, at most 200 pages and at least 3000 ms between pages');
  }
  const output = resolve(options.output);
  if (!output.endsWith('.json') || output.split(sep).includes('catalog')) {
    throw new Error('Use a separate research JSON file outside catalog/');
  }
  const scope = explicit ? { baseUrl: BASE, archiveIds: [...explicit] } : { baseUrl: BASE, startId, endId };
  let report;
  try {
    report = JSON.parse(await readFile(output, 'utf8'));
    if (JSON.stringify(report.scope) !== JSON.stringify(scope)) throw new Error('Resume scope differs');
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
    report = { schemaVersion: 1, transport: 'normal_in_app_browser',
      scope, startedAt: new Date().toISOString(), results: [], imported: false, published: false };
  }
  if (options.resumeBrowserError && report.status === 'stopped' && report.results.at(-1)?.status === 'browser_error') {
    report.status = 'ready';
  }
  const direction = endId < startId ? -1 : 1;
  const ids = explicit ? [...explicit] : Array.from({ length: Math.abs(startId - endId) + 1 }, (_, i) => startId + i * direction);
  let lastNavigation = 0;
  async function save() {
    await mkdir(dirname(output), { recursive: true });
    await writeFile(output + '.tmp', JSON.stringify(report, null, 2) + '\n');
    await rename(output + '.tmp', output);
  }
  async function observe(id) {
    return tab.playwright.evaluate(id => {
      const text = document.body?.innerText?.slice(0, 3_000) ?? '';
      const title = document.title;
      const challenge = /just a moment|attention required/i.test(title) ||
        /performing security verification|verifying you are human|verify you are human/i.test(text);
      const article = document.getElementById('post-' + id);
      const content = article || document.getElementById('content') || document.querySelector('main');
      return { url: location.href, title, challenge,
        missing: !article && /page not found|nothing found|\b404\b/i.test(title),
        html: challenge ? '' : (content?.outerHTML ?? ''),
        anchorCount: content?.querySelectorAll('a').length ?? 0 };
    }, id);
  }
  async function capture(id) {
    const sourceUrl = `${BASE}/archives/${id}`;
    const row = { archiveId: id, sourceUrl, checkedAt: new Date().toISOString(),
      transport: 'normal_in_app_browser' };
    try {
      if (await tab.url() !== sourceUrl) {
        await delay(Math.max(0, lastNavigation + intervalMs - Date.now()));
        lastNavigation = Date.now();
        await tab.goto(sourceUrl);
      }
      await tab.playwright.domSnapshot();
      let page = await observe(id);
      if (page.challenge) {
        // Wait for the site's observed header after automatic verification.
        // Never interact with the security check or solve a CAPTCHA.
        try {
          await tab.playwright.getByRole('link', { name: 'LINK DOWNLOAD FREE', exact: true })
            .waitFor({ state: 'visible', timeoutMs: 15_000 });
        } catch { /* An unresolved check stays held. */ }
        await tab.playwright.domSnapshot();
        page = await observe(id);
      }
      if (page.challenge) return { ...row, status: 'browser_verification_required', stop: true };
      const final = new URL(page.url);
      if (final.origin !== BASE || final.pathname !== `/archives/${id}`) {
        return { ...row, status: 'unexpected_navigation', stop: true };
      }
      if (page.missing) return { ...row, documentTitle: page.title, status: 'missing', httpStatus: null };
      if (!page.html || Buffer.byteLength(page.html, 'utf8') > LIMIT) {
        return { ...row, status: 'missing_or_oversized_page_content', stop: true };
      }
      const htmlPath = join(dirname(output), 'pages', `${id}.html`);
      await mkdir(dirname(htmlPath), { recursive: true });
      const escapedTitle = page.title.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;');
      const html = `<title>${escapedTitle}</title>\n${page.html}`;
      await writeFile(htmlPath, html);
      return { ...row, status: 'captured', documentTitle: page.title, htmlPath,
        sha256: createHash('sha256').update(html).digest('hex'),
        bytes: Buffer.byteLength(html), anchorCount: page.anchorCount,
        httpStatus: null, cookiesExported: false };
    } catch (error) {
      return { ...row, status: 'browser_error', errorType: error.name, stop: true };
    }
  }
  const progress = () => ({ status: report.status, total: ids.length,
    checked: report.results.filter(r => success.has(r.status)).length,
    captured: report.results.filter(r => r.status === 'captured').length,
    missing: report.results.filter(r => r.status === 'missing').length,
    last: report.results.at(-1)?.archiveId, output });
  if (options.resumeAfterAutomaticVerification && report.status === 'stopped' &&
      report.results.at(-1)?.status === 'browser_verification_required') {
    const last = report.results.at(-1);
    await tab.playwright.domSnapshot();
    const page = await observe(last.archiveId);
    if (!page.challenge && page.url === last.sourceUrl) report.status = 'ready';
  }
  return {
    progress,
    async runBatch(count = 5) {
      if (!Number.isInteger(count) || count < 1 || count > 10) throw new Error('Batch size must be 1–10');
      if (report.status === 'stopped') return { ...progress(), stop: report.results.at(-1) };
      report.status = 'running';
      await save();
      const done = new Set(report.results.filter(r => success.has(r.status)).map(r => r.archiveId));
      const current = [];
      for (const id of ids.filter(id => !done.has(id)).slice(0, count)) {
        const row = await capture(id);
        report.results = report.results.filter(r => r.archiveId !== id).concat(row);
        report.updatedAt = new Date().toISOString();
        current.push({ archiveId: id, status: row.status, anchorCount: row.anchorCount });
        if (row.stop) report.status = 'stopped';
        await save();
        if (row.stop) break;
      }
      if (report.status !== 'stopped' && report.results.filter(r => success.has(r.status)).length === ids.length) {
        report.status = 'complete';
        report.finishedAt = new Date().toISOString();
        await save();
      }
      return { ...progress(), current };
    },
  };
}
