#!/usr/bin/env python3
"""Preserve release-name claims as evidence, never as verified compatibility.

This reads names already present in archive inspection reports. It does not
download, extract or execute files, and never changes catalogue requirements.
"""
import argparse
import json
from pathlib import Path
import re
import time

BOUNDARY = r'(?<![a-z0-9])'
PATTERNS = {
    'titleId': re.compile(BOUNDARY + r'(?:PPSA|CUSA)\d{5}(?!\d)', re.I),
    'firmware': re.compile(BOUNDARY + r'(?:fw|firmware)[ _:.=-]*(\d{1,2}(?:[._](?:\d{1,2}|xx))?(?:[ ]*\+)?)(?![a-z0-9])', re.I),
    'version': re.compile(BOUNDARY + r'(?:v|ver\.?|version)[ _:.=-]*(\d{1,3}[._]\d{1,3}(?:[._]\d{1,3})?)(?![a-z0-9])', re.I),
    'backport': re.compile(BOUNDARY + r'back[ _-]?port(?:ed|s)?(?![a-z0-9])', re.I),
    'region': re.compile(BOUNDARY + r'(?:USA|EUR|EUROPE|JPN|JAPAN|ASIA)(?![a-z0-9])', re.I),
}
FIRMWARE_WILDCARD = re.compile(BOUNDARY + r'(\d{1,2}[._]xx(?:[ ]*\+)?)(?![a-z0-9])', re.I)
BACKPORT_SHORT = re.compile(BOUNDARY + r'bp(?=[ _:.=(\[-]+(?:fw[ _:.=-]*)?\d{1,2}[._](?:\d{2}|xx)(?!\d))', re.I)
BACKPORT_FIRMWARE = re.compile(BOUNDARY + r'(\d{1,2}[._](?:\d{2}|xx)(?:[ ]*\+)?)(?![a-z0-9]|\.\d)', re.I)
UNPREFIXED_VERSION = re.compile(BOUNDARY + r'(\d{2,3}[._]\d{3}[._]\d{3})(?![a-z0-9]|\.\d)', re.I)


def name_hints(filename, files):
    sources = [('archiveFilename', filename)]
    sources.extend(('archiveMember', row.get('path', '')) for row in files)
    hints = []
    seen = set()
    for source, path in sources:
        # Inspect each component only once. A firmware-labelled root repeated
        # in 10,000 paths must not produce 10,000 identical evidence entries.
        for component in path.replace('\\', '/').split('/'):
            identity = (source, component)
            if not component or identity in seen:
                continue
            seen.add(identity)
            matches = [(field, match) for field, pattern in PATTERNS.items()
                       for match in pattern.finditer(component)]
            # Publisher labels also use "4.xx" and "BP 4.xx" without an FW
            # prefix. Keep these as name claims with their original context.
            matches.extend(('backport', match) for match in BACKPORT_SHORT.finditer(component))
            firmware_pattern = BACKPORT_FIRMWARE if any(field == 'backport' for field, _ in matches) else FIRMWARE_WILDCARD
            for match in firmware_pattern.finditer(component):
                if not any(field == 'firmware' and other.start() <= match.start() < other.end()
                           for field, other in matches):
                    matches.append(('firmware', match))
            # A bare PS5-style version is meaningful in the archive name or
            # a title-labelled directory, not an arbitrary internal filename.
            if source == 'archiveFilename' or PATTERNS['titleId'].search(component):
                for match in UNPREFIXED_VERSION.finditer(component):
                    if not any(field == 'version' and other.start() <= match.start() < other.end()
                               for field, other in matches):
                        matches.append(('version', match))
            for field, match in matches:
                value = match.group(1).replace('_', '.') if field in ('firmware', 'version') else match.group(0)
                hint = dict(field=field, value=value, matchedText=match.group(0),
                            source=source, component=component, examplePath=path, verified=False)
                if field == 'backport':
                    # Preserve negation and other qualifiers; do not emit
                    # a boolean that could turn "not backported" into yes.
                    hint['value'] = 'mentioned'
                    hint['context'] = component[max(0, match.start()-48):match.end()+48]
                hints.append(hint)
    values = {}
    for hint in hints:
        if hint['field'] != 'backport':
            values.setdefault(hint['field'], set()).add(hint['value'].upper())
    return dict(method='Archive filename and listed member names only',
                compatibilityVerified=False, claims=hints,
                fieldsWithMultipleValues=sorted(k for k, v in values.items() if len(v)>1))


def enrich_result(result):
    return name_hints(result.get('original', {}).get('filename', ''),
                      result.get('inspection', {}).get('files', []))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path, help='Inspection report or background status JSON')
    parser.add_argument('--output', type=Path, required=True, help='Separate derived evidence file')
    parser.add_argument('--watch', action='store_true', help='Refresh until a background status leaves running/stopping')
    args = parser.parse_args()
    if args.output.resolve() == args.report.resolve():
        parser.error('Keep the original inspection evidence unchanged')
    while True:
        data = json.loads(args.report.read_text())
        results = [dict(releaseId=r['releaseId'], game=r.get('game'),
                        archiveInspectionStatus=r.get('status'),
                        metadataHints=enrich_result(r)) for r in data.get('results', [])]
        output = dict(sourceReport=str(args.report.resolve()), published=False,
                      applied=False, results=results)
        temporary = args.output.with_suffix(args.output.suffix+'.tmp')
        temporary.write_text(json.dumps(output, ensure_ascii=False, indent=2)+'\n')
        temporary.replace(args.output)
        if not args.watch or data.get('phase') not in ('running', 'stopping'):
            break
        time.sleep(15)


if __name__ == '__main__':
    main()
