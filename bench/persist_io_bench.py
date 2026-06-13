#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
import csv
import mmap
import os
import random
import re
import statistics
import time
import zlib
from dataclasses import dataclass
from pathlib import Path


_SIZE_RE = re.compile(r"^\s*(\d+(?:\.\d+)?)\s*([KMGTP]?i?B?)?\s*$", re.IGNORECASE)


def parse_size(s: str) -> int:
    """Parse human size like 64K, 4MiB, 1G into bytes."""
    m = _SIZE_RE.match(s or "")
    if not m:
        raise SystemExit(f"invalid size: {s!r} (examples: 4096, 64K, 4MiB, 1G)")
    num = float(m.group(1))
    suf = (m.group(2) or "").strip().lower()

    if suf in {"", "b"}:
        mult = 1
    elif suf in {"k", "kb"}:
        mult = 1000
    elif suf in {"m", "mb"}:
        mult = 1000**2
    elif suf in {"g", "gb"}:
        mult = 1000**3
    elif suf in {"t", "tb"}:
        mult = 1000**4
    elif suf in {"ki", "kib"}:
        mult = 1024
    elif suf in {"mi", "mib"}:
        mult = 1024**2
    elif suf in {"gi", "gib"}:
        mult = 1024**3
    elif suf in {"ti", "tib"}:
        mult = 1024**4
    else:
        raise SystemExit(f"invalid size suffix: {s!r}")

    out = int(num * mult)
    if out <= 0:
        raise SystemExit(f"size must be > 0: {s!r}")
    return out


def ensure_parent(p: Path) -> None:
    p.parent.mkdir(parents=True, exist_ok=True)


def percentile(values: list[float], q: float) -> float:
    if not values:
        return 0.0
    vs = sorted(values)
    if len(vs) == 1:
        return vs[0]
    pos = (len(vs) - 1) * q
    lo = int(pos)
    hi = min(lo + 1, len(vs) - 1)
    frac = pos - lo
    return vs[lo] * (1.0 - frac) + vs[hi] * frac


@dataclass
class Result:
    op: str  # read_seq/read_rand/write
    method: str  # mmap/fread/fwrite
    size_bytes: int
    chunk_bytes: int
    rand_ops: int
    rand_block: int
    sync: str
    iter_idx: int
    seconds: float
    mib_per_s: float
    checksum: int
    path: str


def checksum_update(crc: int, data: memoryview | bytes) -> int:
    # adler32 is fast in zlib C code
    return zlib.adler32(data, crc)


def run_read_seq_mmap(path: Path, chunk_bytes: int) -> tuple[int, int]:
    fd = os.open(path, os.O_RDONLY)
    try:
        st = os.fstat(fd)
        size = int(st.st_size)
        if size <= 0:
            return 0, 1
        mm = mmap.mmap(fd, 0, access=mmap.ACCESS_READ)
        mv = memoryview(mm)
        try:
            crc = 1
            # chunked scan to keep peak RSS stable
            for off in range(0, size, chunk_bytes):
                view = mv[off : off + chunk_bytes]
                try:
                    crc = checksum_update(crc, view)
                finally:
                    # Ensure no exported pointers remain before mm.close().
                    view.release()
            return size, crc
        finally:
            mv.release()
            mm.close()
    finally:
        os.close(fd)


def run_read_seq_fread(path: Path, chunk_bytes: int) -> tuple[int, int]:
    size = 0
    crc = 1
    with open(path, "rb", buffering=0) as f:
        while True:
            b = f.read(chunk_bytes)
            if not b:
                break
            size += len(b)
            crc = checksum_update(crc, b)
    return size, crc


def run_read_rand_mmap(path: Path, rand_ops: int, rand_block: int, seed: int) -> tuple[int, int]:
    fd = os.open(path, os.O_RDONLY)
    try:
        st = os.fstat(fd)
        size = int(st.st_size)
        if size <= 0:
            return 0, 1
        mm = mmap.mmap(fd, 0, access=mmap.ACCESS_READ)
        mv = memoryview(mm)
        try:
            rng = random.Random(seed)
            crc = 1
            max_off = max(0, size - rand_block)
            for _ in range(rand_ops):
                off = rng.randrange(0, max_off + 1) if max_off > 0 else 0
                view = mv[off : off + rand_block]
                try:
                    crc = checksum_update(crc, view)
                finally:
                    view.release()
            return rand_ops * rand_block, crc
        finally:
            mv.release()
            mm.close()
    finally:
        os.close(fd)


def run_read_rand_pread(path: Path, rand_ops: int, rand_block: int, seed: int) -> tuple[int, int]:
    fd = os.open(path, os.O_RDONLY)
    try:
        st = os.fstat(fd)
        size = int(st.st_size)
        if size <= 0:
            return 0, 1
        rng = random.Random(seed)
        crc = 1
        max_off = max(0, size - rand_block)
        for _ in range(rand_ops):
            off = rng.randrange(0, max_off + 1) if max_off > 0 else 0
            b = os.pread(fd, rand_block, off)
            crc = checksum_update(crc, b)
        return rand_ops * rand_block, crc
    finally:
        os.close(fd)


def run_write_fwrite(dst: Path, data: bytes, sync: str) -> int:
    ensure_parent(dst)
    with open(dst, "wb", buffering=0) as f:
        f.write(data)
        f.flush()
        if sync == "fsync":
            os.fsync(f.fileno())
    return zlib.adler32(data, 1)


def run_write_mmap(dst: Path, data: bytes, sync: str) -> int:
    ensure_parent(dst)
    fd = os.open(dst, os.O_CREAT | os.O_TRUNC | os.O_RDWR, 0o644)
    try:
        os.ftruncate(fd, len(data))
        mm = mmap.mmap(fd, len(data), access=mmap.ACCESS_WRITE)
        try:
            mm[:] = data
            if sync in {"msync", "msync+fsync"}:
                mm.flush()
        finally:
            mm.close()
        if sync in {"fsync", "msync+fsync"}:
            os.fsync(fd)
        return zlib.adler32(data, 1)
    finally:
        os.close(fd)


def maybe_run_cmd(cmd: str | None) -> None:
    if not cmd:
        return
    rc = os.system(cmd)
    if rc != 0:
        raise SystemExit(f"drop-caches command failed (rc={rc}): {cmd}")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=(
            "Compare persistence I/O performance: mmap vs fread/fwrite. "
            "Outputs CSV and can be plotted by bench/plot_io_compare.py"
        )
    )
    ap.add_argument("--read-file", help="File to benchmark read performance (e.g., log/dump.rdb or log/appendonly.aof)")
    ap.add_argument(
        "--write-size",
        default="256MiB",
        help="Write benchmark payload size (default: 256MiB; examples: 64MiB, 1GiB)",
    )
    ap.add_argument(
        "--write-from-file",
        help="Use bytes from this file as the write payload (overrides --write-size)",
    )
    ap.add_argument("--out", default="bench/out/io_compare.csv", help="Output CSV path")
    ap.add_argument("--iters", type=int, default=5, help="Iterations per method")
    ap.add_argument("--chunk", default="8MiB", help="Chunk size for sequential scan / fread loop")
    ap.add_argument("--rand-ops", type=int, default=20000, help="Random read operations (only for read_rand)")
    ap.add_argument("--rand-block", default="4KiB", help="Bytes per random read")
    ap.add_argument(
        "--sync",
        default="none",
        choices=["none", "fsync", "msync", "msync+fsync"],
        help="Sync policy for write benchmark",
    )
    ap.add_argument(
        "--workloads",
        default="read_seq,read_rand,write",
        help="Comma-separated: read_seq,read_rand,write",
    )
    ap.add_argument(
        "--drop-caches-cmd",
        default=None,
        help=(
            "Optional command to drop OS page cache between runs (needs root on Linux). "
            'Example: --drop-caches-cmd "sudo sh -c \'sync; echo 3 > /proc/sys/vm/drop_caches\'"'
        ),
    )
    ap.add_argument("--tmp-dir", default="bench/out/io_tmp", help="Temp dir for write outputs")
    ap.add_argument("--seed", type=int, default=12345, help="Random seed")
    args = ap.parse_args()

    chunk_bytes = parse_size(args.chunk)
    rand_block = parse_size(args.rand_block)

    workloads = [w.strip() for w in (args.workloads or "").split(",") if w.strip()]
    if not workloads:
        raise SystemExit("no workloads selected")

    read_file = Path(args.read_file) if args.read_file else None
    if any(w.startswith("read_") for w in workloads) and not read_file:
        raise SystemExit("--read-file is required for read_seq/read_rand")

    out_path = Path(args.out)
    ensure_parent(out_path)

    # Prepare write payload
    if args.write_from_file:
        payload = Path(args.write_from_file).read_bytes()
    else:
        n = parse_size(args.write_size)
        # deterministic pseudo-random bytes; faster than os.urandom for large sizes
        rng = random.Random(args.seed)
        payload = bytearray(n)
        for i in range(n):
            payload[i] = rng.getrandbits(8)
        payload = bytes(payload)

    tmp_dir = Path(args.tmp_dir)
    tmp_dir.mkdir(parents=True, exist_ok=True)

    results: list[Result] = []

    def record(op: str, method: str, size_bytes: int, seconds: float, checksum: int, path: str):
        mib_per_s = (size_bytes / (1024 * 1024)) / seconds if seconds > 0 else 0.0
        results.append(
            Result(
                op=op,
                method=method,
                size_bytes=size_bytes,
                chunk_bytes=chunk_bytes,
                rand_ops=int(args.rand_ops),
                rand_block=rand_block,
                sync=args.sync,
                iter_idx=len([r for r in results if r.op == op and r.method == method]) + 1,
                seconds=seconds,
                mib_per_s=mib_per_s,
                checksum=checksum,
                path=path,
            )
        )

    # ---- read_seq ----
    if "read_seq" in workloads and read_file:
        for i in range(args.iters):
            maybe_run_cmd(args.drop_caches_cmd)
            t0 = time.perf_counter()
            size, crc = run_read_seq_mmap(read_file, chunk_bytes)
            t1 = time.perf_counter()
            record("read_seq", "mmap", size, t1 - t0, crc, str(read_file))

            maybe_run_cmd(args.drop_caches_cmd)
            t0 = time.perf_counter()
            size2, crc2 = run_read_seq_fread(read_file, chunk_bytes)
            t1 = time.perf_counter()
            record("read_seq", "fread", size2, t1 - t0, crc2, str(read_file))

    # ---- read_rand ----
    if "read_rand" in workloads and read_file:
        for i in range(args.iters):
            seed = args.seed + i

            maybe_run_cmd(args.drop_caches_cmd)
            t0 = time.perf_counter()
            size, crc = run_read_rand_mmap(read_file, int(args.rand_ops), rand_block, seed)
            t1 = time.perf_counter()
            record("read_rand", "mmap", size, t1 - t0, crc, str(read_file))

            maybe_run_cmd(args.drop_caches_cmd)
            t0 = time.perf_counter()
            size2, crc2 = run_read_rand_pread(read_file, int(args.rand_ops), rand_block, seed)
            t1 = time.perf_counter()
            record("read_rand", "pread", size2, t1 - t0, crc2, str(read_file))

    # ---- write ----
    if "write" in workloads:
        for i in range(args.iters):
            dst1 = tmp_dir / f"write_mmap_{i}.bin"
            dst2 = tmp_dir / f"write_fwrite_{i}.bin"

            # remove old
            for p in (dst1, dst2):
                try:
                    p.unlink()
                except FileNotFoundError:
                    pass

            t0 = time.perf_counter()
            crc = run_write_mmap(dst1, payload, args.sync)
            t1 = time.perf_counter()
            record("write", "mmap", len(payload), t1 - t0, crc, str(dst1))

            t0 = time.perf_counter()
            crc2 = run_write_fwrite(dst2, payload, "fsync" if args.sync == "fsync" else "none")
            # Note: fwrite path doesn't have an msync analogue; we keep fsync option parity.
            t1 = time.perf_counter()
            record("write", "fwrite", len(payload), t1 - t0, crc2, str(dst2))

    # Write CSV
    with open(out_path, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        # Keep a stable schema for our plotter, and add legacy/alias columns so older plot scripts
        # (expecting MiBps/bytes/block) can consume the same CSV.
        w.writerow(
            [
                "op",
                "method",
                "size_bytes",
                "chunk_bytes",
                "rand_ops",
                "rand_block",
                "sync",
                "iter",
                "seconds",
                "MiB_per_s",
                "checksum",
                "path",
                # aliases
                "bytes",
                "block",
                "MiBps",
            ]
        )
        for r in results:
            block = r.rand_block if r.op == "read_rand" else r.chunk_bytes
            w.writerow(
                [
                    r.op,
                    r.method,
                    r.size_bytes,
                    r.chunk_bytes,
                    r.rand_ops,
                    r.rand_block,
                    r.sync,
                    r.iter_idx,
                    f"{r.seconds:.9f}",
                    f"{r.mib_per_s:.3f}",
                    r.checksum,
                    r.path,
                    # aliases
                    r.size_bytes,
                    block,
                    f"{r.mib_per_s:.3f}",
                ]
            )

    # Print a small summary (median)
    groups: dict[tuple[str, str], list[float]] = {}
    for r in results:
        groups.setdefault((r.op, r.method), []).append(r.mib_per_s)
    for (op, method), vals in sorted(groups.items()):
        med = statistics.median(vals)
        p10 = percentile(vals, 0.10)
        p90 = percentile(vals, 0.90)
        print(f"[{op}] {method}: median={med:.2f} MiB/s  p10={p10:.2f}  p90={p90:.2f}")

    print(f"csv: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
