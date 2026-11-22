"""Stage bulk score/playtime metadata from published offline CSV snapshots.

Never contacts a game site, changes the catalogue, or guesses an edition.
Run game_insights.py on the resulting batch to validate/apply it locally.
"""
import argparse
import csv
from datetime import date
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP
import io
import json
from pathlib import Path
import re
import unicodedata
from urllib.parse import urlsplit
import zipfile
from game_insights import validate_insights

HLTB_SOURCE = 'https://www.kaggle.com/datasets/zaireali/howlongtobeat-games-scraper-2162025'
MC_SOURCE = 'https://www.kaggle.com/datasets/zaireali/metacritic-games-scrape'
MC_PLATFORM_SOURCE = 'https://www.kaggle.com/datasets/sinaghafouri/video-games-on-metacriticcom'

def name_key(name):
    # Punctuation/diacritics only. Keep edition words, numbers, order and subtitles.
    return ''.join(c for c in unicodedata.normalize('NFKD', name).casefold() if c.isalnum())

def rows(path, members=None):
    if not path:
        return
    with zipfile.ZipFile(path) as archive:
        for item in archive.infolist():
            if item.filename.lower().endswith('.csv') and item.file_size <= 120 * 1024 * 1024 and (members is None or item.filename in members):
                with archive.open(item) as source:
                    for row in csv.DictReader(io.TextIOWrapper(source, encoding='utf-8-sig')):
                        row['_datasetMember'] = item.filename
                        yield row

def minutes(value, divisor=60):
    try:
        number = Decimal(value) / divisor
        return int(number.quantize(Decimal('1'), rounding=ROUND_HALF_UP)) if number > 0 else None
    except (InvalidOperation, TypeError):
        return None

def provenance(url, snapshot, licence):
    return {'checkedAt': date.today().isoformat(), 'snapshotAt': snapshot,
            'dataSource': url, 'license': licence}

def build(games, releases, hltb_zip=None, mc_zip=None, platform_zip=None):
    platforms = {r['gameId']: 'ps4' if r['titleId'].startswith('CUSA') else 'ps5' for r in releases}
    times, scores = {}, {}
    for row in rows(hltb_zip):
        if row.get('game_game_type') != 'game':
            continue
        identity = row.get('game_game_id', '')
        if not re.fullmatch(r'[1-9][0-9]{0,8}', identity):
            continue
        name = row.get('game_game_name', '')
        # Use explicitly documented per-style averages, not speedruns, user
        # review scores, or the dataset's undocumented combined statistics.
        record = {'edition': name, 'url': 'https://howlongtobeat.com/game/' + identity,
                  **provenance(HLTB_SOURCE, '2025-02-17', 'CC-BY-SA-4.0')}
        for key, column in [('mainMinutes', 'main'), ('extrasMinutes', 'plus'), ('completionistMinutes', '100')]:
            if Decimal(row.get('game_comp_' + column + '_count') or '0') >= 3:
                value = minutes(row.get('game_comp_' + column + '_avg'))
                if value:
                    record[key] = value
        if not any(k in record for k in ('mainMinutes', 'extrasMinutes', 'completionistMinutes')):
            continue
        for platform, label in [('ps4', 'PlayStation 4'), ('ps5', 'PlayStation 5')]:
            if label not in row.get('game_profile_platform', '').split(', '):
                continue
            times.setdefault((name_key(name), platform), {})[identity] = record
    for row in rows(mc_zip):
        platform_list = {value.strip() for key, value in row.items() if key.startswith('platforms/') and value}
        # This source's headline metascore is not platform-specific. Accept it
        # only when exactly one platform exists. Never call a PC score PS5.
        if len(platform_list) != 1:
            continue
        platform = {'PlayStation 4': 'ps4', 'PlayStation 5': 'ps5'}.get(next(iter(platform_list)))
        raw = row.get('metascore', '')
        if not platform or not raw.isdigit() or not 0 <= int(raw) <= 100:
            continue
        url = row.get('url', '')
        if urlsplit(url).hostname not in ('metacritic.com', 'www.metacritic.com'):
            continue
        record = {'score': int(raw), 'platform': platform, 'url': url,
                  **provenance(MC_SOURCE, '2025-02-16', 'CC-BY-SA-4.0')}
        scores.setdefault((name_key(row.get('title', '')), platform), {})[url] = record
    # A platform-specific secondary snapshot can fill multi-platform games;
    # its parser intentionally accepts only an explicit platform+score row.
    for row in rows(platform_zip, ('01_ps4_details.csv','01_ps5_details.csv')):
        row = {**row, 'platform':'PS4' if row['_datasetMember']=='01_ps4_details.csv' else 'PS5'}
        platform = {'PlayStation 4': 'ps4', 'PlayStation 5': 'ps5', 'PS4': 'ps4', 'PS5': 'ps5'}.get(row.get('platform', '').strip())
        raw = row.get('Metascore',row.get('metascore', row.get('meta_score', '')))
        if not platform or not str(raw).isdigit() or not 0 <= int(raw) <= 100:
            continue
        title, url = row.get('Name',row.get('name', row.get('title', ''))), row.get('Link',row.get('url', row.get('link', '')))
        if urlsplit(url).hostname not in ('metacritic.com', 'www.metacritic.com'):
            continue
        key = (name_key(title), platform)
        if key in scores and any(record.get('snapshotAt','') > '2023-03-05' for record in scores[key].values()):
            continue
        record = {'score':int(raw), 'platform':platform, 'url':url,
                  **provenance(MC_PLATFORM_SOURCE, '2023-03-05', 'CC0-1.0')}
        scores.setdefault(key, {})[url] = record
    staged, held = [], []
    for game in games:
        key = (name_key(game['title']), platforms[game['id']])
        current = dict(game.get('insights') or {})
        found = dict(current)
        for field, index in [('metacritic', scores), ('howLongToBeat', times)]:
            if field in current:
                continue
            matches = index.get(key, {})
            if len(matches) == 1:
                found[field] = next(iter(matches.values()))
            elif len(matches) > 1:
                held.append({'gameId':game['id'], 'source':field, 'reason':'ambiguous-edition', 'candidates':list(matches)})
        for field in list(found):
            if field in current:
                continue
            try:
                validate_insights({field:found[field]}, platforms[game['id']])
            except ValueError as error:
                held.append({'gameId':game['id'], 'source':field, 'reason':str(error)})
                del found[field]
        if found != current:
            staged.append({'gameId':game['id'], 'platform':platforms[game['id']], 'insights':found})

    return {'games':staged}, {'staged':len(staged), 'metacritic':sum('metacritic' in row['insights'] for row in staged),
        'howLongToBeat':sum('howLongToBeat' in row['insights'] for row in staged), 'held':held,
        'matching':'unique exact title and supported platform; edition words retained', 'applied':False, 'published':False}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hltb-zip', type=Path)
    parser.add_argument('--metacritic-zip', type=Path)
    parser.add_argument('--platform-score-zip', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    batch, report = build(json.loads((root/'catalog/games.json').read_text()), json.loads((root/'catalog/releases.json').read_text()),
        args.hltb_zip, args.metacritic_zip, args.platform_score_zip)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output/'insights-import.json').write_text(json.dumps(batch,ensure_ascii=False,indent=2)+'\n')
    (args.output/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps({key:value for key,value in report.items() if key!='held'} | {'held':len(report['held'])}))

if __name__ == '__main__':
    main()
