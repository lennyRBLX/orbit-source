#!/usr/bin/env python3
"""Convert a local Padrino Store checkout to Orbit's bundled community feed."""

import argparse
import gzip
import json
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote, urlsplit, urlunsplit
import zlib


ROOT = Path(__file__).resolve().parents[1]
SNAPSHOT = ROOT / 'catalog/padrino.json.gz'
TITLE_ID = re.compile(r'(CUSA|PPSA)[0-9]{5}\Z')
ZSTD = re.compile(r'(?<![a-z0-9])(?:zstd|zst|zstandard)(?![a-z0-9])', re.I)
FILE = re.compile(r'\.(?:pkg|fpkg|exfat|ffpfsc|zip|7z|rar)\Z', re.I)
VOLUME = re.compile(r'(?:\.part[0-9]+\.rar|\.r[0-9]{2}|\.z[0-9]{2}|\.[0-9]{3})\Z', re.I)
VIKING = {'vikingfile.com', 'vik1ngfile.site', 'vik1ngf1le.online'}


def https(value, limit=2048):
    if not isinstance(value, str) or not value.startswith('https://') or len(value.encode()) >= limit:
        return None
    if re.search(r'[\x00-\x20\\\x7f]', value):
        return None
    try:
        parts = urlsplit(value)
        if parts.hostname and not parts.username and parts.password is None:
            return parts
    except ValueError:
        pass
    return None


def provider_page(parts):
    host, path = parts.hostname, parts.path
    return (
        (not parts.query and (
            (host in VIKING and re.fullmatch(r'/f/[A-Za-z0-9]{10}/?', path))
            or (host in {'akirabox.com', 'akirabox.to'} and re.fullmatch(r'/[A-Za-z0-9]{12}/file/?', path))
            or (host == 'datanodes.to' and re.fullmatch(r'/[A-Za-z0-9]{12}/?', path))
        ))
        or (host == 'archive.org' and path.startswith('/details/'))
    )


def option(row):
    if (row.get('category') not in ('game', None) or row.get('volume') or row.get('unavailableReason')
            or row.get('health') in ('dead', 'broken', 'unavailable')
            or any(ZSTD.search(str(row.get(key, ''))) for key in
                   ('assetCompression', 'compression', 'format', 'source', 'label', 'filename', 'url', 'browserUrl'))):
        return None
    filename = row.get('filename') or ''
    if VOLUME.search(filename):
        return None
    page = False
    for url in (row.get('browserUrl'), row.get('url')):
        parts = https(url)
        if not parts or ZSTD.search(unquote(url)):
            continue
        if provider_page(parts) and len(url.split('#', 1)[0].encode()) < 128:
            if parts.fragment and parts.hostname not in VIKING:
                continue
            url = urlunsplit(parts._replace(fragment='', path=parts.path.rstrip('/') or '/'))
            page = True
            break
        name = unquote(parts.path.rsplit('/', 1)[-1])
        if (not row.get('requiresBrowser') and not parts.fragment and FILE.search(name)
                and not VOLUME.search(name) and not re.search(r'[/\\:\x00-\x1f\x7f]|\.\.', name)
                and len(name.encode()) < 160):
            filename = name
            break
    else:
        return None

    if not FILE.search(filename) or len(filename.encode()) >= 160 or re.search(r'[/\\:\x00-\x1f\x7f]|\.\.', filename):
        filename = ''
    result = {'url': url, 'name': row.get('provider') or 'Padrino Store'}
    if filename:
        result['filename'] = filename
    # Padrino's package labels distinguish PS4 fake packages from official PKGs.
    if re.search(r'\bfpkg\b', row.get('label') or '', re.I):
        result['format'] = 'FPKG'
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
    return result


def convert(checkout):
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
    for game in games:
        tid = game['id']
        if not TITLE_ID.fullmatch(tid) or tid in packages:
            continue
        path = data / 'releases' / f'{tid}.json'
        links = {}
        if path.is_file():
            for row in json.loads(path.read_text()):
                link = option(row)
                if link:
                    links.setdefault(link['url'], link)
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
    args = parser.parse_args()
    if args.checkout:
        convert(args.checkout.resolve())
    else:
        embed(args.embed)
