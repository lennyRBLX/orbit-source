"""Use RAR5 Quick Open offsets as untrusted prefetch hints.

The regular rarfile parser still walks every original header and checks its CRC,
password, paths and end marker. Index copies never enter the parser's cache.
"""
import struct
import zlib
import rarfile

class NoIndex(Exception): pass


def vint(data, pos):
    value = 0
    for i in range(10):
        if pos >= len(data): raise NoIndex('truncated_index')
        byte = data[pos]; pos += 1
        value |= (byte & 127) << (7*i)
        if not byte & 128:
            if value > 0xffffffffffffffff: raise NoIndex('oversized_integer')
            return value, pos
    raise NoIndex('oversized_integer')


class Probe(rarfile.RAR5Parser):
    def _parse_main_block(self, header, data, pos):
        self.main_bytes = data
        return super()._parse_main_block(header, data, pos)


def locator(data):
    _, pos = vint(data, 4)
    kind, pos = vint(data, pos); flags, pos = vint(data, pos)
    if kind != 1: raise NoIndex('missing_main_header')
    extra, pos = vint(data, pos) if flags & 1 else (0, pos)
    if flags & 2: _, pos = vint(data, pos)
    if extra > len(data)-pos: raise NoIndex('invalid_main_extra')
    pos = len(data)-extra
    while pos < len(data):
        size, start = vint(data, pos); end = start+size
        if size == 0 or end > len(data): raise NoIndex('invalid_main_extra')
        kind, cur = vint(data, start)
        if kind == 1:
            flags, cur = vint(data, cur)
            if flags & 1:
                offset, cur = vint(data, cur)
                if cur > end or offset == 0: raise NoIndex('invalid_locator')
                return offset
        pos = end
    raise NoIndex('not_present')


def index_ranges(data, offset):
    """Validate index records; only their original-file byte positions are used."""
    pos = 0; ranges = []; previous_end = -1
    while pos < len(data):
        # Encrypted service data can have at most one AES block of zero padding.
        if len(data)-pos < 16 and not any(data[pos:]): break
        if len(data)-pos < 5: raise NoIndex('truncated_record')
        crc = struct.unpack_from('<I', data, pos)[0]
        size, cur = vint(data, pos+4); end = cur+size
        if not size or end > len(data) or zlib.crc32(data[pos+4:end]) != crc:
            raise NoIndex('index_crc_or_size')
        flags, cur = vint(data, cur); distance, cur = vint(data, cur); length, cur = vint(data, cur)
        start = offset-distance; stop = start+length
        if (flags != 0 or cur+length != end or not 1 <= length <= 65536 or
                start <= previous_end or start < 8 or stop > offset):
            raise NoIndex('invalid_index_range')
        ranges.append((start,stop-1)); previous_end = stop-1
        if len(ranges) > 100000: raise NoIndex('too_many_index_records')
        pos = end
    if not ranges: raise NoIndex('empty_index')
    return ranges, merge_ranges(ranges)


def merge_ranges(ranges, max_gap=1024):
    # Nearby small files can share one request, bounded to 64 KiB per request.
    merged = []
    for start,end in ranges:
        if merged and start-merged[-1][1]-1 <= max_gap and end-merged[-1][0]+1 <= 65536:
            merged[-1] = (merged[-1][0],end)
        else: merged.append((start,end))
    return merged


def plan_ranges(ranges, fp):
    # Large manifests can exceed the request cap while using very few bytes.
    # Coalesce nearby headers only when the complete plan fits BOTH budgets,
    # leaving room for the final standard-parser traversal. Every read still
    # goes through RangeFile's original byte/request/time accounting.
    requests = max(0, fp.max_requests - fp.requests - 8)
    byte_budget = max(0, fp.max_bytes - fp.bytes - 65536)
    for gap in (1024, 4096, 16384, 65536):
        candidate = merge_ranges(ranges, gap)
        if len(candidate) <= requests and sum(end-start+1 for start,end in candidate) <= byte_budget:
            return candidate
    return merge_ranges(ranges)


def prefetch_headers(fp, passwords):
    fp.seek(0)
    if fp.read(8) != b'Rar!\x1a\x07\x01\0': return dict(used=False,reason='not_rar5')
    parser = None
    try:
        for password in passwords:
            try:
                parser = Probe(fp,password,True,None,True,None,0,False)
                fp.seek(8); start = fp.tell(); header = parser._parse_header(fp)
                if header and header.block_type == rarfile.RAR5_BLOCK_ENCRYPTION:
                    if password is None: continue
                    start = fp.tell(); header = parser._parse_header(fp)
                if not header or header.block_type != rarfile.RAR5_BLOCK_MAIN: raise NoIndex('missing_main_header')
                if header.flags & rarfile.RAR_MAIN_VOLUME: raise NoIndex('multipart')
                offset = start+locator(parser.main_bytes)
                if not start < offset < fp.size-32: raise NoIndex('invalid_locator')
                fp.seek(offset); header = parser._parse_header(fp)
                if (not header or header.block_type != rarfile.RAR5_BLOCK_SERVICE or header.filename != 'QO' or
                        header.compress_type != rarfile.RAR_M0 or header.file_size != header.add_size or
                        not 0 < header.add_size <= 1024*1024): raise NoIndex('unsupported_index')
                if header.data_offset+header.add_size > fp.size: raise NoIndex('invalid_index_bounds')
                data = fp.read(header.add_size)
                if header.flags & rarfile.RAR_FILE_PASSWORD:
                    algo, _, kdf, salt, iv, _ = header.file_encryption
                    if algo != rarfile.RAR5_XENC_CIPHER_AES256 or len(data)%16: raise NoIndex('unsupported_index_encryption')
                    data = rarfile.AES_CBC_Decrypt(parser._gen_key(kdf,salt),iv).decrypt(data)
                ranges, _ = index_ranges(data,offset)
                merged = plan_ranges(ranges,fp)
                requests = fp.prefetch(merged)
                return dict(used=True,indexRecords=len(ranges),prefetchRequests=requests,
                            originalHeadersVerifiedBy='Full standard-header traversal; index copies are never trusted')
            except (rarfile.PasswordRequired,rarfile.RarWrongPassword): continue
        return dict(used=False,reason='index_password_unavailable')
    except NoIndex as error:
        return dict(used=False,reason=str(error))
    except (rarfile.BadRarFile,struct.error,ValueError,IndexError,AttributeError):
        return dict(used=False,reason='invalid_or_unsupported_index')
    finally:
        fp.seek(0)
