#!/usr/bin/env python3
"""Measure frozen codec binaries on complete files and one mixed folder.

Research tooling (Python standard library, Linux /proc and GNU time).
No generated fixture, model or dependency is required by the C codec.
Source: OpenAI Codex, GPT-6; algorithm choices frozen before these runs.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import platform
import random
import shutil
import stat
import struct
import subprocess
import zipfile
import zlib
from pathlib import Path
from typing import Any

import telemetry

STAMP = (1_600_000_000_123_456_789, 1_600_000_001_987_654_321)
PROFILES = ("baseline", "rates", "sparse", "retained", "auto")


def sha256(path: Path) -> str:
    """Hash in bounded chunks without changing benchmarked codec memory."""
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def bmp_fixture(width: int = 509, height: int = 513, noise: bool = False) -> bytes:
    """Generate BMP24 with explicit padding, reserved bytes and a suffix."""
    stride = (width * 3 + 3) & ~3
    pixels = bytearray(random.Random(901).randbytes(stride * height))
    if not noise:
        for y in range(height):
            for x in range(width):
                green = (x // 2 + y // 3) & 255
                pixels[y * stride + x * 3 : y * stride + x * 3 + 3] = bytes(
                    ((green + x // 32) & 255, green, (green + y // 32) & 255)
                )
    header = b"BM" + struct.pack("<IHHI", 54 + len(pixels), 17, 29, 54)
    header += struct.pack(
        "<IiiHHIIiiII", 40, width, -height, 1, 24, 0, len(pixels), 0, 0, 0, 0
    )
    return header + pixels + b"preserved application trailer\x00\xff"


def png_fixture() -> bytes:
    """Generate a valid PNG whose IDAT is already deflated."""

    def chunk(kind: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + kind
            + data
            + struct.pack(">I", zlib.crc32(kind + data))
        )

    raw = b"".join(
        b"\0" + bytes((x + y) & 255 for x in range(512 * 3)) for y in range(256)
    )
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 512, 256, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw))
        + chunk(b"IEND", b"")
    )


def pdf_fixture() -> bytes:
    """Generate a small PDF with exact xref offsets and compressed content."""
    content = zlib.compress(
        b"BT /F1 12 Tf 50 750 Td (Byte exact document storage.) Tj ET\n" * 200
    )
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>",
        f"<< /Length {len(content)} /Filter /FlateDecode >>\nstream\n".encode()
        + content
        + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    result = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for number, obj in enumerate(objects, 1):
        offsets.append(len(result))
        result += f"{number} 0 obj\n".encode() + obj + b"\nendobj\n"
    xref = len(result)
    result += b"xref\n0 6\n0000000000 65535 f \n"
    result += b"".join(f"{offset:010d} 00000 n \n".encode() for offset in offsets[1:])
    result += f"trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode()
    return bytes(result)


def prepare_fixtures(directory: Path, quick: bool) -> list[Path]:
    """Create a fixed, disclosed matrix; never optimize against its results."""
    directory.mkdir()
    text = {
        "latin-utf8": "Armazenamento íntegro: ações, informação, saúde; Straße, façade, mañana.",
        "cyrillic-utf8": "Точное хранение документов: каждое изменение сохраняется без потери данных.",
        "cjk-utf8": "文件必须完整恢复，每个字节都相同。日本語の記録と한국어 문서를保存します。",
    }
    data: dict[str, bytes] = {}
    for name, sentence in text.items():
        data[name + ".txt"] = "".join(
            f"{i:07d}\t{sentence}\t{i * 17 % 997}\n" for i in range(8192)
        ).encode()
    data["cjk-utf16le.txt"] = ("\ufeff" + text["cjk-utf8"] * 4096).encode("utf-16le")
    data["records.csv"] = (
        "id,account,amount,region\r\n"
        + "".join(
            f'{i},"account {i % 157}",{i * 73 % 100003}.{i % 100:02d},{i % 19}\r\n'
            for i in range(16384)
        )
    ).encode()
    data["records.xml"] = (
        "<?xml version='1.0' encoding='UTF-8'?><records>"
        + "".join(
            f'<row id="{i}"><value>{i % 997}</value><text>{text["cyrillic-utf8"]}</text></row>'
            for i in range(8192)
        )
        + "</records>"
    ).encode()
    data["page.html"] = (
        "<!doctype html><html><meta charset='utf-8'><title>Archive</title><style>"
        "p{color:#123;margin:1em}</style><body>"
        + "".join(f'<p id="r{i}">{text["latin-utf8"]}</p>' for i in range(8192))
        + "</body></html>"
    ).encode()
    data["style.css"] = "\n".join(
        f".item-{i}{{color:#{i % 0xFFFFFF:06x};margin:{i % 31}px}}" for i in range(8192)
    ).encode()
    data["source.c"] = (telemetry.ROOT / "adpat/c/adapt_model.h").read_bytes()
    data["binary.elf"] = (telemetry.ROOT / "adpat/c/build/cadapt").read_bytes()
    data["raw-bits.bin"] = random.Random(314159).randbytes(1 << 20)
    data["empty.bin"] = b""
    data["gradient.bmp"] = bmp_fixture()
    data["noise.bmp"] = bmp_fixture(noise=True)
    data["image.png"] = png_fixture()
    data["document.pdf"] = pdf_fixture()
    zipped = io.BytesIO()
    with zipfile.ZipFile(zipped, "w") as archive:
        for name, payload in {
            "mimetype": b"application/epub+zip",
            "META-INF/container.xml": b'<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0"><rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>',
            "content.opf": b'<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Byte records</dc:title><dc:language>en</dc:language><dc:identifier id="id">fixture</dc:identifier></metadata><manifest><item id="page" href="page.xhtml" media-type="application/xhtml+xml"/><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/></manifest><spine toc="ncx"><itemref idref="page"/></spine></package>',
            "toc.ncx": b'<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head><meta name="dtb:uid" content="fixture"/></head><docTitle><text>Byte records</text></docTitle><navMap><navPoint id="p1" playOrder="1"><navLabel><text>Records</text></navLabel><content src="page.xhtml"/></navPoint></navMap></ncx>',
            "page.xhtml": (
                '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Records</title></head><body><p>'
                + text["latin-utf8"] * 4096
                + "</p></body></html>"
            ).encode(),
        }.items():
            info = zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0))
            info.compress_type = (
                zipfile.ZIP_STORED if name == "mimetype" else zipfile.ZIP_DEFLATED
            )
            archive.writestr(info, payload)
    data["book.epub"] = zipped.getvalue()
    data["recurring-regimes.bin"] = b"".join(
        data[name]
        for name in (
            "records.csv",
            "cjk-utf8.txt",
            "records.xml",
            "records.csv",
            "cjk-utf8.txt",
            "records.xml",
        )
    )
    for name, payload in data.items():
        (directory / name).write_bytes(payload)
    enwik = directory / ("enwik8-prefix-65536" if quick else "enwik8")
    with (
        zipfile.ZipFile(telemetry.ROOT / "static/corpus/enwik8.zip") as archive,
        archive.open("enwik8") as source,
    ):
        with enwik.open("wb") as destination:
            if quick:
                destination.write(source.read(65536))
            else:
                shutil.copyfileobj(source, destination, 1 << 20)
    folder = directory / "mixed-folder"
    (folder / "nested" / "empty-directory").mkdir(parents=True)
    for name in (
        "records.csv",
        "source.c",
        "cyrillic-utf8.txt",
        "image.png",
        "document.pdf",
        "book.epub",
    ):
        shutil.copyfile(directory / name, folder / "nested" / name)
    return [enwik, *[directory / name for name in data], folder]


def manifest(path: Path) -> dict[str, Any]:
    """Record contents and reset all metadata before each encode."""
    paths = [path, *sorted(path.rglob("*"))] if path.is_dir() else [path]
    entries = {}
    for entry in paths:
        is_dir = entry.is_dir()
        entries[str(entry.relative_to(path))] = {
            "directory": is_dir,
            "bytes": 0 if is_dir else entry.stat().st_size,
            "sha256": None if is_dir else sha256(entry),
            "mode": 0o751 if is_dir else 0o640,
            "atime_ns": STAMP[0],
            "mtime_ns": STAMP[1],
        }
    return entries


def reset_metadata(path: Path, entries: dict[str, Any]) -> None:
    """Make each candidate encode the same filesystem state."""
    for name, entry in entries.items():
        target = path / name if name != "." else path
        os.chmod(target, entry["mode"])
        os.utime(target, ns=STAMP)


def compare_restoration(
    source: Path, restored: Path, entries: dict[str, Any]
) -> dict[str, bool]:
    """Compare metadata before reading files, then compare every byte."""
    metadata = True
    for name, expected in entries.items():
        target = restored / name if name != "." else restored
        actual = target.stat()
        metadata &= (
            stat.S_IMODE(actual.st_mode),
            actual.st_atime_ns,
            actual.st_mtime_ns,
        ) == (expected["mode"], expected["atime_ns"], expected["mtime_ns"])
    namespace = (
        {".", *map(lambda p: str(p.relative_to(restored)), restored.rglob("*"))}
        if restored.is_dir()
        else {"."}
    )
    byte_exact = namespace == set(entries)
    for name, expected in entries.items():
        if expected["directory"]:
            continue
        left = source / name if name != "." else source
        right = restored / name if name != "." else restored
        with left.open("rb") as original, right.open("rb") as recovered:
            while True:
                a, b = original.read(1 << 20), recovered.read(1 << 20)
                byte_exact &= a == b
                if not a and not b:
                    break
    return {"byte_exact": byte_exact, "mode_and_ns_timestamps_exact": metadata}


def main(argv: list[str] | None = None) -> int:
    """Run a finite matrix and checkpoint both successful and failed trials."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--baseline-revision", default="unspecified")
    parser.add_argument(
        "--binary", type=Path, default=telemetry.ROOT / "adpat/c/build/cadapt"
    )
    parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help="new directory; never reuses old measurements",
    )
    parser.add_argument("--memory", type=int, default=64 << 20)
    parser.add_argument(
        "--quick",
        action="store_true",
        help="replace full enwik8 by its first 65536 bytes",
    )
    parser.add_argument(
        "--fixtures",
        nargs="+",
        help="select fixture names, without changing their contents",
    )
    args = parser.parse_args(argv)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    binary, baseline = output / "candidate-cadapt", output / "baseline-cadapt"
    shutil.copy2(args.binary, binary)
    shutil.copy2(args.baseline, baseline)
    fixtures = prepare_fixtures(output / "fixtures", args.quick)
    if args.fixtures:
        fixtures = [path for path in fixtures if path.name in args.fixtures]
        if {p.name for p in fixtures} != set(args.fixtures):
            parser.error("unknown fixture name")
    report: dict[str, Any] = {
        "schema": "cadapt.causal-experiment.v1",
        "generated_by": "OpenAI Codex GPT-6",
        "started_at": telemetry.utc_now(),
        "complete": False,
        "quick": args.quick,
        "memory_budget_bytes": args.memory,
        "host": platform.platform(),
        "baseline_sha256": sha256(baseline),
        "candidate_sha256": sha256(binary),
        "runner_sha256": sha256(Path(__file__)),
        "telemetry_sha256": sha256(Path(telemetry.__file__)),
        "baseline_revision": args.baseline_revision,
        "candidate_git_head": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=telemetry.ROOT, text=True
        ).strip(),
        "expected_runs": sum(
            9 if source.suffix == ".bmp" else 5 for source in fixtures
        ),
        "fixtures": {},
        "runs": [],
    }
    for source in fixtures:
        entries = manifest(source)
        report["fixtures"][source.name] = entries
        profiles = PROFILES + (
            ("normal-bmp", "rates-bmp", "sparse-bmp", "retained-bmp")
            if source.suffix == ".bmp"
            else ()
        )
        for profile in profiles:
            directory = output / f"{source.name}-{profile}"
            directory.mkdir()
            encoded, restored = directory / "encoded.adpt", directory / "restored"
            reset_metadata(source, entries)
            executable = baseline if profile == "baseline" else binary
            command = [
                str(executable),
                "encode",
                str(source),
                "-o",
                str(encoded),
                "--max-memory",
                str(args.memory),
            ]
            mode = profile.removesuffix("-bmp")
            if mode not in ("baseline", "normal"):
                command += ["--adaptive", mode]
            if profile.endswith("-bmp"):
                command += ["--transform", "bmp"]
            record: dict[str, Any] = {
                "fixture": source.name,
                "profile": profile,
                "encode_command": command,
            }
            report["runs"].append(record)
            telemetry.atomic_json(output / "results.json", report)
            try:
                record["encode"] = telemetry.measure(
                    command,
                    directory,
                    source,
                    encoded,
                    0.1,
                    3600,
                    Path("/usr/bin/time"),
                )
                if record["encode"]["status"] == "ok":
                    record["container_bytes"] = encoded.stat().st_size
                    record["container_sha256"] = sha256(encoded)
                    record["codec"] = telemetry.inspect_container(encoded)
                    record["selection_log"] = (directory / "stderr.txt").read_text()
                    decode_dir = directory / "decode"
                    decode_dir.mkdir()
                    command = [str(binary), "decode", str(encoded), "-o", str(restored)]
                    record["decode_command"] = command
                    record["decode"] = telemetry.measure(
                        command,
                        decode_dir,
                        encoded,
                        restored,
                        0.1,
                        3600,
                        Path("/usr/bin/time"),
                    )
                    if record["decode"]["status"] == "ok":
                        record["restoration"] = compare_restoration(
                            source, restored, entries
                        )
            except (OSError, ValueError, KeyError) as error:
                record["error"] = str(error)
            telemetry.atomic_json(output / "results.json", report)
            print(
                json.dumps(
                    {
                        k: record.get(k)
                        for k in (
                            "fixture",
                            "profile",
                            "container_bytes",
                            "restoration",
                        )
                    }
                ),
                flush=True,
            )
    comparisons = []
    for source in fixtures:
        rows = [r for r in report["runs"] if r["fixture"] == source.name]
        successful = [r for r in rows if "container_bytes" in r and "error" not in r]
        if len(successful) == len(rows):
            base = next(
                r["container_bytes"] for r in rows if r["profile"] == "baseline"
            )
            chosen = next(r["container_bytes"] for r in rows if r["profile"] == "auto")
            comparisons.append(
                {
                    "fixture": source.name,
                    "baseline_bytes": base,
                    "auto_bytes": chosen,
                    "saved_percent": 100 * (base - chosen) / base,
                    "auto_equals_smallest": chosen
                    == min(r["container_bytes"] for r in rows),
                }
            )
    report["comparisons"] = comparisons
    report["complete"] = len(report["runs"]) == report["expected_runs"] and all(
        "error" not in r
        and r.get("encode", {}).get("status") == "ok"
        and r.get("decode", {}).get("status") == "ok"
        and all(r.get("restoration", {}).values())
        and bool(r.get("restoration"))
        for r in report["runs"]
    )
    report["complete"] &= len(comparisons) == len(fixtures) and all(
        c["auto_equals_smallest"] for c in comparisons
    )
    report["finished_at"] = telemetry.utc_now()
    telemetry.atomic_json(output / "results.json", report)
    return 0 if report["complete"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
