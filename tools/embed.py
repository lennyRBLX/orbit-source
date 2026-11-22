"""Embed the built UI and curated catalogue; no filesystem web root on PS5."""
import gzip, json, mimetypes, re
from pathlib import Path
from catalog_crypto import unseal
root = Path(__file__).resolve().parent.parent
out = root / 'build/generated'
out.mkdir(parents=True, exist_ok=True)
parts = ['#ifndef ORBIT_ASSETS_H\n#define ORBIT_ASSETS_H\n#include <stddef.h>\n']
rows = []
# Text files are stored gzipped only (issue 185): the main script goes from about
# 570 KB to 170 KB over a phone's Wi-Fi, and keeping one copy holds the payload under
# the 64 MiB that Orbit's updater accepts. api.c unpacks them for the rare client
# that doesn't take gzip.
COMPRESSIBLE = {'.js', '.mjs', '.css', '.html', '.svg', '.json', '.txt'}
# Vite names files under assets/ by their content hash, e.g. index-ihbe_Mwr.js.
HASHED = re.compile(r'^assets/[^/]+-[A-Za-z0-9_-]{8}\.[A-Za-z0-9]+$')
for i, p in enumerate(sorted((root / 'dist').rglob('*'))):
    if not p.is_file(): continue
    data = p.read_bytes()
    mime = mimetypes.guess_type(p.name)[0] or 'application/octet-stream'
    relative = p.relative_to(root/'dist').as_posix()
    packed = gzip.compress(data, compresslevel=9, mtime=0) if p.suffix in COMPRESSIBLE else None
    if packed and len(packed) < len(data) * 0.9:
        parts.append(f'static const unsigned char asset_{i}_gzip[] = {{{",".join(str(b) for b in packed)}}};\n')
        stored = f'NULL,{len(data)},asset_{i}_gzip,sizeof(asset_{i}_gzip)'
    else:
        parts.append(f'static const unsigned char asset_{i}[] = {{{",".join(str(b) for b in data)}}};\n')
        stored = f'asset_{i},sizeof(asset_{i}),NULL,0'
    immutable = 1 if HASHED.match(relative) else 0
    rows.append('{'+json.dumps('/'+relative)+','+json.dumps(mime)+f',{stored},{immutable}'+'}')
parts.append('/* Each file is stored once: plain (gzip NULL), or gzipped only (data NULL, size its\n'
             ' * unpacked length). Immutable files are named by their content hash. */\n')
parts.append('struct orbit_asset { const char *path, *mime; const unsigned char *data; size_t size; const unsigned char *gzip; size_t gzip_size; int immutable; };\n')
parts.append('static const struct orbit_asset assets[] = {'+','.join(rows)+'};\n#endif\n')
(out/'assets.h').write_text(''.join(parts))
catalog = (root/'catalog/catalog.enc').read_bytes()
revision = json.loads((root/'catalog/revision.json').read_text())['revision']
if json.loads(unseal(catalog))['revision'] != revision:
    raise ValueError('Encrypted catalogue revision is stale; run catalog:build')
(out/'catalog.h').write_text(f'static const unsigned catalog_revision = {revision};\n' +
    'static const unsigned char catalog_data[] = {' + ','.join(str(b) for b in catalog) + '};\n')
ca = root/'.deps/cacert.pem'
if ca.exists():
    (out/'ca.h').write_text('static const char orbit_ca[] = '+json.dumps(ca.read_text())+';\n')
print(f'Embedded {len(rows)} UI assets and catalogue')
