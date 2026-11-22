#!/usr/bin/env python3
"""Inspect bounded batches of Vikingfile RAR and 7z catalogue options.

Uses an existing, matching Vikingfile session from Orbit Zero. Credentials and
cookies stay in Electron. Archive headers are fetched with strictly bounded ranges;
there is no full-download, extraction, login or CAPTCHA fallback.
"""
import argparse
import base64
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import select
import subprocess
import tempfile
import time
from datetime import datetime, timezone
from html.parser import HTMLParser
from urllib.parse import urljoin, urlsplit, unquote

from archive_rar import Held, RangeFile, inspect
from archive_name_hints import enrich_result
from catalog import compile_catalog, viking_page

ROOT = Path(__file__).resolve().parent.parent
def now(): return datetime.now(timezone.utc).isoformat()
def digest(data): return hashlib.sha256(data).hexdigest()
def save(path, data):
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n')
    tmp.replace(path)


NETWORK_RETRY_DELAYS = (2, 5)


def safe_diagnostics(value):
    """Keep transport codes/hostnames, never exception text, URLs or cookies."""
    if not isinstance(value, dict): return {}
    result = {}
    for key in ('name', 'code', 'causeCode'):
        item = value.get(key)
        if isinstance(item, int) and not isinstance(item, bool): result[key] = item
        elif isinstance(item, str) and re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]{0,79}', item): result[key] = item
    for key in ('requestHost', 'redirectHost'):
        item = value.get(key)
        if isinstance(item, str) and re.fullmatch(r'[A-Za-z0-9.-]{1,253}', item): result[key] = item
    return result


class Session:
    def __init__(self, electron, credentials, provider, *, network_retries=2):
        if network_retries not in (0, 1, 2): raise ValueError('Invalid network retry limit')
        self.network_retry_limit = network_retries
        self.metadata_retry_count = 0
        self.transport_failures = []
        self.on_retry = None
        self.temp = tempfile.TemporaryDirectory(prefix='orbit-viking-range-')
        env = dict(os.environ); env.pop('ELECTRON_RUN_AS_NODE', None)
        env['ORBIT_VIKING_INSPECT_DIAGNOSTICS'] = '1'
        self.process = subprocess.Popen([str(electron), str(Path(__file__).with_name('viking_inspect_session.cjs')),
            str(credentials.resolve()), str(provider.resolve()), self.temp.name], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1, env=env)
        try:
            ready = self.receive()
            if ready.get('ready') is not True: raise Held(ready.get('error', 'session_unavailable'))
        except BaseException:
            self.close(); raise
    def receive(self, timeout=45):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            available, _, _ = select.select([self.process.stdout], [], [], max(0, deadline - time.monotonic()))
            if not available: break
            line = self.process.stdout.readline()
            if not line: raise Held('session_closed')
            try: return json.loads(line)
            except ValueError: continue
        raise Held('session_timeout')
    def fetch(self, url, **args):
        for attempt in range(self.network_retry_limit + 1):
            self.process.stdin.write(json.dumps(dict(url=url, **args)) + '\n'); self.process.stdin.flush()
            r = self.receive(timeout=90 if args.get('ranges') else 45)
            if r.get('ok'): return r
            reason = r.get('error', 'session_failed')
            phase = 'range_batch' if args.get('ranges') else 'range' if args.get('range') else 'file_page' if args.get('page') else 'metadata'
            if len(self.transport_failures) < 32:
                self.transport_failures.append(dict(reason=reason, phase=phase,
                    diagnostics=safe_diagnostics(r.get('diagnostics'))))
            # Range retries belong to RetryingRangeFile so every repeated byte
            # and request is charged. Never retry auth, refusal or IPC failures.
            retryable = (args.get('page') or args.get('metadataHash')) and not args.get('range') and not args.get('ranges')
            if not retryable or reason != 'network_request_failed' or attempt == self.network_retry_limit:
                raise Held(reason)
            time.sleep(NETWORK_RETRY_DELAYS[attempt])
            self.metadata_retry_count += 1
            if self.on_retry: self.on_retry(dict(phase='retrying provider request', attempt=attempt + 1))
    def close(self):
        try:
            if self.process.poll() is None: self.process.communicate('{"close":true}\n', timeout=8)
        except (subprocess.TimeoutExpired, BrokenPipeError):
            self.process.kill(); self.process.wait()
        self.temp.cleanup()


class RetryingRangeFile(RangeFile):
    """Retry transient reads without resetting the archive's range budgets."""
    def __init__(self, size, fetch, *, network_retries=2, on_retry=None, **kwargs):
        if network_retries not in (0, 1, 2): raise ValueError('Invalid network retry limit')
        self.network_retry_count = 0
        def retry(call, lengths):
            for attempt in range(network_retries + 1):
                try: return call()
                except Held as error:
                    if str(error) != 'network_request_failed' or attempt == network_retries: raise
                    time.sleep(NETWORK_RETRY_DELAYS[attempt])
                    # read/prefetch already charged the initial attempt. A failed
                    # batch is conservatively charged in full again on retry.
                    self._charge(lengths)
                    self.network_retry_count += 1
                    if on_retry: on_retry(dict(phase='retrying archive headers', attempt=attempt + 1,
                        ranges=len(lengths), bytesReadUpperBound=sum(lengths)))
        def one(start, end, etag):
            return retry(lambda: fetch(start, end, etag), [end-start+1])
        many = kwargs.get('fetch_many')
        if many:
            def batch(ranges, etag):
                return retry(lambda: many(ranges, etag), [end-start+1 for start, end in ranges])
            kwargs['fetch_many'] = batch
        super().__init__(size, one, **kwargs)


class Links(HTMLParser):
    def __init__(self): super().__init__(); self.links = []
    def handle_starttag(self, name, attrs):
        attrs = dict(attrs)
        if name == 'a' and attrs.get('href') and not any(k in attrs for k in ('hidden', 'disabled', 'inert')):
            self.links.append(attrs['href'])


def direct_link(session, row):
    page = row.get('browserUrl') or row['url']
    if not viking_page(page):
        raise Held('unsupported_file_page')
    # Catalogue-supported aliases use the same public file hash. Authenticate
    # only with the main provider host, never forward its cookies to an alias.
    page = 'https://vikingfile.com/f/' + page.rsplit('/', 1)[-1]
    u = urlsplit(page)
    metadata = session.fetch('https://vikingfile.com/api/check-file', metadataHash=u.path.split('/')[-1], cap=32768)
    try: m = json.loads(base64.b64decode(metadata['body']))
    except (ValueError, KeyError): raise Held('invalid_provider_metadata')
    if isinstance(m, list):
        if len(m) != 1: raise Held('ambiguous_provider_metadata')
        m = m[0]
    if not isinstance(m, dict) or m.get('hash', u.path.split('/')[-1]) != u.path.split('/')[-1]:
        raise Held('provider_identity_changed')
    if m.get('exist') is not True:
        raise Held('file_unavailable')
    if m.get('name') != row['filename'] or m.get('size') != row['sizeBytes']:
        raise Held('provider_identity_changed')
    response = session.fetch(page, page=True, cap=1024*1024)
    if response.get('location'): links = [response['location']]
    else:
        parser = Links(); parser.feed(base64.b64decode(response.get('body', '')).decode('utf-8', 'replace'))
        links = [urljoin(page, x) for x in parser.links]
    matches = set()
    for link in links:
        parsed = urlsplit(link)
        if (parsed.scheme == 'https' and parsed.hostname == 'vikingfile.com' and not parsed.username and
            not parsed.password and not parsed.query and not parsed.fragment and
            re.fullmatch(r'/d/[A-Za-z0-9]{10}/[^/]+', parsed.path) and
            unquote(parsed.path.rsplit('/', 1)[-1]) == row['filename']): matches.add(link)
    if len(matches) != 1: raise Held('download_link_not_ready')
    return matches.pop()


def select_rows(games, releases, limit, only=(), skip=(), platform='ps5', sort='release', distinct_games=False, compression='all'):
    by_id = {g['id']: g for g in games}; skip = set(skip); only = set(only)
    if compression not in ('rar', '7z', 'all'): raise ValueError('Unknown archive compression')
    kinds = ('rar', '7z') if compression == 'all' else (compression,)
    rows = [r for r in releases if r.get('sourceId') == 'vikingfile' and r.get('compression') in kinds and
        r.get('archiveContents') == 'unverified' and r['id'] not in skip and
        (not only or r['id'] in only or r['titleId'] in only) and
        (platform == 'all' or by_id[r['gameId']].get('platform', 'ps5') == platform)]
    # Browse's "Newest first" uses releaseDate, not the date added to Orbit.
    # Within a game, prefer the smallest option unless largest-first was requested.
    rows.sort(key=lambda r: (r['sizeBytes'], r['id']))
    if sort == 'release':
        rows.sort(key=lambda r: by_id[r['gameId']]['title'].casefold())
        rows.sort(key=lambda r: by_id[r['gameId']].get('releaseDate') or '', reverse=True)
    elif sort == 'added':
        rows.sort(key=lambda r: by_id[r['gameId']].get('addedAt') or '', reverse=True)
    elif sort == 'size':
        rows.sort(key=lambda r: r['sizeBytes'], reverse=True)
    else:
        raise ValueError('Unknown catalogue sort')
    if distinct_games:
        seen = set(); unique = []
        for row in rows:
            if row['gameId'] not in seen:
                unique.append(row); seen.add(row['gameId'])
        rows = unique
    return rows[:limit]


def inspect_archive(fp, row, *, inspect_packages=False):
    passwords = [row['archivePassword']] if row.get('archivePassword') else []
    if row.get('compression') == '7z':
        from archive_sevenzip import inspect as inspect_7z
        detail = inspect_7z(fp, row['titleId'], passwords)
        if inspect_packages and detail.get('reason') == 'package_type_requires_review' and row['titleId'].startswith('PPSA'):
            from archive_package_probe import inspect_packages as probe
            try:
                detail['packageHeaders'] = probe(fp, row, detail)
                detail.update(format='FPKG', reason=None)
                detail['ranges'] = fp.ranges
            except Held as error:
                detail['packageProbeHeld'] = str(error)
        return detail
    if row.get('compression') != 'rar': raise Held('unsupported_archive_compression')
    detail = inspect(fp, row['titleId'], passwords)
    detail['compression'] = 'rar'
    return detail


def apply_results(games, releases, results):
    updated = copy.deepcopy(releases); by_id = {r['id']: r for r in updated}; applied = []
    for result in results:
        detail = result.get('inspection', {})
        if result.get('status') != 'verified' or not detail.get('complete') or detail.get('reason') or not detail.get('format'):
            continue
        row = by_id.get(result['releaseId'])
        if row is None or row != result['original']: raise Held('catalogue_option_changed')
        row['format'] = detail['format']; row['extractedBytes'] = detail['extractedBytes']
        row.pop('archiveContents', None)
        if detail.get('contentFormat'): row['contentFormat'] = detail['contentFormat']
        if detail.get('assetCompression') == 'ZSTD':
            row['assetCompression'] = 'ZSTD'
            row.pop('contentFormat', None)
        if detail.get('headerPassword'): row['archivePassword'] = detail['headerPassword']
        row.setdefault('importEvidence', {})['archiveInspection'] = {
            'checkedAt': result['checkedAt'],
            'method': f"Complete {row['compression'].upper()} headers via bounded authenticated HTTP ranges",
            'regularFiles': detail['regularFiles'], 'extractedBytes': detail['extractedBytes'],
            'format': detail['format'], 'bytesReadUpperBound': result['bytesReadUpperBound'],
            'rangeRequests': result['rangeRequests'], 'manifestSha256': digest(json.dumps(detail['files'], sort_keys=True).encode()),
            'headerPasswordVerified': bool(detail.get('headerPassword')),
            'payloadPasswordVerified': False, 'payloadExtractionTested': False, 'consoleVerified': False}
        if detail.get('packageHeaders'):
            evidence = row['importEvidence']['archiveInspection']
            evidence['method'] += '; bounded FIH/debug-byte and embedded CNT/content-ID checks of stored package members'
            evidence['packageHeaders'] = detail['packageHeaders']
        applied.append(row['id'])
    compile_catalog(games, updated)
    return updated, applied


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--account-file', type=Path, required=True)
    parser.add_argument('--session-path', type=Path, required=True, help="Orbit Zero's Vikingfile provider partition; sign in and quit Zero first")
    parser.add_argument('--electron', type=Path, required=True)
    parser.add_argument('--catalogue', type=Path, default=ROOT/'catalog')
    parser.add_argument('--output', type=Path, required=True, help='New batch directory; never overwrite an existing report')
    parser.add_argument('--limit', type=int, default=10, help='Maximum links to inspect, 1–500 (default: 10)')
    parser.add_argument('--only', nargs='*', default=[], help='Release IDs or title IDs')
    parser.add_argument('--after-report', action='append', type=Path, default=[])
    parser.add_argument('--platform', choices=['ps5', 'ps4', 'all'], default='ps5')
    parser.add_argument('--sort', choices=['release', 'added', 'size'], default='release', help='release matches Browse newest first; added uses catalogue addition dates; size checks largest archives first')
    parser.add_argument('--distinct-games', action='store_true', help='Inspect one option per game so the batch limit counts games')
    parser.add_argument('--compression', choices=['rar', '7z', 'all'], default='all', help='Inspect RAR, 7z, or both (default)')
    parser.add_argument('--max-mib', type=int, default=8)
    parser.add_argument('--max-requests', type=int, default=384)
    parser.add_argument('--max-seconds', type=int, default=300)
    parser.add_argument('--block-kib', type=int, default=0, help='0 chooses 4 KiB for archives below 256 MiB, otherwise 32 KiB')
    parser.add_argument('--prefetch-headers', action=argparse.BooleanOptionalAction, default=True,
        help='Use Quick Open offsets to prefetch original RAR5 headers in parallel (default); all original headers are still verified')
    parser.add_argument('--network-retries', type=int, choices=(0, 1, 2), default=2,
        help='Retry transient provider/header requests up to twice, within the same archive budgets (default: 2)')
    parser.add_argument('--apply', action='store_true', help='Apply verified format/extracted-size tags locally after compiler validation')
    parser.add_argument('--inspect-packages', action='store_true', help='Check PS5 package headers inside stored 7z members; never decompress full members')
    args = parser.parse_args()
    if not 1 <= args.limit <= 500 or not 1 <= args.max_mib <= 32 or not 0 <= args.block_kib <= 128 or not 1 <= args.max_requests <= 2000 or not 1 <= args.max_seconds <= 3600:
        parser.error('Invalid batch/range budgets')
    if (args.output/'report.json').exists(): parser.error('Output already has a report; choose a new batch directory')
    raw_games = (args.catalogue/'games.json').read_bytes(); raw_releases = (args.catalogue/'releases.json').read_bytes()
    games = json.loads(raw_games); releases = json.loads(raw_releases)
    skip = [r['releaseId'] for p in args.after_report for r in json.loads(p.read_text())['results']]
    rows = select_rows(games, releases, args.limit, args.only, skip, args.platform, args.sort, args.distinct_games, args.compression)
    if not rows: raise Held('no_matching_unverified_archives')
    by_id = {g['id']: g for g in games}
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(startedAt=now(), account='matching_session_required', catalogueSha256=digest(raw_releases),
        selected=[r['id'] for r in rows], sort=args.sort, distinctGames=args.distinct_games, compression=args.compression,
        results=[], applied=[], published=False,
        limits=dict(maxMiB=args.max_mib, maxRequests=args.max_requests, maxSeconds=args.max_seconds,
                    maxFractionOfArchive=0.01, blockKiB=args.block_kib or 'automatic', networkRetries=args.network_retries))
    save(args.output/'report.json', report)
    try:
        session = Session(args.electron, args.account_file, args.session_path)
        session.network_retry_limit = args.network_retries
    except Held as e:
        report.update(stoppedEarly=str(e), finishedAt=now(), summary=dict(attempted=0, verified=0, applied=0, bytesReadUpperBound=0))
        save(args.output/'report.json', report)
        raise
    report['account'] = 'matched_saved_hash'
    try:
        for row in rows:
            result = dict(releaseId=row['id'], game=by_id[row['gameId']]['title'], checkedAt=now(), original=row,
                          status='held', bytesReadUpperBound=0, rangeRequests=0)
            result['rangeBlockKiB'] = args.block_kib or (4 if row['sizeBytes'] < 256*1024*1024 else 32)
            fp = None
            session.metadata_retry_count = 0
            session.transport_failures = []
            def on_retry(event):
                report['active'] = dict(releaseId=row['id'], game=result['game'],
                    rangeRequests=fp.requests if fp else 0, bytesReadUpperBound=fp.bytes if fp else 0,
                    phase=event['phase'], retryAttempt=event['attempt'])
                save(args.output/'report.json', report)
            session.on_retry = on_retry
            report['active'] = dict(releaseId=row['id'], game=result['game'], phase='resolving download link',
                rangeRequests=0, bytesReadUpperBound=0)
            save(args.output/'report.json', report)
            try:
                link = direct_link(session, row)
                def fetch(start, end, etag):
                    nonlocal link
                    time.sleep(0.15)
                    response = session.fetch(link, range=[start, end], size=row['sizeBytes'], cap=end-start+1, etag=etag)
                    link = response.get('finalUrl', link) # Temporary URL stays in memory, never in the report.
                    if fp.requests % 16 == 0:
                        report['active'] = dict(releaseId=row['id'], game=result['game'], rangeRequests=fp.requests, bytesReadUpperBound=fp.bytes)
                        save(args.output/'report.json', report)
                    return base64.b64decode(response['body']), response['headers']
                def fetch_many(ranges, etag):
                    nonlocal link
                    time.sleep(0.15) # Pace batches, not each of their parallel reads.
                    response = session.fetch(link, ranges=ranges, size=row['sizeBytes'],
                        cap=sum(end-start+1 for start,end in ranges), etag=etag)
                    parts = response.get('ranges', [])
                    if parts: link = parts[-1].get('finalUrl',link)
                    report['active'] = dict(releaseId=row['id'], game=result['game'],
                        rangeRequests=fp.requests, bytesReadUpperBound=fp.bytes, phase='prefetching original headers')
                    save(args.output/'report.json', report)
                    return [(base64.b64decode(r['body']),r['headers']) for r in parts]
                fp = RetryingRangeFile(row['sizeBytes'], fetch, max_bytes=min(args.max_mib*1024*1024, row['sizeBytes']//100),
                    max_requests=args.max_requests, max_seconds=args.max_seconds, block=result['rangeBlockKiB']*1024,
                    fetch_many=fetch_many if args.prefetch_headers else None,
                    network_retries=args.network_retries, on_retry=on_retry)
                detail = inspect_archive(fp, row, inspect_packages=args.inspect_packages)
                result.update(inspection=detail, reason=detail['reason'], status='verified' if detail['format'] and not detail['reason'] else 'held')
            except KeyboardInterrupt:
                result['reason'] = 'interrupted'; report['interrupted'] = True
            except Exception as e:
                # Only our fixed errors can reach the report; never arbitrary provider URLs/errors.
                result['reason'] = str(e) if isinstance(e, Held) else 'internal_' + type(e).__name__
            if fp: result.update(bytesReadUpperBound=fp.bytes, rangeRequests=fp.requests)
            result['networkRetries'] = dict(provider=session.metadata_retry_count,
                headers=fp.network_retry_count if fp else 0)
            if session.transport_failures: result['transportFailures'] = session.transport_failures
            result['metadataHints'] = enrich_result(result)
            report.pop('active', None)
            report['results'].append(result); save(args.output/'report.json', report)
            print(json.dumps({k: result[k] for k in ('game', 'status', 'reason', 'bytesReadUpperBound', 'rangeRequests')}, ensure_ascii=False), flush=True)
            if (result.get('reason') or '').startswith(('provider_http_', 'provider_stopped', 'human_verification', 'session_', 'internal_', 'interrupted')):
                report['stoppedEarly'] = result['reason']; break
    finally: session.close()
    if args.apply and not report.get('interrupted'):
        if (args.catalogue/'releases.json').read_bytes() != raw_releases or (args.catalogue/'games.json').read_bytes() != raw_games:
            raise Held('catalogue_changed_during_run')
        updated, applied = apply_results(games, releases, report['results'])
        if applied:
            if (args.catalogue/'releases.json').read_bytes() != raw_releases or (args.catalogue/'games.json').read_bytes() != raw_games:
                raise Held('catalogue_changed_during_validation')
            (args.output/'releases.before.json').write_bytes(raw_releases)
            save(args.catalogue/'releases.json', updated)
        report['applied'] = applied
    report['finishedAt'] = now()
    report['summary'] = dict(attempted=len(report['results']), verified=sum(r['status']=='verified' for r in report['results']),
        applied=len(report['applied']), bytesReadUpperBound=sum(r['bytesReadUpperBound'] for r in report['results']))
    save(args.output/'report.json', report)
    print(json.dumps(report['summary']), flush=True)


if __name__ == '__main__':
    try: main()
    except Held as e: raise SystemExit(f'Inspection held: {e}')
