"""Tests for the Padrino snapshot importer and its skip diagnostics."""

from contextlib import redirect_stderr, redirect_stdout
import gzip
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools import import_padrino


FICHIER = 'https://1fichier.com/?A1b2C3d4E5f6G7h8I9j0'
DIRECT = 'https://example.org/download/Game%20Disc.rar'


class PadrinoImporterTests(unittest.TestCase):
    def test_supported_provider_pages(self):
        fixture = Path(__file__).parent / 'fixtures/community-provider-pages.json'
        cases = [(url, url) for _, url in json.loads(fixture.read_text())['providers']]
        cases += [('https://akirabox.to/abcd12345678/file/', 'https://akirabox.to/abcd12345678/file')]
        for url, canonical in cases:
            with self.subTest(url=url):
                link, reason = import_padrino.option({
                    'url': url, 'requiresBrowser': True, 'filename': 'unknown.binary',
                    'sha256': 'a' * 64,
                })
                self.assertIsNone(reason)
                self.assertEqual(link, {'url': canonical, 'name': 'Padrino Store', 'page': True})

    def test_1fichier_rejects_noncanonical_routes(self):
        cases = (
            FICHIER[:-1], FICHIER + 'x', FICHIER[:-1] + '-', FICHIER[:-1] + '%30',
            FICHIER + '&af=1', FICHIER + '#', FICHIER + '#fragment',
            FICHIER.replace('1fichier.com', '1fichier.com.example.org'),
            FICHIER.replace('1fichier.com', 'www.1fichier.com'),
            FICHIER.replace('1fichier.com', '1fichier.com:443'),
            FICHIER.replace('1fichier.com', '@1fichier.com'),
            FICHIER.replace('/?', '/account?'), FICHIER.replace('/?', '?'),
        )
        for url in cases:
            with self.subTest(url=url):
                link, reason = import_padrino.option({'url': url, 'requiresBrowser': True})
                self.assertIsNone(link)
                self.assertIsNotNone(reason)

    def test_fallback_preserves_direct_file_and_browser_behavior(self):
        cases = (
            ({'browserUrl': FICHIER, 'url': DIRECT}, FICHIER, True),
            ({'browserUrl': FICHIER + '#bad', 'url': FICHIER, 'requiresBrowser': True}, FICHIER, True),
            ({'browserUrl': FICHIER[:-1], 'url': DIRECT}, DIRECT, False),
        )
        for row, url, page in cases:
            with self.subTest(row=row):
                link, reason = import_padrino.option({**row, 'filename': 'untrusted.rar', 'sha256': 'A' * 64})
                self.assertIsNone(reason)
                self.assertEqual(link['url'], url)
                self.assertEqual(link.get('page', False), page)
                self.assertEqual(link['filename'], 'untrusted.rar' if page else 'Game Disc.rar')
                self.assertEqual(link.get('sha256'), None if page else 'a' * 64)

    def test_skip_reasons(self):
        cases = (
            ({'url': DIRECT, 'requiresBrowser': True}, 'unsupported_provider_page'),
            ({'url': DIRECT, 'label': 'LZ4 game folder'}, 'unsupported_compression'),
            ({'url': DIRECT, 'label': 'Game part 2'}, 'multipart'),
            ({'url': 'https://filekeeper.net/abc123def456/Game.part01.rar', 'requiresBrowser': True}, 'multipart'),
            ({'url': 'https://rapidgator.net/file/0123456789abcdef0123456789abcdef/Game.part01.rar.html',
              'requiresBrowser': True}, 'multipart'),
            ({'url': DIRECT, 'category': 'update'}, 'unsupported_category'),
            ({'url': DIRECT, 'volume': 1}, 'multipart'),
            ({'url': DIRECT, 'filename': 'game.part01.rar'}, 'multipart'),
            ({'url': 'https://example.org/game.7z.001'}, 'multipart'),
            ({'url': DIRECT, 'unavailableReason': 'removed'}, 'unavailable'),
            ({'url': DIRECT, 'health': 'dead'}, 'unavailable'),
            ({'url': DIRECT, 'compression': 'zstandard'}, 'unsupported_compression'),
            ({'url': 'https://example.org/game-%7Astd.rar'}, 'unsupported_compression'),
            ({'url': 'http://example.org/game.rar'}, 'invalid_url_or_filename'),
            ({'url': 'https://example.org:invalid/game.rar'}, 'invalid_url_or_filename'),
            ({'url': 'https://example.org/game.rar#'}, 'invalid_url_or_filename'),
            ({'url': 'https://example.org/unsafe%2Fgame.rar'}, 'invalid_url_or_filename'),
        )
        for row, reason in cases:
            with self.subTest(row=row):
                self.assertEqual(import_padrino.option(row), (None, reason))

    def test_preserves_provider_metadata(self):
        link, reason = import_padrino.option({
            'id': 'fixture', 'label': 'EUR · exFAT', 'provider': '1fichier', 'url': FICHIER,
            'version': '2.10', 'region': 'EUR', 'filename': 'Cyberpunk.exfat', 'sizeBytes': 5678,
            'sourcePage': 'https://example.org/game', 'archivePassword': 'example',
        })
        self.assertIsNone(reason)
        self.assertEqual(link, {
            'name': '1fichier', 'url': FICHIER, 'page': True, 'label': 'EUR · exFAT',
            'version': '2.10', 'region': 'EUR', 'filename': 'Cyberpunk.exfat', 'format': 'exfat',
            'sizeEstimateBytes': 5678, 'sourcePage': 'https://example.org/game', 'archivePassword': 'example',
        })

    def test_convert_reports_rejected_rows_without_changing_the_snapshot_schema(self):
        rows = [
            {'id': 'accepted', 'url': FICHIER, 'requiresBrowser': True},
            {'id': 'rejected', 'provider': 'Example', 'url': 'https://example.org/id',
             'browserUrl': 'https://example.org/page', 'requiresBrowser': True},
        ]
        with tempfile.TemporaryDirectory() as directory:
            checkout = Path(directory)
            data = checkout / 'app/data'
            releases = data / 'releases'
            releases.mkdir(parents=True)
            (data / 'catalog.js').write_text('var games = [{"id":"PPSA12345","title":"Example"}];\nvar twins = [];\n')
            (releases / 'PPSA12345.json').write_text(json.dumps(rows))
            subprocess.run(['git', 'init', '-q'], cwd=checkout, check=True)
            subprocess.run(['git', 'add', 'app/data'], cwd=checkout, check=True)
            subprocess.run(['git', '-c', 'user.name=Test', '-c', 'user.email=test@example.org',
                            'commit', '-qm', 'Fixture'], cwd=checkout, check=True)

            snapshot = checkout / 'snapshot.json.gz'
            stdout, stderr = io.StringIO(), io.StringIO()
            with patch.object(import_padrino, 'SNAPSHOT', snapshot), redirect_stdout(stdout), redirect_stderr(stderr):
                import_padrino.convert(checkout)
                self.assertEqual(stderr.getvalue(), '')
                original = snapshot.read_bytes()
                import_padrino.convert(checkout, show_skips=True)
                self.assertEqual(snapshot.read_bytes(), original)

            self.assertIn('Skipped 1 candidates (unsupported_provider_page=1)', stdout.getvalue())
            self.assertEqual([json.loads(line) for line in stderr.getvalue().splitlines()], [{
                'titleId': 'PPSA12345', 'id': 'rejected', 'reason': 'unsupported_provider_page',
                'provider': 'Example', 'url': 'https://example.org/[redacted]', 'browserUrl': 'https://example.org/[redacted]',
            }])
            result = json.loads(gzip.decompress(original))
            self.assertEqual(result['schemaVersion'], 1)
            self.assertEqual(result['packages'][0]['downloadLinks'], [
                {'url': FICHIER, 'name': 'Padrino Store', 'page': True},
            ])
