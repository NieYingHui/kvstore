#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
import csv
import os
import statistics
from collections import defaultdict


def read_rows(path: str) -> list[dict[str, str]]:
    with open(path, "r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        rows = list(reader)
    if not rows:
        raise SystemExit(f"empty csv: {path}")
    keys = set(rows[0].keys())
    # Accept both new schema (MiB_per_s) and legacy schema (MiBps).
    required_base = {"op", "method"}
    missing = required_base - keys
    if missing:
        raise SystemExit(f"csv missing columns: {sorted(missing)}")
    if ("MiB_per_s" not in keys) and ("MiBps" not in keys):
        raise SystemExit("csv missing throughput column: need MiB_per_s or MiBps")
    return rows


def fnum(s: str) -> float:
    try:
        return float(s)
    except Exception:
        return 0.0


def inum(s: str) -> int:
    try:
        return int(float(s))
    except Exception:
        return 0


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


def ensure_out_dir(path: str) -> None:
    os.makedirs(path, exist_ok=True)


def main() -> int:
    ap = argparse.ArgumentParser(description="Plot mmap vs fread/fwrite I/O benchmark results")
    ap.add_argument("csv", help="CSV produced by bench/persist_io_bench.py")
    ap.add_argument("--out", default="bench/plots/io_compare.png", help="Output image path")
    ap.add_argument("--format", default=None, choices=[None, "png", "svg"], help="Override output format")
    ap.add_argument(
        "--ops",
        default="read_rand",
        help="Comma-separated ops order (default: read_rand)",
    )
    ap.add_argument(
        "--methods",
        default="mmap,pread",
        help="Comma-separated method order (default: mmap,pread)",
    )
    args = ap.parse_args()

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:
        raise SystemExit(
            "matplotlib not available. Install it first, e.g.\n"
            "  python3 -m pip install --user matplotlib\n"
            f"Original error: {e}"
        )

    rows = read_rows(args.csv)

    ops = [o.strip() for o in args.ops.split(",") if o.strip()]
    methods = [m.strip() for m in args.methods.split(",") if m.strip()]

    # group: (op, method) -> list[MiB/s]
    g: dict[tuple[str, str], list[float]] = defaultdict(list)
    meta = {"chunk_bytes": 0, "rand_ops": 0, "rand_block": 0, "sync": ""}
    for r in rows:
        op = (r.get("op") or "").strip()
        method = (r.get("method") or "").strip()
        mib = r.get("MiB_per_s")
        if mib is None or mib == "":
            mib = r.get("MiBps")
        g[(op, method)].append(fnum(mib or "0"))
        meta["chunk_bytes"] = max(meta["chunk_bytes"], inum(r.get("chunk_bytes") or r.get("block") or "0"))
        meta["rand_ops"] = max(meta["rand_ops"], inum(r.get("rand_ops") or "0"))
        meta["rand_block"] = max(meta["rand_block"], inum(r.get("rand_block") or r.get("block") or "0"))
        if not meta["sync"]:
            meta["sync"] = (r.get("sync") or "").strip()

    ensure_out_dir(os.path.dirname(args.out) or ".")

    # Prepare plot data: each op is a subplot
    n_ops = len(ops)
    fig, axes = plt.subplots(1, n_ops, figsize=(6 * n_ops, 4), constrained_layout=True)
    if n_ops == 1:
        axes = [axes]

    for ax, op in zip(axes, ops):
        present = [m for m in methods if (op, m) in g]
        if not present:
            ax.axis("off")
            continue

        meds = [statistics.median(g[(op, m)]) for m in present]
        p10 = [percentile(g[(op, m)], 0.10) for m in present]
        p90 = [percentile(g[(op, m)], 0.90) for m in present]
        yerr = [[max(0.0, meds[i] - p10[i]) for i in range(len(present))], [max(0.0, p90[i] - meds[i]) for i in range(len(present))]]

        xs = list(range(len(present)))
        bars = ax.bar(xs, meds, yerr=yerr, capsize=4)
        ax.set_xticks(xs, present)
        ax.set_ylabel("MiB/s (median, p10-p90)")

        title = op
        if op == "read_seq":
            title += f"\nchunk={meta['chunk_bytes'] / (1024 * 1024):.0f}MiB"
        elif op == "read_rand":
            title += f"\nops={meta['rand_ops']}, block={meta['rand_block'] / 1024:.0f}KiB"
        elif op == "write":
            title += f"\nsync={meta['sync'] or 'none'}"
        ax.set_title(title)
        ax.grid(axis="y", linestyle="--", linewidth=0.5, alpha=0.6)

        ymax = max(meds) if meds else 0.0
        if ymax > 0:
            ax.set_ylim(0, ymax * 1.25)

        for rect, y in zip(bars, meds):
            ax.text(rect.get_x() + rect.get_width() / 2, rect.get_height(), f"{y:.1f}", ha="center", va="bottom", fontsize=9)

    out = args.out
    if args.format:
        root, _ = os.path.splitext(out)
        out = f"{root}.{args.format}"

    fig.suptitle("mmap vs buffered I/O (fread/pread/fwrite)")
    fig.savefig(out, dpi=160)
    print(f"plot: {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
