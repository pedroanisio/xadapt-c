#!/usr/bin/env python3
"""Fetch the multi-script corpus rows FMT-8 measures, reproducibly.

Each row is one UTF-8 text file of Wikipedia article text in one script,
zipped like the other fixtures: Latin (English), Cyrillic (Russian), CJK
(Chinese, Japanese) and diacritic-heavy Latin (Vietnamese, Czech). Article
text is CC BY-SA 4.0, the licence enwik8 carries too; the manifest names
every article and its revision so a row can be traced or fetched again.

Selection is deterministic: the first articles, in title order, that
Wikipedia's `allpages` lists as non-redirects above a size floor, their
plain-text extracts concatenated until the row reaches its byte target.
Content still drifts with edits; the manifest's hashes pin what was
measured.

    python3 bench/fetch_scripts.py            # every row into static/corpus/scripts/
    python3 bench/fetch_scripts.py --rows cyrillic-ru --target 1MiB
    python3 bench/fetch_scripts.py --from-manifest static/corpus/scripts/manifest.json

Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-14,
with Claude Opus 5 (Claude Code).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
import zlib
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DESTINATION = ROOT / "static" / "corpus" / "scripts"
USER_AGENT = "cadapt-bench/1.0 (https://github.com/pedroanisio; FMT-8 corpus rows)"
MIB = 1 << 20

# row -> (wiki, script, the first title to list from, the wikitext size floor)
ROWS: dict[str, tuple[str, str, str, int]] = {
    "latin-en": ("en", "Latin", "A", 40000),
    "cyrillic-ru": ("ru", "Cyrillic", "А", 40000),
    "cjk-zh": ("zh", "CJK (Han)", "一", 20000),
    "cjk-ja": ("ja", "CJK (kana, kanji)", "あ", 20000),
    "diacritics-vi": ("vi", "Latin with diacritics", "A", 30000),
    "diacritics-cs": ("cs", "Latin with diacritics", "A", 30000),
}


def byte_count(value: str) -> int:
    units = {"KiB": 1 << 10, "MiB": 1 << 20, "GiB": 1 << 30}
    for suffix, factor in units.items():
        if value.endswith(suffix):
            return int(value[: -len(suffix)]) * factor
    return int(value)


def api(wiki: str, params: dict[str, str], attempts: int = 5) -> dict:
    """One MediaWiki API call, JSON, retried with backoff on failure."""
    query = urllib.parse.urlencode({**params, "format": "json", "formatversion": "2", "maxlag": "5"})
    url = f"https://{wiki}.wikipedia.org/w/api.php?{query}"
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    for attempt in range(attempts):
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                payload = json.loads(response.read().decode("utf-8"))
            if "error" in payload:
                if payload["error"].get("code") == "maxlag":
                    time.sleep(2 ** attempt)
                    continue
                raise RuntimeError(f"{wiki}: {payload['error']}")
            return payload
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
            if attempt == attempts - 1:
                raise RuntimeError(f"{wiki}: {error}") from error
            time.sleep(2 ** attempt)
    raise RuntimeError(f"{wiki}: gave up")


def list_titles(wiki: str, start: str, floor: int, count: int) -> list[str]:
    """The first `count` non-redirect article titles from `start`, above the floor."""
    titles: list[str] = []
    params = {
        "action": "query",
        "list": "allpages",
        "apnamespace": "0",
        "apfilterredir": "nonredirects",
        "apminsize": str(floor),
        "aplimit": "500",
        "apfrom": start,
    }
    while len(titles) < count:
        payload = api(wiki, params)
        titles.extend(page["title"] for page in payload["query"]["allpages"])
        if "continue" not in payload:
            break
        params.update(payload["continue"])
    return titles[:count]


def extract(wiki: str, title: str) -> tuple[str, int]:
    """An article's plain text and revision id."""
    payload = api(
        wiki,
        {
            "action": "query",
            "prop": "extracts|revisions",
            "explaintext": "1",
            "exlimit": "1",
            "rvprop": "ids",
            "titles": title,
        },
    )
    page = payload["query"]["pages"][0]
    if page.get("missing"):
        return "", 0
    return page.get("extract", ""), page.get("revisions", [{}])[0].get("revid", 0)


def fetch_row(name: str, target: int, titles: list[str] | None, pause: float) -> dict:
    wiki, script, start, floor = ROWS[name]
    chosen = titles if titles is not None else list_titles(wiki, start, floor, 2000)
    parts: list[str] = []
    articles: list[dict] = []
    size = 0
    for title in chosen:
        text, revision = extract(wiki, title)
        if not text.strip():
            continue
        body = f"= {title} =\n\n{text.strip()}\n\n"
        parts.append(body)
        articles.append({"title": title, "revision": revision, "bytes": len(body.encode("utf-8"))})
        size += articles[-1]["bytes"]
        print(f"  {name}: {len(articles):4d} articles, {size / MIB:6.2f} MiB, last: {title}", file=sys.stderr)
        if size >= target and titles is None:
            break
        time.sleep(pause)
    text = "".join(parts)
    data = text.encode("utf-8")
    non_ascii = sum(1 for b in data if b >= 0x80)
    return {
        "row": name,
        "wiki": f"{wiki}.wikipedia.org",
        "script": script,
        "licence": "CC BY-SA 4.0 (Wikipedia article text; authors per article history)",
        "bytes": len(data),
        "non_ascii_bytes": non_ascii,
        "articles": articles,
        "sha256": hashlib.sha256(data).hexdigest(),
        "crc32": f"{zlib.crc32(data) & 0xFFFFFFFF:08x}",
        "_data": data,
    }


def write_row(destination: Path, row: dict) -> Path:
    destination.mkdir(parents=True, exist_ok=True)
    path = destination / f"{row['row']}.zip"
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        info = zipfile.ZipInfo(f"{row['row']}.txt", date_time=(2026, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        archive.writestr(info, row["_data"])
    return path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rows", nargs="+", choices=sorted(ROWS), default=sorted(ROWS))
    parser.add_argument("--target", type=byte_count, default=2 * MIB, help="bytes per row (default 2MiB)")
    parser.add_argument("--destination", type=Path, default=DESTINATION)
    parser.add_argument("--from-manifest", type=Path, help="fetch the titles a manifest names, not a fresh selection")
    parser.add_argument("--pause", type=float, default=0.1, help="seconds between article requests")
    args = parser.parse_args(argv)

    titles_by_row: dict[str, list[str]] = {}
    if args.from_manifest is not None:
        manifest = json.loads(args.from_manifest.read_text(encoding="utf-8"))
        titles_by_row = {row["row"]: [a["title"] for a in row["articles"]] for row in manifest["rows"]}

    rows = []
    for name in args.rows:
        print(f"{name}: fetching from {ROWS[name][0]}.wikipedia.org", file=sys.stderr)
        row = fetch_row(name, args.target, titles_by_row.get(name), args.pause)
        path = write_row(args.destination, row)
        row["file"] = str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path)
        row["member"] = f"{name}.txt"
        del row["_data"]
        rows.append(row)
        print(f"{name}: {row['bytes']} bytes, {len(row['articles'])} articles -> {path}", file=sys.stderr)

    manifest_path = args.destination / "manifest.json"
    previous = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {"rows": []}
    kept = [row for row in previous["rows"] if row["row"] not in {r["row"] for r in rows}]
    manifest = {
        "schema": "cadapt.corpus.scripts.v1",
        "fetched_at": datetime.now(timezone.utc).isoformat(),
        "selection": "allpages, namespace 0, non-redirects, above the size floor, from the row's start title, in title order",
        "rows": sorted(kept + rows, key=lambda r: r["row"]),
    }
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"manifest: {manifest_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
