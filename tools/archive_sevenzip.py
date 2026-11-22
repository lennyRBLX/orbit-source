"""Bounded 7z metadata listing; never read or decompress a member's payload.

py7zr is pinned because its header parser is used directly for preflight. Its
encoded-header codecs are checked before opening the archive. A guarded view
then permits reads of only the signature, next header and encoded header stream.
"""
from contextlib import contextmanager
import io
import lzma
import re
import struct
import threading
import zlib

import py7zr
from py7zr import archiveinfo
from py7zr.properties import PROPERTY

from archive_rar import Held, PASSWORDS, classify

MAGIC = b'7z\xbc\xaf\x27\x1c'
MAX_HEADER = 64 * 1024 * 1024
MAX_DICTIONARY = 64 * 1024 * 1024
MAX_FILES = 500000
MAX_KDF_POWER = 20
CHUNK = 65536
AES = b'\x06\xf1\x07\x01'
_parser_lock = threading.RLock()


class InlineFilesInfo(archiveinfo.FilesInfo):
    def _read(self, fp):
        start = fp.tell(); count = archiveinfo.read_uint64(fp)
        if not 0 < count <= MAX_FILES: raise Held('7z_file_count_limit')
        end = fp.seek(0, 2); fp.seek(start)
        archiveinfo.read_uint64(fp)
        seen = set()
        while True:
            prop = fp.read(1)
            if prop == PROPERTY.END: break
            if not prop: raise Held('incomplete_7z_file_properties')
            size = archiveinfo.read_uint64(fp); position = fp.tell()
            if size > end-position: raise Held('invalid_7z_file_properties')
            if prop != PROPERTY.DUMMY and prop in seen: raise Held('duplicate_7z_file_property')
            seen.add(prop)
            if prop == PROPERTY.NAME:
                if not size or fp.read(1) != b'\0': raise Held('external_7z_file_properties')
            elif prop in (PROPERTY.ATTRIBUTES, PROPERTY.CREATION_TIME, PROPERTY.LAST_ACCESS_TIME, PROPERTY.LAST_WRITE_TIME):
                # Only parse the bounded bit vector, not external offsets.
                vector = io.BytesIO(fp.read(size))
                archiveinfo.read_boolean(vector, count, checkall=True)
                if vector.read(1) != b'\0': raise Held('external_7z_file_properties')
            elif prop not in (PROPERTY.DUMMY, PROPERTY.EMPTY_STREAM, PROPERTY.EMPTY_FILE):
                # Includes anti-items (incremental deletion) and split/start
                # positions, neither of which describes a standalone game.
                raise Held('unsupported_7z_file_property')
            fp.seek(position+size)
        if PROPERTY.NAME not in seen: raise Held('unnamed_7z_members')
        fp.seek(start)
        super()._read(fp)


@contextmanager
def parser_limits():
    # These globals govern allocations in the pinned metadata parser. Serialize
    # inspection in this process and restore them even when parsing fails.
    caps = dict(MAX_HEADER_SIZE=MAX_HEADER, MAX_NUMFILES=MAX_FILES,
                MAX_NUMFOLDERS=MAX_FILES, MAX_NUMSTREAMS=MAX_FILES,
                MAX_SUBSTREAMS=MAX_FILES, MAX_CODER_PROPERTIES=1024,
                MAX_CODERS=4, MAX_CODER_STREAMS=8)
    with _parser_lock:
        if any(type(getattr(archiveinfo, key, None)) is not int for key in caps):
            raise Held('7z_inspection_dependency_mismatch: use tools/rar-inspection-requirements.txt')
        original = {k: getattr(archiveinfo, k) for k in caps}
        files_info = archiveinfo.FilesInfo
        try:
            # Some valid non-solid archives have over 65,536 packed streams.
            # Bound that vector by our file limit instead of the library's
            # smaller default; its remaining-header checks still apply.
            for k, v in caps.items():
                setattr(archiveinfo, k, v if k == 'MAX_NUMSTREAMS' else min(original[k], v))
            archiveinfo.FilesInfo = InlineFilesInfo
            yield
        finally:
            archiveinfo.FilesInfo = files_info
            for k, v in original.items(): setattr(archiveinfo, k, v)


def read_at(fp, start, length):
    if not 0 <= start <= fp.size or not 0 <= length <= fp.size - start:
        raise Held('invalid_7z_header_bounds')
    fp.seek(start)
    block = fp.block
    fp.block = 0  # Do not read ahead into an adjacent file's packed bytes.
    try:
        data = bytearray()
        while length:
            part = fp.read(min(CHUNK, length))
            if not part: raise Held('incomplete_7z_header')
            data.extend(part); length -= len(part)
        return bytes(data)
    finally:
        fp.block = block


class HeaderView(io.RawIOBase):
    def __init__(self, fp, intervals):
        self.fp = fp; self.size = fp.size; self.intervals = intervals; self.pos = 0
    def readable(self): return True
    def seekable(self): return True
    def tell(self): return self.pos
    def seek(self, offset, whence=0):
        pos = offset if whence == 0 else self.pos + offset if whence == 1 else self.size + offset if whence == 2 else -1
        if pos < 0: raise Held('invalid_7z_seek')
        self.pos = pos; return pos
    def read(self, n=-1):
        n = self.size - self.pos if n is None or n < 0 else min(n, max(0, self.size-self.pos))
        if n == 0: return b''
        if not any(start <= self.pos and self.pos+n <= end for start, end in self.intervals):
            raise Held('7z_parser_requested_payload')
        data = read_at(self.fp, self.pos, n); self.pos += len(data)
        return data


def check_coders(folder):
    coders = folder.coders
    if not 1 <= len(coders) <= 4: raise Held('unsupported_7z_header_codec')
    if (folder.packed_indices != [0] or
            [(p.incoder, p.outcoder) for p in folder.bindpairs] != [(i+1, i) for i in range(len(coders)-1)]):
        raise Held('unsupported_7z_header_codec')
    crypt = 0; compressors = 0
    for coder in coders:
        if coder.get('numinstreams') != 1 or coder.get('numoutstreams') != 1:
            raise Held('unsupported_7z_header_codec')
        method = coder['method']; props = coder.get('properties') or b''
        if method in (b'\x03\x01\x01', b'\x21'):
            compressors += 1
            filter_id = lzma.FILTER_LZMA1 if method == b'\x03\x01\x01' else lzma.FILTER_LZMA2
            try: spec = lzma._decode_filter_properties(filter_id, props)
            except (ValueError, lzma.LZMAError) as e: raise Held('invalid_7z_header_codec') from e
            if spec.get('dict_size', 0) > MAX_DICTIONARY:
                raise Held('7z_header_dictionary_limit')
        elif method == AES:
            crypt += 1
            if len(props) < 2 or not props[0] & 0xc0:
                raise Held('invalid_7z_header_encryption')
            if (props[0] & 0x3f) > MAX_KDF_POWER:
                raise Held('7z_header_password_cost_limit')
            salt_size = ((props[0] >> 7) & 1) + (props[1] >> 4)
            iv_size = ((props[0] >> 6) & 1) + (props[1] & 15)
            if salt_size > 16 or iv_size > 16 or len(props) != 2 + salt_size + iv_size:
                raise Held('invalid_7z_header_encryption')
        elif method != b'\x00' or props:
            raise Held('unsupported_7z_header_codec')
    if crypt > 1 or compressors > 1: raise Held('unsupported_7z_header_codec')
    if not folder.unpacksizes or any(type(n) is not int or not 0 <= n <= MAX_HEADER for n in folder.unpacksizes):
        raise Held('7z_decoded_header_limit')
    return bool(crypt)


def preflight(fp):
    if fp.size < 32: raise Held('incomplete_7z_header')
    start = read_at(fp, 0, 32)
    if start[:6] != MAGIC: raise Held('not_7z')
    if start[6] != 0: raise Held('unsupported_7z_version')
    crc, offset, size, next_crc = struct.unpack('<IQQI', start[8:])
    if zlib.crc32(start[12:]) != crc: raise Held('invalid_7z_start_crc')
    next_start = 32 + offset
    if not 0 < size <= MAX_HEADER or not 32 <= next_start <= fp.size or size != fp.size-next_start:
        raise Held('invalid_7z_header_bounds')
    data = read_at(fp, next_start, size)
    if zlib.crc32(data) != next_crc: raise Held('invalid_7z_next_crc')
    intervals = [(0, 32), (next_start, fp.size)]
    if data[:1] == PROPERTY.HEADER:
        return HeaderView(fp, intervals), False, next_start
    if data[:1] != PROPERTY.ENCODED_HEADER: raise Held('unsupported_7z_header')
    buf = io.BytesIO(data[1:])
    streams = archiveinfo.HeaderStreamsInfo.retrieve(buf)
    if buf.tell() != len(data)-1: raise Held('invalid_7z_encoded_header')
    pack = streams.packinfo; unpack = streams.unpackinfo
    # The library's encoded-header path expects a single linear stream. Do not
    # guess at multiple folders or read external/additional metadata streams.
    if (pack.numstreams != 1 or len(pack.packsizes) != 1 or
            unpack.numfolders != 1 or len(unpack.folders) != 1 or streams.substreamsinfo is not None):
        raise Held('unsupported_7z_header_streams')
    encrypted = check_coders(unpack.folders[0])
    packed_start = 32 + pack.packpos; packed_size = pack.packsizes[0]
    if not 32 <= packed_start < next_start or not 0 < packed_size <= next_start-packed_start:
        raise Held('invalid_7z_header_bounds')
    if packed_size > fp.max_bytes: raise Held('7z_packed_header_limit')
    intervals.append((packed_start, packed_start+packed_size))
    return HeaderView(fp, intervals), encrypted, packed_start


def validate_streams(archive, payload_end):
    header = archive.header
    if header.files_info is None: raise Held('empty_7z_archive')
    raw = header.files_info.files
    if any(not isinstance(f.get('filename'), str) or f.get('anti', False) for f in raw):
        raise Held('unsupported_7z_file_entry')
    streams = header.main_streams
    if streams is None:
        if any(not f['emptystream'] for f in raw): raise Held('invalid_7z_payload_sizes')
        return
    pack = streams.packinfo; unpack = streams.unpackinfo; sub = streams.substreamsinfo
    if (pack is None or unpack is None or sub is None or
            pack.numstreams != len(pack.packsizes) or unpack.numfolders != len(unpack.folders)):
        raise Held('invalid_7z_payload_sizes')
    if any(type(n) is not int or n < 0 for n in pack.packsizes) or pack.packpos < 0:
        raise Held('invalid_7z_payload_sizes')
    if 32 + pack.packpos + sum(pack.packsizes) > payload_end:
        raise Held('invalid_7z_payload_bounds')
    counts = sub.num_unpackstreams_folders
    if len(counts) != len(unpack.folders) or sum(counts) != sum(not f['emptystream'] for f in raw):
        raise Held('invalid_7z_payload_sizes')
    if sum(f.uncompressed for f in archive.files) != sum(f.unpacksizes[-1] for f in unpack.folders):
        raise Held('invalid_7z_payload_sizes')


def header_error(error):
    if (isinstance(error, py7zr.exceptions.Bad7zFile) and
            re.fullmatch(r'.+ value \d+ exceeds limit \d+', str(error))):
        return Held('7z_parser_count_limit')
    return Held('invalid_7z_headers')


def inspect(fp, title_id, passwords=()):
    try:
        with parser_limits():
            view, header_encrypted, payload_end = preflight(fp)
            candidates = list(dict.fromkeys([*passwords, *PASSWORDS])) if header_encrypted else [None]
            for password in candidates:
                view.seek(0)
                try:
                    archive = py7zr.SevenZipFile(view, 'r', password=password)
                except Held: raise
                except Exception as e:
                    if header_encrypted: continue
                    raise header_error(e) from e
                with archive:
                    validate_streams(archive, payload_end)
                    encrypted = header_encrypted or archive.needs_password()
                    entries = [dict(path=f.filename, sizeBytes=f.uncompressed, directory=f.is_directory,
                        special=bool(f.is_symlink or f.is_junction or f.is_socket or
                                     not (f.is_file or f.is_directory)),
                        encrypted=bool(f.folder and any(c['method'] == AES for c in f.folder.coders)),
                        crc32=f.crc32) for f in archive.files]
                break
            else: raise Held('header_password_required')
        result = classify(entries, title_id, max_entries=MAX_FILES)
        result.update(compression='7z', headerEncrypted=header_encrypted,
                      headerPassword=password if header_encrypted else None, encrypted=encrypted,
                      payloadExtractionTested=False, payloadPasswordVerified=False,
                      consoleVerified=False, ranges=fp.ranges)
        return result
    except Held: raise
    except Exception as e: raise header_error(e) from e
