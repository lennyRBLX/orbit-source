"""Identify PS5 debug packages inside stored 7z members using bounded metadata.

No member is extracted, decompressed or executed. Compressed/encrypted member
streams stay held. This verifies container type and title identity, not the
complete package's integrity, installability or firmware compatibility.
FIH layout reference: SvenGDK/LibProsperoPKG/docs/ps5-pkg-format.md, section 5.
"""
import re

import py7zr

from archive_rar import Held
from archive_sevenzip import parser_limits, preflight, read_at, validate_streams


def stored_members(archive, payload_end):
    streams = archive.header.main_streams
    if streams is None:
        raise Held('package_stream_missing')
    pack, unpack = streams.packinfo, streams.unpackinfo
    if pack.numstreams != len(unpack.folders):
        raise Held('package_stream_layout_unsupported')
    position = 32 + pack.packpos
    result = {}
    files = list(archive.files)
    for folder, packed_size in zip(unpack.folders, pack.packsizes):
        members = [f for f in files if not f.emptystream and f.folder is folder]
        coders = folder.coders
        stored = (len(coders) == 1 and coders[0]['method'] == b'\x00' and
                  coders[0].get('numinstreams') == 1 and coders[0].get('numoutstreams') == 1 and
                  not coders[0].get('properties') and folder.packed_indices == [0] and
                  not folder.bindpairs and sum(f.uncompressed for f in members) == packed_size)
        offset = position
        for member in members:
            if stored:
                if offset < 32 or offset + member.uncompressed > payload_end:
                    raise Held('package_member_out_of_bounds')
                result[member.filename] = (offset, member.uncompressed)
            offset += member.uncompressed
        position += packed_size
    return result


def inspect_packages(fp, row, detail):
    if row.get('compression') != '7z' or not detail.get('complete'):
        raise Held('package_probe_requires_complete_7z_headers')
    files = detail['files']
    packages = [f for f in files if f['path'].lower().endswith(('.pkg', '.fpkg'))]
    if not 1 <= len(packages) <= 16:
        raise Held('package_count_unsupported')
    package_paths = {f['path'] for f in packages}
    # These auxiliary checksum files occur in the Black Ops bundles. They are
    # listed but never executed. Other mixed payloads require separate review.
    for f in files:
        if f['path'] in package_paths:
            if set(re.findall(r'(?:PPSA|CUSA)\d{5}(?!\d)', f['path'])) != {row['titleId']}:
                raise Held('package_filename_title_mismatch')
        elif not (f['sizeBytes'] <= 65536 and (f['path'].lower().endswith(('.txt', '.nfo', '.sha256', '.sfv', '.md5', '.ini')) or f['path'] == 'verify_checksum.bat')) and not (f['path'] == '_SHA256/SharpSFV.exe' and f['sizeBytes'] <= 1024*1024):
            raise Held('package_mixed_payload_requires_review')
    with parser_limits():
        view, _, payload_end = preflight(fp)
        with py7zr.SevenZipFile(view, 'r', password=detail.get('headerPassword')) as archive:
            validate_streams(archive, payload_end)
            actual = {f.filename: f.uncompressed for f in archive.files if not f.is_directory}
            if actual != {f['path']: f['sizeBytes'] for f in files}:
                raise Held('package_manifest_changed')
            offsets = stored_members(archive, payload_end)
    evidence = []
    for f in packages:
        if f['path'] not in offsets:
            raise Held('package_member_compressed_or_encrypted')
        start, size = offsets[f['path']]
        if size < 0x10000:
            raise Held('package_too_small')
        header = read_at(fp, start, 0xA8)
        if header[:4] == b'\x7fLIH':
            # LIH is a separate patch container, not a base-game FIH image.
            # Bundles label these "backport" or "original files". Neither
            # wording establishes compatibility: identify only the container.
            if int.from_bytes(header[0x18:0x20], 'little') != size:
                raise Held('package_patch_size_mismatch')
            evidence.append(dict(path=f['path'], sizeBytes=size, kind='PS5 LIH patch companion',
                                 headerOffset=start, contentIdentityVerified=False,
                                 completeIntegrityTested=False))
            continue
        if header[:4] != b'\x7fFIH':
            raise Held('package_not_ps5_finalized_image')
        if header[5] != 0:
            raise Held('package_not_debug_image')
        pfs_start = int.from_bytes(header[0x10:0x18], 'little')
        pfs_size = int.from_bytes(header[0x18:0x20], 'little')
        cnt_start = int.from_bytes(header[0x58:0x60], 'little')
        # Only inspect the fixed CNT prefix at the advertised offset. FIH
        # variants use different later fields; do not infer the full SC size
        # from 0xA0 or claim that this validates every package segment.
        if (pfs_start != 0x10000 or pfs_size <= 0 or cnt_start < pfs_start+pfs_size or
                cnt_start > size or 0x70 > size-cnt_start):
            raise Held('package_segment_bounds_invalid')
        cnt = read_at(fp, start+cnt_start, 0x70)
        if cnt[:4] != b'\x7fCNT':
            raise Held('package_metadata_container_invalid')
        cid = cnt[0x40:0x70].split(b'\0', 1)[0]
        if not re.fullmatch(rb'[A-Z]{2}\d{4}-' + row['titleId'].encode() + rb'_\d{2}-[A-Za-z0-9_-]{1,16}', cid):
            raise Held('package_content_id_mismatch')
        evidence.append(dict(path=f['path'], sizeBytes=size, kind='PS5 debug finalized image',
                             contentId=cid.decode('ascii'), signedByte=header[5],
                             headerOffset=start, metadataOffset=start+cnt_start,
                             completeIntegrityTested=False))
    if not any(p['kind'] == 'PS5 debug finalized image' for p in evidence):
        raise Held('base_debug_package_missing')
    return evidence
