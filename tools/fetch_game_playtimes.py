"""Stage missing playtimes from exact official game pages, with bounded requests.

The input is a reviewed JSON list of {gameId, url} leads. A lead is not evidence:
the response must identify the exact title, edition and supported platform.
No catalogue writes, login, challenge handling or publishing are performed.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import date
import json
from pathlib import Path
import re
import threading
import time
from urllib.error import HTTPError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen
from enrich_game_insights import name_key, minutes
from game_insights import validate_insights


def official_url(url):
    parsed = urlsplit(url)
    if parsed.scheme != 'https' or parsed.netloc != 'howlongtobeat.com' or not re.fullmatch(r'/game/[1-9][0-9]*', parsed.path) or parsed.query or parsed.fragment:
        raise ValueError('Invalid official game URL')
    return parsed


def checked_playtime(html, game, platform, url):
    parsed = official_url(url)
    match = re.search(r'<script id="__NEXT_DATA__"[^>]*>(.*?)</script>', html, re.S)
    if not match:
        raise ValueError('Official game data unavailable')
    data = json.loads(match[1])['props']['pageProps']['game']['data']['game']
    if not isinstance(data, list) or len(data) != 1:
        raise ValueError('Ambiguous official game identity')
    row = data[0]
    if str(row.get('game_id')) != parsed.path.rsplit('/', 1)[1] or row.get('game_type') != 'game':
        raise ValueError('Official game identity mismatch')
    if name_key(row.get('game_name', '')) != name_key(game['title']):
        raise ValueError('Official title or edition mismatch')
    label = {'ps4': 'PlayStation 4', 'ps5': 'PlayStation 5'}[platform]
    if label not in row.get('profile_platform', '').split(', '):
        raise ValueError('Platform not listed by the source')
    result = {'edition': row['game_name'], 'url': url, 'checkedAt': date.today().isoformat()}
    for key, style in [('mainMinutes', 'main'), ('extrasMinutes', 'plus'), ('completionistMinutes', '100')]:
        count, seconds = row.get('comp_' + style + '_count'), row.get('comp_' + style + '_avg')
        if type(count) is int and count >= 3 and type(seconds) in (int, float) and seconds > 0:
            value = minutes(str(seconds))
            if value:
                result[key] = value
    validate_insights({'howLongToBeat': result}, platform)
    # Keep only aggregate fields used by the importer, not player reviews.
    evidence = {k: v for k, v in row.items() if k in ('game_id', 'game_name', 'game_type', 'profile_platform') or re.fullmatch(r'comp_(main|plus|100)_(count|avg)', k)}
    return result, evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('queue', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    games = {g['id']: g for g in json.loads((root / 'catalog/games.json').read_text())}
    platforms = {r['gameId']: 'ps4' if r['titleId'].startswith('CUSA') else 'ps5' for r in json.loads((root / 'catalog/releases.json').read_text())}
    queue = json.loads(args.queue.read_text())
    args.output.mkdir(parents=True, exist_ok=True)
    output = args.output / 'checked.json'
    previous = json.loads(output.read_text()) if output.exists() else []
    complete = {r['gameId'] for r in previous}
    queue = [r for r in queue if r['gameId'] not in complete and not games[r['gameId']].get('insights', {}).get('howLongToBeat')]
    stopped = threading.Event()
    lock = threading.Lock()
    last = 0

    def fetch(item):
        nonlocal last
        result = dict(item, status='held')
        if stopped.is_set():
            return dict(result, reason='Source refused requests; batch stopped')
        with lock:
            time.sleep(max(0, last + 0.8 - time.monotonic()))
            last = time.monotonic()
        if stopped.is_set():
            return dict(result, reason='Source refused requests; batch stopped')
        try:
            official_url(item['url'])
            req = Request(item['url'], headers={'User-Agent': 'OrbitStore-MetadataReview/1.0', 'Accept-Encoding': 'identity'})
            with urlopen(req, timeout=20) as response:
                if response.url != item['url']:
                    raise ValueError('Unexpected source redirect')
                body = response.read(2_000_001)
            if len(body) > 2_000_000:
                raise ValueError('Source page exceeded size limit')
            text = body.decode('utf8')
            if 'cf-chl-' in text or 'Just a moment...' in text:
                stopped.set()
                raise ValueError('Human verification required; batch stopped')
            record, evidence = checked_playtime(text, games[item['gameId']], platforms[item['gameId']], item['url'])
            return dict(result, status='ready', record=record, evidence=evidence)
        except HTTPError as error:
            if error.code in (401, 403, 429, 503):
                stopped.set()
            return dict(result, reason='HTTP ' + str(error.code))
        except (ValueError, KeyError, TypeError, OSError) as error:
            return dict(result, reason=str(error)[:160])

    with ThreadPoolExecutor(max_workers=3) as pool:
        for result in pool.map(fetch, queue):
            previous.append(result)
            temp = output.with_suffix('.tmp')
            temp.write_text(json.dumps(previous, ensure_ascii=False, indent=2) + '\n')
            temp.replace(output)
            if len(previous) % 25 == 0:
                print(json.dumps({'checked': len(previous), 'ready': sum(r['status'] == 'ready' for r in previous)}), flush=True)
    batch = []
    for row in previous:
        game = games[row['gameId']]
        if row['status'] == 'ready' and not game.get('insights', {}).get('howLongToBeat'):
            batch.append({'gameId': game['id'], 'platform': platforms[game['id']], 'insights': {**game.get('insights', {}), 'howLongToBeat': row['record']}})
    (args.output / 'insights-import.json').write_text(json.dumps({'games': batch}, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'checked': len(previous), 'ready': len(batch), 'stopped': stopped.is_set(), 'applied': False}))


if __name__ == '__main__':
    main()
