#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
import csv
import math
import os
from collections import defaultdict


def read_rows(path: str) -> list[dict[str, str]]:
    with open(path, "r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        rows = list(reader)
    if not rows:
        raise SystemExit(f"empty csv: {path}")
    required = {
        "mode",
        "struct",
        "phase",
        "count",
        "value_size",
        "cmds_per_s",
        "time_ms",
        "sent_bytes",
        "VmRSS_kB",
        "VmHWM_kB",
    }
    missing = required - set(rows[0].keys())
    if missing:
        raise SystemExit(f"csv missing columns: {sorted(missing)}")
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


def ensure_out_dir(path: str) -> None:
    os.makedirs(path, exist_ok=True)


def _format_bar_value(y: float, ylabel: str) -> str:
    if ylabel.lower() in {"mib", "mb"}:
        return f"{y:.2f}"
    # cmds/s or other rates
    if abs(y) >= 1_000_000:
        return f"{y/1_000_000:.2f}M"
    if abs(y) >= 1_000:
        return f"{y/1_000:.2f}k"
    if abs(y) >= 10:
        return f"{y:.0f}"
    return f"{y:.2f}"


def plot_grouped_bars(ax, title: str, modes: list[str], values_by_mode: dict[str, float], ylabel: str):
    xs = list(range(len(modes)))
    ys = [values_by_mode.get(m, 0.0) for m in modes]

    bars = ax.bar(xs, ys)
    ax.set_title(title)
    ax.set_xticks(xs, modes)
    ax.set_ylabel(ylabel)
    ax.grid(axis="y", linestyle="--", linewidth=0.5, alpha=0.6)

    # Add value labels on bars
    ymax = max(ys) if ys else 0.0
    if ymax > 0:
        ax.set_ylim(0, ymax * 1.18)
    for rect, y in zip(bars, ys):
        if y == 0:
            continue
        ax.text(
            rect.get_x() + rect.get_width() / 2,
            rect.get_height(),
            _format_bar_value(y, ylabel),
            ha="center",
            va="bottom",
            fontsize=9,
            rotation=0,
        )


def main() -> int:
    ap = argparse.ArgumentParser(description="Plot kvstore bench CSV results")
    ap.add_argument("--csv", required=True, help="CSV produced by bench/run_matrix.sh")
    ap.add_argument("--out-dir", default="bench/plots", help="Output directory")
    ap.add_argument("--format", default="png", choices=["png", "svg"], help="Image format")
    ap.add_argument(
        "--modes",
        default="glibc,jemalloc,pool",
        help="Comma-separated mode order (default: glibc,jemalloc,pool)",
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
    modes = [m.strip() for m in args.modes.split(",") if m.strip()]

    # Group rows: struct -> phase -> mode -> row
    by_struct: dict[str, dict[str, dict[str, dict[str, str]]]] = defaultdict(lambda: defaultdict(dict))
    for r in rows:
        st = r["struct"].strip()
        ph = r["phase"].strip().upper()
        md = r["mode"].strip()
        if not st or not ph or not md:
            continue
        by_struct[st][ph][md] = r

    ensure_out_dir(args.out_dir)

    structs = sorted(by_struct.keys())

    # Use the first row for annotating N and value_size.
    sample = rows[0]
    n = inum(sample.get("count", "0"))
    vsize = inum(sample.get("value_size", "0"))

    for st in structs:
        phases = by_struct[st]

        # Throughput figure (all available phases)
        phase_order = ["SET", "GET", "DEL", "MIX"]
        phases_present = [p for p in phase_order if p in phases]
        if not phases_present:
            phases_present = sorted(phases.keys())

        k = len(phases_present)
        if k <= 2:
            rows, cols = 1, k
            figsize = (12, 4)
        elif k == 3:
            rows, cols = 1, 3
            figsize = (15, 4)
        else:
            cols = 2
            rows = int(math.ceil(k / cols))
            figsize = (12, 4 * rows)

        fig, axes = plt.subplots(rows, cols, figsize=figsize, constrained_layout=True)
        if hasattr(axes, "ravel"):
            axes_list = list(axes.ravel())
        else:
            axes_list = [axes]

        # Use per-phase N for title when available
        for i, ph in enumerate(phases_present):
            ax = axes_list[i]
            rows_by_mode = phases.get(ph, {})
            vals = {m: fnum(rows_by_mode.get(m, {}).get("cmds_per_s", "0")) for m in modes}
            ph_n = inum(next(iter(rows_by_mode.values()), {}).get("count", str(n)))
            ph_v = inum(next(iter(rows_by_mode.values()), {}).get("value_size", str(vsize)))
            plot_grouped_bars(
                ax,
                f"{st} {ph} throughput (N={ph_n}, value_size={ph_v})",
                modes,
                vals,
                "cmds/s",
            )
        # Hide any unused axes
        for j in range(len(phases_present), len(axes_list)):
            axes_list[j].axis("off")

        out1 = os.path.join(args.out_dir, f"throughput_{st}.{args.format}")
        fig.savefig(out1, dpi=160)
        plt.close(fig)

        # Memory figure (RSS/HWM) - prefer values after LOAD/SET phase (most meaningful)
        fig, axes = plt.subplots(1, 2, figsize=(12, 4), constrained_layout=True)
        for i, metric in enumerate(["VmRSS_kB", "VmHWM_kB"]):
            rows_by_mode = phases.get("LOAD", {}) or phases.get("SET", {}) or phases.get("READ", {}) or phases.get("GET", {})
            vals = {m: fnum(rows_by_mode.get(m, {}).get(metric, "0")) / 1024.0 for m in modes}
            plot_grouped_bars(
                axes[i],
                f"{st} memory {metric} (after phase={('LOAD' if 'LOAD' in phases else ('SET' if 'SET' in phases else ('READ' if 'READ' in phases else 'GET')))})",
                modes,
                vals,
                "MiB",
            )
        out2 = os.path.join(args.out_dir, f"memory_{st}.{args.format}")
        fig.savefig(out2, dpi=160)
        plt.close(fig)

    # Also write a tiny index text for convenience
    index_path = os.path.join(args.out_dir, "index.txt")
    with open(index_path, "w", encoding="utf-8") as f:
        for st in structs:
            f.write(f"throughput_{st}.{args.format}\n")
            f.write(f"memory_{st}.{args.format}\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
