#!/usr/bin/env python3
"""Research public DLPS numeric download indexes. Never edit Orbit's catalogue.

Example: python3 tools/dlps_archive_scan.py --start-id 51750 --end-id 51741
         --output .state/dlps-archive-discovery/sample.json

IDs identify source pages, not games. A page may describe an update, DLC, a
multipart download or another platform. Provider links remain unverified leads.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
from datetime import datetime, timezone
from html.parser import HTMLParser
import json
from pathlib import Path
import re
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin, urlsplit, unquote
from urllib.request import HTTPRedirectHandler, Request, build_opener


BASE = "https://downloadgameps3.net"
PAGE_LIMIT = 2_000_000
PROVIDERS = {
    "vikingfile.com": "Vikingfile", "vik1ngfile.site": "Vikingfile",
    "akirabox.to": "AkiraBox", "akirabox.com": "AkiraBox",
    "archive.org": "Archive.org", "1fichier.com": "1fichier",
    "datanodes.to": "DataNodes",
    "rootz.so": "Rootz", "gofile.io": "GoFile", "mediafire.com": "MediaFire",
    "mega.nz": "MEGA", "filecrypt.cc": "Filecrypt", "filecrypt.co": "Filecrypt",
    "fileditch.com": "Fileditch", "fileditchfiles.st": "Fileditch",
}
CHALLENGES = ("cf-chl-", "challenge-platform", "verify you are human",
              "checking your browser", "attention required! | cloudflare")


def now():
    return datetime.now(timezone.utc).isoformat()


def clean(value):
    return re.sub(r"\s+", " ", value).strip()


def provider(url):
    p = urlsplit(url)
    if p.scheme != "https" or p.username or p.password:
        return None
    return PROVIDERS.get((p.hostname or "").removeprefix("www."))


class Page(HTMLParser):
    """Read text/anchors only; never execute source scripts or follow file links."""
    def __init__(self, source_url):
        super().__init__(convert_charrefs=True)
        self.source_url = source_url
        self.stack = []
        self.text = []
        self.document_title = []
        self.headings = []
        self.links = []
        self.payloads = []
        self.anchor = None
        self.h1 = None

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag not in {"br", "hr", "img", "input", "meta", "link", "source", "wbr"}:
            self.stack.append(tag)
        if attrs.get("data-payload") and len(self.payloads) < 20:
            self.payloads.append(attrs["data-payload"])
        if self.ignored():
            return
        if tag in {"p", "div", "tr", "h1", "h2", "h3", "br"}:
            self.text.append("\n")
        if tag == "h1":
            self.h1 = []
        if tag == "a":
            href = attrs.get("href", "")
            # Recent index pages assemble ordinary public links from these
            # attributes on click. Read the attributes; never execute page JS.
            split = [attrs.get(k, "") for k in ("data-d1", "data-d2", "data-d3", "data-path")]
            split_url = "".join(split) if all(split) and href in {"", "#"} else ""
            self.anchor = {"url": urljoin(self.source_url, split_url or href),
                           "linkEncoding": "split_data_attributes" if split_url else "href",
                           "parts": [], "context": clean("".join(self.text)[-600:])}

    def ignored(self):
        return any(t in self.stack for t in ("script", "style", "nav", "header", "footer"))

    def handle_data(self, data):
        if "title" in self.stack:
            self.document_title.append(data)
        if self.ignored() or "head" in self.stack:
            return
        self.text.append(data)
        if self.anchor is not None:
            self.anchor["parts"].append(data)
        if self.h1 is not None:
            self.h1.append(data)

    def handle_endtag(self, tag):
        if not self.ignored():
            if tag == "a" and self.anchor is not None:
                a = self.anchor
                a["label"] = clean("".join(a.pop("parts")))
                self.links.append(a)
                self.anchor = None
            if tag == "h1" and self.h1 is not None:
                self.headings.append(clean("".join(self.h1)))
                self.h1 = None
        if tag in self.stack:
            del self.stack[len(self.stack) - 1 - self.stack[::-1].index(tag):]


def parse_page(body, url):
    page = Page(url)
    page.feed(body)
    decoded = 0
    for payload in page.payloads[:]:
        if len(payload) > PAGE_LIMIT:
            continue
        try:
            value = base64.b64decode(payload, validate=True)
            if len(value) > PAGE_LIMIT:
                continue
            extra = Page(url)
            extra.feed(value.decode("utf-8"))
            page.links.extend(extra.links)
            page.text.extend(extra.text)
            decoded += 1
        except (ValueError, UnicodeError):
            continue
    title = next((h for h in page.headings if h), clean("".join(page.document_title)))
    options = []
    seen = set()
    for link in page.links:
        host = provider(link["url"])
        if not host or link["url"] in seen:
            continue
        seen.add(link["url"])
        options.append({**link, "provider": host,
                        "multipartHint": bool(re.search(r"\bpart\s*\d+|\.part\d+\.|\[\d+\s*/\s*\d+\]", link["label"] + " " + link["url"], re.I)),
                        "availabilityVerified": False})
    text = " ".join(page.text)
    ids = sorted(set(re.findall(r"(?:CUSA|PPSA)\d{5}", unquote(text + " " + " ".join(o["url"] for o in options)).upper())))
    missing = bool(re.search(r"\b404\b|page not found|nothing found", title, re.I)) and not options
    return {"title": title, "titleIds": ids,
            "platformHints": sorted({"ps5" if t.startswith("PPSA") else "ps4" for t in ids}),
            "options": options, "decodedPublicHtmlBlocks": decoded,
            "status": "missing" if missing else "candidate" if options else "no_provider_links",
            "needsIdentityAndFileReview": True}


class SameOriginRedirects(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        p = urlsplit(newurl)
        if p.scheme != "https" or p.hostname not in {"downloadgameps3.net", "www.downloadgameps3.net"} or p.username or p.password:
            raise ValueError("Source redirected outside its public HTTPS origin")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


class Reader:
    def __init__(self, interval=8):
        if interval < 3:
            raise ValueError("Keep at least three seconds between source requests")
        self.interval = interval
        self.last = 0
        self.opener = build_opener(SameOriginRedirects())

    def get(self, archive_id):
        url = f"{BASE}/archives/{archive_id}"
        record = {"archiveId": archive_id, "sourceUrl": url, "checkedAt": now()}
        time.sleep(max(0, self.last + self.interval - time.monotonic()))
        self.last = time.monotonic()
        request = Request(url, headers={"User-Agent": "OrbitCatalogueResearch/1.0 (public page discovery)", "Accept-Encoding": "identity", "Accept": "text/html"})
        try:
            with self.opener.open(request, timeout=25) as response:
                record["httpStatus"] = response.status
                if "text/html" not in response.headers.get("Content-Type", "").lower():
                    return {**record, "status": "unexpected_content_type", "stop": True}
                raw = response.read(PAGE_LIMIT + 1)
                if len(raw) > PAGE_LIMIT:
                    return {**record, "status": "response_limit", "stop": True}
                body = raw.decode(response.headers.get_content_charset() or "utf-8", errors="replace")
                if any(s in body.lower() for s in CHALLENGES):
                    return {**record, "status": "human_verification_required", "stop": True}
                return {**record, "finalUrl": response.url, "pageBytes": len(raw), **parse_page(body, response.url)}
        except HTTPError as exc:
            status = exc.code
            # Safe diagnostics only: no cookies, tokens or challenge payloads
            # are saved. A browser challenge is not evidence of a missing page.
            headers = {k.lower(): v for k, v in exc.headers.items()}
            diagnostics = {k: headers[k] for k in ("server", "content-type", "cf-mitigated", "retry-after") if k in headers}
            try:
                try:
                    error_body = exc.read(16_384).decode("utf-8", errors="replace").lower()
                except (OSError, TimeoutError):
                    error_body = ""
                browser_check = headers.get("cf-mitigated", "").lower() == "challenge" or any(s in error_body for s in CHALLENGES)
            finally:
                exc.close()
            if status in {404, 410}:
                return {**record, "httpStatus": status, "status": "missing"}
            state = "browser_verification_required" if browser_check else "source_refused" if status in {401, 403, 429, 503} else "http_error"
            return {**record, "httpStatus": status, "status": state, "httpDiagnostics": diagnostics, "stop": True}
        except (URLError, TimeoutError, OSError, ValueError) as exc:
            return {**record, "status": "request_failed", "errorType": type(exc).__name__, "stop": True}


class BrowserCaptureReader:
    """Parse public DOM captured by the companion through the normal browser API."""
    def __init__(self, path, scope):
        self.path = path.resolve()
        capture = json.loads(path.read_text())
        if capture.get("scope") != scope:
            raise ValueError("Browser capture scope differs from the requested range")
        self.rows = {r["archiveId"]: r for r in capture["results"]}

    def get(self, archive_id):
        row = self.rows.get(archive_id)
        if not row:
            return {"archiveId": archive_id, "status": "awaiting_browser_capture", "stop": True}
        if row["status"] != "captured":
            return dict(row)
        path = Path(row["htmlPath"]).resolve()
        if not path.is_relative_to(self.path.parent / "pages"):
            raise ValueError("Browser HTML must stay in the research pages directory")
        raw = path.read_bytes()
        if len(raw) > PAGE_LIMIT + 1_000:
            raise ValueError("Browser capture exceeds the metadata size limit")
        if row.get('sha256') and hashlib.sha256(raw).hexdigest() != row['sha256']:
            raise ValueError('Browser capture changed after it was recorded')
        if row['sourceUrl'] != f'{BASE}/archives/{archive_id}':
            raise ValueError('Browser capture URL does not match its numeric ID')
        result = parse_page(raw.decode("utf-8"), row["sourceUrl"])
        return {**{k: v for k, v in row.items() if k != "status"}, **result}


def scan(ids, reader, report, save):
    done = {r["archiveId"] for r in report["results"] if r["status"] in {"candidate", "no_provider_links", "missing"}}
    report.update(status="running", resumedAt=now())
    save(report)
    for archive_id in ids:
        if archive_id in done:
            continue
        result = reader.get(archive_id)
        report["results"] = [r for r in report["results"] if r["archiveId"] != archive_id] + [result]
        print(json.dumps({"archiveId": archive_id, "status": result["status"], "providerLinks": len(result.get("options", [])), "httpStatus": result.get("httpStatus")}), flush=True)
        report["status"] = "stopped" if result.get("stop") else "running"
        report["updatedAt"] = now()
        save(report)
        if result.get("stop"):
            return report
    report.update(status="complete", finishedAt=now())
    save(report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--start-id", type=int)
    parser.add_argument("--end-id", type=int)
    parser.add_argument("--max-pages", type=int, default=20, help="Explicit batch ceiling, 1–200 (default 20)")
    parser.add_argument("--interval", type=float, default=8, help="Seconds between pages, minimum 3")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--browser-captures", type=Path,
                        help="Use local DOM captures made with dlps_archive_scan_browser.mjs; no HTTP requests")
    args = parser.parse_args()
    if not 1 <= args.max_pages <= 200 or args.interval < 3:
        parser.error("Use positive IDs, a 1–200 page ceiling and an interval of at least three seconds")
    if args.browser_captures and args.start_id is None and args.end_id is None:
        scope = json.loads(args.browser_captures.read_text())['scope']
        if scope.get('baseUrl') != BASE:
            parser.error('Browser capture source differs from the approved numeric index')
        if 'archiveIds' in scope:
            ids = scope['archiveIds']
        else:
            step = 1 if scope['endId'] >= scope['startId'] else -1
            count = abs(scope['endId'] - scope['startId']) + 1
            if count > args.max_pages:
                parser.error('Browser capture range exceeds the page ceiling')
            ids = list(range(scope['startId'], scope['endId'] + step, step))
    else:
        if args.start_id is None or args.end_id is None or min(args.start_id, args.end_id) < 1:
            parser.error('Specify both numeric range endpoints, or use --browser-captures alone')
        count = abs(args.end_id - args.start_id) + 1
        if count > args.max_pages:
            parser.error(f'Range contains {count} pages, exceeding --max-pages {args.max_pages}')
        step = 1 if args.end_id >= args.start_id else -1
        ids = list(range(args.start_id, args.end_id + step, step))
        scope = {"baseUrl": BASE, "startId": args.start_id, "endId": args.end_id}
    if not isinstance(ids, list) or not ids or any(type(i) is not int or i < 1 for i in ids) or len(set(ids)) != len(ids):
        parser.error('Browser capture plan must contain distinct positive numeric IDs')
    count = len(ids)
    if count > args.max_pages:
        parser.error(f"Range contains {count} pages, exceeding --max-pages {args.max_pages}")
    if "catalog" in args.output.parts or args.output.suffix.lower() != ".json":
        parser.error("Write a separate research JSON report outside catalog/")
    if args.output.exists():
        if not args.resume:
            parser.error("Report already exists; use --resume to continue its exact range")
        report = json.loads(args.output.read_text())
        if report.get("scope") != scope:
            parser.error("Resume range/source must match the existing report")
    else:
        report = {"schemaVersion": 1, "startedAt": now(), "scope": scope, "results": [], "imported": False, "published": False}
    def save(value):
        args.output.parent.mkdir(parents=True, exist_ok=True)
        temporary = args.output.with_suffix(".json.tmp")
        temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")
        temporary.replace(args.output)
    reader = BrowserCaptureReader(args.browser_captures, scope) if args.browser_captures else Reader(args.interval)
    result = scan(ids, reader, report, save)
    return 0 if result["status"] == "complete" else 2


if __name__ == "__main__":
    raise SystemExit(main())
