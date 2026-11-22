"""Checked Metacritic/HowLongToBeat metadata; no live scraping in Orbit apps.

Validate or apply a reviewed JSON batch: {"games":[{"gameId":...,"platform":
"ps5","insights":{...}}]}. Dates, URLs, platform and edition are evidence, not
inferences from game titles. Missing values are omitted, never zero-filled.
"""
import argparse
from datetime import date
import json
from pathlib import Path
from urllib.parse import urlsplit

def validate_insights(value, platform):
    if value is None:
        return
    if not isinstance(value, dict) or not value or set(value) - {'metacritic', 'howLongToBeat'}:
        raise ValueError('Invalid game insights')
    for name, record in value.items():
        if not isinstance(record, dict):
            raise ValueError('Insights need checked source records')
        checked = record.get('checkedAt')
        if not isinstance(checked, str) or len(checked) != 10 or date.fromisoformat(checked) > date.today():
            raise ValueError('Insights need a valid checked date')
        if 'snapshotAt' in record:
            if not isinstance(record['snapshotAt'], str) or len(record['snapshotAt']) != 10 or date.fromisoformat(record['snapshotAt']) > date.today():
                raise ValueError('Invalid source snapshot date')
            source = urlsplit(record.get('dataSource', ''))
            if source.scheme != 'https' or source.hostname != 'www.kaggle.com' or not source.path.startswith('/datasets/') or record.get('license') not in ('CC-BY-SA-4.0', 'CC0-1.0'):
                raise ValueError('Snapshot needs its published source and licence')
        url = urlsplit(record.get('url', ''))
        domain = 'metacritic.com' if name == 'metacritic' else 'howlongtobeat.com'
        if url.scheme != 'https' or url.hostname not in (domain, 'www.' + domain) or url.username or url.password or url.query or url.fragment or not url.path.startswith('/game/'):
            raise ValueError('Insights need the exact official game page')
        if name == 'metacritic':
            if set(record) - {'score', 'platform', 'url', 'checkedAt', 'snapshotAt', 'dataSource', 'license'} or record.get('platform') != platform or type(record.get('score')) is not int or not 0 <= record['score'] <= 100:
                raise ValueError('Metacritic score must match this platform')
        else:
            if set(record) - {'mainMinutes', 'extrasMinutes', 'completionistMinutes', 'edition', 'url', 'checkedAt', 'snapshotAt', 'dataSource', 'license'} or not isinstance(record.get('edition'), str) or not 1 <= len(record['edition']) <= 128:
                raise ValueError('HowLongToBeat needs its checked edition')
            times = [record.get(k) for k in ('mainMinutes', 'extrasMinutes', 'completionistMinutes')]
            if not any(v is not None for v in times) or any(v is not None and (type(v) is not int or not 1 <= v <= 600000) for v in times):
                raise ValueError('HowLongToBeat values are positive estimates in minutes')
            known = [v for v in times if v is not None]
            if known != sorted(known):
                raise ValueError('HowLongToBeat play styles are out of order')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('batch', type=Path, nargs='?')
    parser.add_argument('--missing', type=Path, help='write a review queue for old games and newly imported games')
    parser.add_argument('--apply', action='store_true', help='apply checked metadata locally; never publish')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    games_path = root / 'catalog/games.json'
    games = json.loads(games_path.read_text())
    releases = json.loads((root / 'catalog/releases.json').read_text())
    platforms = {r['gameId']: 'ps4' if r['titleId'].startswith('CUSA') else 'ps5' for r in releases}
    by_id = {g['id']: g for g in games}
    if args.missing:
        pending = []
        for game in games:
            insights = game.get('insights', {})
            missing = [name for name in ('metacritic', 'howLongToBeat') if name not in insights]
            if missing:
                pending.append({'gameId':game['id'], 'title':game['title'], 'platform':platforms[game['id']],
                    'releaseDate':game.get('releaseDate'), 'missing':missing, 'insights':insights})
        pending.sort(key=lambda g:(g.get('releaseDate') or '',g['title']), reverse=True)
        args.missing.parent.mkdir(parents=True, exist_ok=True)
        args.missing.write_text(json.dumps({'games':pending},ensure_ascii=False,indent=2)+'\n')
        print(json.dumps({'games':len(games),'pending':len(pending),'metacritic':sum(bool(g.get('insights',{}).get('metacritic')) for g in games),
            'howLongToBeat':sum(bool(g.get('insights',{}).get('howLongToBeat')) for g in games),'applied':False,'published':False}))
        return
    if not args.batch:
        parser.error('provide a reviewed batch or --missing report.json')
    rows = json.loads(args.batch.read_text())['games']
    seen = set()
    for row in rows:
        key = row['gameId']
        if key in seen or key not in by_id or row['platform'] != platforms[key]:
            raise ValueError('Duplicate, unknown or mismatched game identity')
        seen.add(key)
        validate_insights(row['insights'], row['platform'])
    changed = 0
    for row in rows:
        game = by_id[row['gameId']]
        if game.get('insights') != row['insights']:
            changed += 1
            game['insights'] = row['insights']
    if args.apply and changed:
        # Use the ordinary compiler before writing; no new feed files/generation.
        from catalog import compile_catalog
        compile_catalog(games, releases)
        games_path.write_text(json.dumps(games, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'reviewed': len(rows), 'changed': changed, 'applied': args.apply, 'published': False}))

if __name__ == '__main__':
    main()
