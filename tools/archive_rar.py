"""Header-only RAR inspection. Never extract or execute an archive entry."""
import hashlib
import io
import re
import time
import unicodedata
import warnings
import rarfile

rarfile.RAR_MAX_COMMENT = 0
PASSWORDS = ['www.DLPSGAME.COM', 'DLPSGAME.COM', '[DLPSGAME.COM]', 'pippo', 'SuperPSX', 'downloadgameps3.com', 'hako']
class Held(Exception): pass
class BudgetExceeded(Held): pass

class RangeFile(io.RawIOBase):
    """Seekable archive view. Small cached reads; never fill skipped file bodies."""
    def __init__(self, size, fetch, *, max_bytes=8*1024*1024, max_requests=96, max_seconds=180, block=4096, fetch_many=None):
        self.size = size; self.fetch = fetch; self.pos = 0; self.cache = {}; self.bytes = 0; self.requests = 0
        self.max_bytes = max_bytes; self.max_requests = max_requests; self.deadline = time.monotonic() + max_seconds
        self.block = block; self.etag = None; self.last_modified = None; self.ranges = []
        self.fetch_many = fetch_many; self.quick_open = None
    def _charge(self, lengths):
        if time.monotonic() > self.deadline: raise BudgetExceeded('Archive inspection time budget reached')
        if self.requests + len(lengths) > self.max_requests or self.bytes + sum(lengths) > self.max_bytes:
            raise BudgetExceeded('Archive header request/byte budget reached')
        self.requests += len(lengths); self.bytes += sum(lengths)
    def _accept(self, start, length, data, headers):
        if len(data) != length: raise Held('Incomplete range read')
        etag = headers.get('etag'); modified = headers.get('last-modified')
        if (self.etag and etag != self.etag) or (self.last_modified and modified != self.last_modified):
            raise Held('Remote file changed during inspection')
        if etag and not etag.startswith('W/'): self.etag = etag
        if modified: self.last_modified = modified
        self.cache[start] = data
        self.ranges.append({'start':start, 'end':start+length-1, 'sha256':hashlib.sha256(data).hexdigest()})
    def prefetch(self, intervals, batch_size=8):
        """Cache original archive bytes, never the index's copies of headers."""
        if self.fetch_many is None: return 0
        pending = []
        for start, end in intervals:
            if not (0 <= start <= end < self.size) or end-start+1 > 65536: raise Held('invalid_prefetch_range')
            if any(o <= start and end < o+len(b) for o,b in self.cache.items()): continue
            pending.append((start,end))
        fetched = 0
        for pos in range(0, len(pending), batch_size):
            batch = pending[pos:pos+batch_size]
            self._charge([end-start+1 for start,end in batch])
            results = self.fetch_many(batch, self.etag)
            if len(results) != len(batch): raise Held('Incomplete range batch')
            for (start,end), (data,headers) in zip(batch, results):
                self._accept(start,end-start+1,data,headers)
            fetched += len(batch)
        return fetched
    def readable(self): return True
    def seekable(self): return True
    def tell(self): return self.pos
    def seek(self, offset, whence=0):
        p = offset if whence == 0 else self.pos + offset if whence == 1 else self.size + offset if whence == 2 else -1
        if p < 0: raise ValueError('Invalid seek')
        self.pos = p; return p
    def read(self, n=-1):
        if n is None or n < 0: n = self.size - self.pos
        n = max(0, min(n, self.size - self.pos))
        if n > self.max_bytes: raise BudgetExceeded('Parser requested an oversized read')
        output = bytearray()
        while n:
            if time.monotonic() > self.deadline: raise BudgetExceeded('Archive inspection time budget reached')
            cached = next(((o, b) for o, b in self.cache.items() if o <= self.pos < o+len(b)), None)
            if cached:
                o, b = cached; take = min(n, o+len(b)-self.pos)
                output.extend(b[self.pos-o:self.pos-o+take]); self.pos += take; n -= take; continue
            start = self.pos
            length = min(self.size-start, max(self.block, n))
            # Charge attempted reads as well as successful ones. A timeout must
            # never reset budgets or trigger a full-file fallback.
            self._charge([length])
            data, headers = self.fetch(start, start+length-1, self.etag)
            self._accept(start,length,data,headers)
        return bytes(output)
    def readinto(self, b):
        data = self.read(len(b)); b[:len(data)] = data; return len(data)


def classify(entries, title_id, *, max_entries=250000):
    seen = set(); files = []
    if type(max_entries) is not int or not 1 <= max_entries <= 500000:
        raise ValueError('Invalid archive entry limit')
    if len(entries) > max_entries: raise Held('too_many_entries')
    for entry in entries:
        p = entry['path'].replace('\\', '/')
        if (not p or p.startswith('/') or re.match(r'^[A-Za-z]:', p) or
                any(ord(c) < 32 for c in p) or any(s in ('.', '..') for s in p.split('/'))):
            raise Held('unsafe_path')
        key = unicodedata.normalize('NFC', p.rstrip('/')).casefold()
        if key in seen: raise Held('duplicate_path')
        seen.add(key)
        if entry.get('special'): raise Held('link_or_special_entry')
        if entry.get('directory'): continue
        if type(entry.get('sizeBytes')) is not int or entry['sizeBytes'] < 0: raise Held('unknown_extracted_size')
        files.append({**entry, 'path': p})
    # Match the released extractor: macOS archive metadata is never published
    # or counted as game data, but still receives the path/link checks above.
    clutter = {f['path'] for f in files if '__MACOSX' in f['path'].split('/') or
               f['path'].rsplit('/', 1)[-1] == '.DS_Store' or
               f['path'].rsplit('/', 1)[-1].startswith('._')}
    total = sum(f['sizeBytes'] for f in files if f['path'] not in clutter)
    if not files or not 0 < total <= 9007199254740991: raise Held('invalid_extracted_size')
    paths = {f['path'] for f in files}
    notes = {f['path'] for f in files if f['sizeBytes'] <= 65536 and
        re.fullmatch(r'(?:read[ _-]?me|notes?|password|info|instructions)(?:[ ._-][^/]*)?\.(?:txt|nfo)',
                     f['path'].rsplit('/', 1)[-1], re.I)}
    content_paths = paths - notes - clutter
    def matches_title(path): return title_id.upper() in re.findall(r'(?:PPSA|CUSA)\d{5}(?!\d)', path.upper())
    packages = [p for p in paths if p.lower().endswith(('.pkg', '.fpkg', '.ffpkg'))]
    images = [p for p in paths if p.lower().endswith(('.exfat', '.ffpfsc'))]
    reason = 'layout_requires_review'; fmt = None; subtype = None
    companion_roots = set()
    if len(images) == 1 and content_paths == set(images):
        if matches_title(images[0]):
            fmt = 'exFAT' if images[0].lower().endswith('.exfat') else 'FFPFSC'; reason = None
        else: reason = 'title_identity_requires_review'
    elif not images and title_id.startswith('PPSA'):
        roots = [p[:-len('eboot.bin')] for p in paths if p.endswith('/eboot.bin') or p == 'eboot.bin']
        roots = [root for root in roots if root + 'sce_sys/param.json' in paths]
        fakelib_roots = {p.split('/', 1)[0] + '/' for p in paths
                         if re.fullmatch(r'[^/]+/fakelib/[A-Za-z0-9_.-]+\.sprx', p)}
        def known_companion(path, root):
            label, separator, relative = path.partition('/')
            # A separately bundled backport is still a game-folder download.
            # Identify only a narrow, complete companion layout; filename
            # claims never establish firmware compatibility or installability.
            if (not separator or not re.fullmatch(r'backports?(?:[ _(-][^/]*)?', label, re.I) or
                    re.search(r'(?:PPSA|CUSA)\d{5}', label, re.I)):
                return False
            prefix = label + '/'
            if prefix + 'eboot.bin' not in paths or prefix not in fakelib_roots:
                return False
            allowed = (relative.lower().endswith(('.bin', '.prx', '.sprx', '.index')) and
                       (root + relative in paths or relative == 'ampr_emu.index' or
                        re.fullmatch(r'fakelib/[A-Za-z0-9_.-]+\.sprx', relative)))
            if allowed: companion_roots.add(prefix)
            return bool(allowed)
        if (len(roots) == 1 and matches_title(roots[0]) and
                all(p.startswith(roots[0]) or known_companion(p, roots[0]) for p in content_paths)):
            fmt = 'Game folder'; reason = None
            if any(p.lower().endswith('.lz4') for p in paths): subtype = 'LZ4'
    if packages and fmt is None:
        # Packages within an identified game's root can be assets or bundled
        # DLC. They do not turn the entire game folder into a package download.
        # A standalone .pkg filename still does not establish PS5 FPKG type.
        reason = 'package_type_requires_review'
    result = dict(format=fmt, contentFormat=subtype, reason=reason, regularFiles=len(files),
                  extractedBytes=total, files=files, complete=True)
    if fmt == 'Game folder' and roots[0] + 'ampr_emu.index' in paths and any(
            re.fullmatch(re.escape(roots[0]) + r'ampr_zstd(?:-\d+)?\.pak', p, re.I) for p in paths):
        result['assetCompression'] = 'ZSTD'
        result['contentFormat'] = None
    if clutter: result['ignoredMetadataFiles'] = len(clutter)
    if fmt == 'Game folder' and companion_roots:
        result.update(companionRoots=sorted(companion_roots), companionCompatibilityVerified=False)
    return result


def inspect(fp, title_id, passwords=()):
    fp.seek(0)
    if fp.read(8)[:7] not in (b'Rar!\x1a\x07\x00', b'Rar!\x1a\x07\x01'): raise Held('not_rar')
    if fp.fetch_many is not None:
        from archive_rar_quickopen import prefetch_headers
        fp.quick_open = prefetch_headers(fp, list(dict.fromkeys([None, *passwords, *PASSWORDS])))
    observed = []; selected = None; encrypted = False
    for password in dict.fromkeys([None, *passwords, *PASSWORDS]):
        fp.seek(0); observed = []
        def on_header(h):
            if h.type == rarfile.RAR_BLOCK_MAIN and h.flags & rarfile.RAR_MAIN_VOLUME:
                raise Held('multipart_rar')
            if h.type == rarfile.RAR_BLOCK_FILE and h.flags & (rarfile.RAR_FILE_SPLIT_BEFORE | rarfile.RAR_FILE_SPLIT_AFTER):
                raise Held('split_entry')
            observed.append(h.type)
            if len(observed) > 100005: raise Held('too_many_entries')
        try:
            with warnings.catch_warnings():
                warnings.simplefilter('error', rarfile.UnsupportedWarning)
                with rarfile.RarFile(fp, errors='strict', info_callback=on_header) as archive:
                    if archive.needs_password() and password is not None: archive.setpassword(password)
                    if rarfile.RAR_BLOCK_ENDARC not in observed:
                        if archive.needs_password() and password is None: raise rarfile.PasswordRequired()
                        raise Held('end_marker_missing')
                    encrypted = archive.needs_password()
                    entries = [dict(path=i.filename, sizeBytes=i.file_size, directory=i.isdir(),
                        special=bool(i.is_symlink() or getattr(i, 'file_redir', None)), encrypted=i.needs_password(),
                        crc32=i.CRC) for i in archive.infolist()]
            selected = password
            break
        except (rarfile.PasswordRequired, rarfile.RarWrongPassword): continue
        except rarfile.BadRarFile as e:
            if password is not None: continue
            raise Held('invalid_rar_headers') from e
    else: raise Held('header_password_required')
    result = classify(entries, title_id)
    result.update(headerPassword=selected if encrypted else None, encrypted=encrypted,
                  payloadExtractionTested=False, payloadPasswordVerified=False,
                  consoleVerified=False, ranges=fp.ranges)
    if fp.quick_open is not None: result['quickOpenPrefetch'] = fp.quick_open
    return result
