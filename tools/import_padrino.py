#!/usr/bin/env python3
"""Convert a local Padrino Store checkout to Orbit's bundled community feed."""

import argparse
from collections import Counter
import gzip
import json
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import unquote, urlsplit, urlunsplit
import zlib


ROOT = Path(__file__).resolve().parents[1]
SNAPSHOT = ROOT / 'catalog/padrino.json.gz'
TITLE_ID = re.compile(r'(CUSA|PPSA)[0-9]{5}\Z')
UNSUPPORTED_COMPRESSION = re.compile(r'(?<![a-z0-9])(?:lz4|zstd|zst|zstandard)(?![a-z0-9])', re.I)
FILE = re.compile(r'\.(?:pkg|fpkg|exfat|ffpfsc|zip|7z|rar)\Z', re.I)
VOLUME = re.compile(r'(?:\.part[0-9]+\.rar|\.r[0-9]{2}|\.z[0-9]{2}|\.[0-9]{3})\Z', re.I)
PART_LABEL = re.compile(r'(?<![a-z0-9])(?:part|volume)[ ._-]*[0-9]+', re.I)
VIKING = {'vikingfile.com', 'vik1ngfile.site', 'vik1ngf1le.online'}


def https(value, limit=2048):
    if not isinstance(value, str) or not value.startswith('https://') or len(value.encode()) >= limit:
        return None
    if re.search(r'[\x00-\x20\\#\x7f]', value):
        return None
    try:
        parts = urlsplit(value)
        if parts.hostname and parts.netloc == parts.hostname and not any(
                segment in ('.', '..') for segment in unquote(parts.path).split('/')):
            return parts
    except ValueError:
        pass
    return None


PAGE_PATHS = {
    'buzzheavier.com': r'/[A-Za-z0-9_-]{6,64}',
    'akirabox.com': r'/[A-Za-z0-9]{12}/file',
    'akirabox.to': r'/[A-Za-z0-9]{12}/file',
    **dict.fromkeys(VIKING, r'/f/[A-Za-z0-9]{10}'),
    'rootz.so': r'/d/[A-Za-z0-9_-]{4,64}',
    'www.rootz.so': r'/d/[A-Za-z0-9_-]{4,64}',
    'rapidgator.net': r'/file/[A-Fa-f0-9]{32}/[^/]+\.html',
    'datanodes.to': r'/[A-Za-z0-9]{12}',
    'datanodes.co': r'/[A-Za-z0-9]{12}',
    'filekeeper.net': r'/(?:[A-Za-z0-9_-]{6,64}|f/[A-Za-z0-9]{32})',
    'datavaults.co': r'/(?:[A-Za-z0-9_-]{6,64}|f/[A-Za-z0-9]{32})',
    'archive.org': r'/details/[A-Za-z0-9_-][A-Za-z0-9_.-]{0,119}',
}


def provider_page(parts):
    host, path = parts.hostname, parts.path
    if '..' in path:
        return False
    if host == '1fichier.com':
        return path == '/' and re.fullmatch(r'[A-Za-z0-9]{20}', parts.query) is not None
    if host == 'pspkg.com':
        game = re.fullmatch(r'/ps[45]/download-[A-Za-z0-9_-][A-Za-z0-9_.-]*-([0-9]{1,12})\.html', path)
        return bool(game and parts.query in ('', f'id={game[1]}'))
    if host == 'rapidgator.net':
        name = unquote(path.rsplit('/', 1)[-1])
        if not name or name.startswith('.') or len(name.encode()) >= 160 or re.search(r'[/\\:?#%\x00-\x1f\x7f]|\.\.', name):
            return False
    pattern = PAGE_PATHS.get(host)
    return bool(pattern and not parts.query and re.fullmatch(pattern, path))


def diagnostic_url(value):
    if not isinstance(value, str):
        return None
    parts = https(value)
    if parts and provider_page(parts):
        return value
    if not parts:
        return '[invalid URL]'
    host = parts.hostname
    return f'https://{host}/[redacted]'


def supported_filename(name):
    return (isinstance(name, str) and FILE.search(name) and len(name.encode()) < 160
            and not re.search(r'[/\\:\x00-\x1f\x7f]|\.\.', name))


def option(row):
    if row.get('category') not in ('game', None):
        return None, 'unsupported_category'
    filename = row.get('filename')
    if (row.get('volume') or row.get('part') or row.get('multipart')
            or any(PART_LABEL.search(str(row.get(key) or '')) for key in ('label', 'edition', 'provider'))
            or (isinstance(filename, str) and (VOLUME.search(filename) or PART_LABEL.search(filename)))):
        return None, 'multipart'
    if row.get('unavailableReason') or row.get('health') in ('dead', 'broken', 'unavailable'):
        return None, 'unavailable'
    if any(UNSUPPORTED_COMPRESSION.search(str(row.get(key, ''))) for key in
           ('assetCompression', 'compression', 'format', 'source', 'label', 'edition', 'filename', 'url', 'browserUrl')):
        return None, 'unsupported_compression'

    requires_browser = row.get('requiresBrowser')
    rejection = None
    page = False
    for url in (row.get('browserUrl'), row.get('url')):
        parts = https(url)
        if not parts:
            continue
        if UNSUPPORTED_COMPRESSION.search(unquote(url)):
            rejection = rejection or 'unsupported_compression'
            continue
        if url.endswith('?') or (parts.hostname == '1fichier.com' and parts.path != '/'):
            continue
        path = parts.path.rstrip('/') or '/'
        name = unquote(path.rsplit('/', 1)[-1])
        candidate = name.removesuffix('.html') if parts.hostname == 'rapidgator.net' else name
        if VOLUME.search(candidate) or PART_LABEL.search(candidate):
            return None, 'multipart'
        if (parts.hostname in {'filekeeper.net', 'datavaults.co', 'datanodes.to'}
                and supported_filename(name) and path.count('/') == 2):
            path = path.rsplit('/', 1)[0]
        canonical = parts._replace(path=path)
        recognized_page = provider_page(canonical)
        if recognized_page and len(urlunsplit(canonical).encode()) < 256:
            url = urlunsplit(canonical)
            page = True
            break
        if not requires_browser and supported_filename(name):
            if parts.query and parts.hostname in PAGE_PATHS:
                continue
            filename = name
            break
        elif not recognized_page and requires_browser:
            rejection = rejection or 'unsupported_provider_page'
        elif not recognized_page and not FILE.search(name):
            rejection = rejection or 'opaque_nonfile_url'
    else:
        return None, rejection or 'invalid_url_or_filename'

    result = {'url': url, 'name': row.get('provider') or 'Padrino Store'}
    for key, cap in (('label', 128), ('edition', 128), ('version', 32), ('region', 32),
                     ('assetCompression', 8), ('compression', 8)):
        value = row.get(key)
        if isinstance(value, str) and value.isprintable() and len(value.encode()) < cap:
            result[key] = value
    source_page = row.get('sourcePage') or row.get('downloadSource')
    if https(source_page, 256) and '?' not in source_page:
        result['sourcePage'] = source_page
    if supported_filename(filename):
        result['filename'] = filename
        if page and isinstance(row.get('id'), str) and filename.rsplit('.', 1)[0].endswith('-' + row['id']):
            result['syntheticFilename'] = True
    # Padrino's package labels distinguish PS4 fake packages from official PKGs.
    if re.search(r'\bfpkg\b', row.get('label') or '', re.I):
        result['format'] = 'FPKG'
    elif re.search(r'\bexfat\b', row.get('label') or '', re.I):
        result['format'] = 'exfat'
    elif row.get('format') in ('pkg', 'zip', 'rar', '7z', 'exfat', 'ffpfsc'):
        result['format'] = row['format']
    if page:
        result['page'] = True
    size = row.get('sizeBytes')
    if type(size) is int and 0 < size <= 2**53 - 1:
        result['sizeEstimateBytes'] = size
    password = row.get('archivePassword')
    if isinstance(password, str) and 0 < len(password.encode()) < 129 and password.isprintable():
        result['archivePassword'] = password
    checksum = row.get('sha256')
    if not page and isinstance(checksum, str) and re.fullmatch(r'[0-9a-fA-F]{64}', checksum):
        result['sha256'] = checksum.lower()
    return result, None


def convert(checkout, show_skips=False):
    data = checkout / 'app/data'
    if subprocess.check_output(['git', 'status', '--porcelain', '--', 'app/data'], cwd=checkout):
        raise ValueError('Commit or clean up Padrino catalog edits before importing')
    # The two arrays are JSON literals inside the JS module. Read the data
    # without evaluating any upstream JavaScript.
    module = (data / 'catalog.js').read_text()
    games = []
    for name in ('games', 'twins'):
        match = re.search(rf'(?m)^\s*var {name}\s*=\s*(?=\[)', module)
        if not match:
            raise ValueError(f'Padrino catalog is missing the {name} array')
        games.extend(json.JSONDecoder().raw_decode(module, match.end())[0])
    packages = {}
    skipped = Counter()
    for game in games:
        tid = game['id']
        if not TITLE_ID.fullmatch(tid) or tid in packages:
            continue
        path = data / 'releases' / f'{tid}.json'
        links = {}
        if path.is_file():
            for row in json.loads(path.read_text()):
                link, reason = option(row)
                if link:
                    links.setdefault((link['url'], link.get('version'), link.get('region'), link.get('edition'), link.get('format'), link.get('compression')), link)
                else:
                    skipped[reason] += 1
                    if show_skips:
                        details = {key: row.get(key) for key in ('id', 'provider', 'label', 'format',
                                                                 'compression', 'assetCompression', 'part', 'volume')}
                        details = {key: value for key, value in details.items() if value is not None}
                        details.update({key: diagnostic_url(row.get(key)) for key in ('url', 'browserUrl')})
                        print(json.dumps({'titleId': tid, 'reason': reason, **details}), file=sys.stderr)
        if not links:
            continue
        title = game['title'].encode()[:127].decode(errors='ignore').strip()
        fields = {'titleId': tid, 'title': title, 'platform': 'ps4' if tid.startswith('CUSA') else 'ps5',
                  'downloadLinks': list(links.values())}
        genres = game.get('genres') or []
        if genres:
            fields['genre'] = genres[0]
        for key in ('publisher', 'releaseDate'):
            if game.get(key):
                fields[key] = game[key]
        for key in ('cover', 'hero'):
            value = (game.get('art') or {}).get(key)
            if https(value) and '#' not in value:
                fields[key] = value
        packages[tid] = fields
    if skipped:
        counts = ', '.join(f'{reason}={count}' for reason, count in sorted(skipped.items()))
        print(f'Skipped {sum(skipped.values())} candidates ({counts})')
    if not packages:
        raise ValueError('No Padrino games had supported download options')
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=checkout, text=True).strip()
    built_at = int(subprocess.check_output(['git', 'show', '-s', '--format=%ct', 'HEAD'], cwd=checkout))
    result = {'schemaVersion': 1, 'commit': commit, 'snapshotAt': built_at,
              'packages': [packages[key] for key in sorted(packages)]}
    options = sum(len(game['downloadLinks']) for game in packages.values())
    print(f'{commit}: {len(packages)} titles, {options} options')
    SNAPSHOT.write_bytes(gzip.compress(json.dumps(result, ensure_ascii=False, separators=(',', ':')).encode(),
                                       mtime=0))


def embed(output):
    body = gzip.decompress(SNAPSHOT.read_bytes())
    snapshot = json.loads(body)
    packed = zlib.compress(body, level=9)
    lines = [','.join(str(value) for value in packed[i:i + 24]) for i in range(0, len(packed), 24)]
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(f'#define PADRINO_JSON_SIZE {len(body)}\n'
                      f'#define PADRINO_SNAPSHOT_AT {snapshot["snapshotAt"]}\n'
                      'static const unsigned char padrino_data[] = {\n' + ',\n'.join(lines) + '\n};\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--checkout', type=Path, help='local checkout at the Padrino revision to import')
    mode.add_argument('--embed', type=Path, help='write the C header from the committed snapshot')
    parser.add_argument('--show-skips', action='store_true', help='write one JSON diagnostic per skipped row to stderr')
    args = parser.parse_args()
    if args.checkout:
        convert(args.checkout.resolve(), show_skips=args.show_skips)
    elif args.show_skips:
        parser.error('--show-skips requires --checkout')
    else:
        embed(args.embed)
