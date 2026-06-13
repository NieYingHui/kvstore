#!/usr/bin/env python3
"""Generate CRLF-correct RESP pipeline files.

Why this exists:
- The server RESP parser requires strict \r\n line endings.
- If pipeline files are treated as text (e.g., by git/autocrlf/editors), \r may be stripped,
  causing `-ERR protocol error` and subsequent desync.

This script writes bytes in binary mode and always uses CRLF.

Examples:
  python3 ./tools/gen_resp_pipelines.py --count 200 --out-dir .
  python3 tools/gen_resp_pipelines.py --count 200 --out-dir . --set-cmd HSET --get-cmd HGET --set-name pipeline_hset.resp --get-name pipeline_hget.resp
"""

from __future__ import annotations

import argparse
from pathlib import Path

CRLF = b"\r\n"


def bulk(s: str) -> bytes:
    b = s.encode("utf-8")
    return b"$" + str(len(b)).encode("ascii") + CRLF + b + CRLF


def array(parts: list[str]) -> bytes:
    out = [b"*" + str(len(parts)).encode("ascii") + CRLF]
    out.extend(bulk(p) for p in parts)
    return b"".join(out)


def gen_set(count: int) -> bytes:
    chunks: list[bytes] = []
    for i in range(1, count + 1):
        key = f"k{i:07d}"
        val = f"v{i:07d}"
        chunks.append(array(["SET", key, val]))
    return b"".join(chunks)


def gen_get(count: int) -> bytes:
    chunks: list[bytes] = []
    for i in range(1, count + 1):
        key = f"k{i:07d}"
        chunks.append(array(["GET", key]))
    return b"".join(chunks)


def _make_key(i: int, prefix: str, width: int) -> str:
    if width <= 0:
        return f"{prefix}{i}"
    return f"{prefix}{i:0{width}d}"


def _make_value(i: int, prefix: str, size: int) -> str:
    """Return an ASCII value of exact `size` bytes (when size>0).

    We keep it simple/deterministic for reproducible benchmarks.
    """
    if size <= 0:
        return f"{prefix}{i}"
    seed = f"{prefix}{i}:"
    if len(seed) >= size:
        return seed[:size]
    pad_len = size - len(seed)
    return seed + ("x" * pad_len)


def gen_set_custom(count: int, cmd: str, key_prefix: str, key_width: int,
                   value_prefix: str, value_size: int) -> bytes:
    chunks: list[bytes] = []
    for i in range(1, count + 1):
        key = _make_key(i, key_prefix, key_width)
        val = _make_value(i, value_prefix, value_size)
        chunks.append(array([cmd, key, val]))
    return b"".join(chunks)


def gen_get_custom(count: int, cmd: str, key_prefix: str, key_width: int) -> bytes:
    chunks: list[bytes] = []
    for i in range(1, count + 1):
        key = _make_key(i, key_prefix, key_width)
        chunks.append(array([cmd, key]))
    return b"".join(chunks)


def gen_del_custom(count: int, cmd: str, key_prefix: str, key_width: int) -> bytes:
    chunks: list[bytes] = []
    for i in range(1, count + 1):
        key = _make_key(i, key_prefix, key_width)
        chunks.append(array([cmd, key]))
    return b"".join(chunks)


def gen_mix_sgd(count: int,
                set_cmd: str,
                get_cmd: str,
                del_cmd: str,
                key_prefix: str,
                key_width: int,
                value_prefix: str,
                value_size: int) -> bytes:
    """Generate a mixed workload: for each key do SET -> GET -> DEL.

    This stresses alloc/free behavior while keeping dataset size bounded.
    Total commands = 3 * count.
    """
    chunks: list[bytes] = []
    for i in range(1, count + 1):
        key = _make_key(i, key_prefix, key_width)
        val = _make_value(i, value_prefix, value_size)
        chunks.append(array([set_cmd, key, val]))
        chunks.append(array([get_cmd, key]))
        chunks.append(array([del_cmd, key]))
    return b"".join(chunks)


def write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--out-dir", type=Path, default=Path("."))
    ap.add_argument("--set-name", default="pipeline_set.resp")
    ap.add_argument("--get-name", default="pipeline_get.resp")

    # Optional extra pipelines
    ap.add_argument("--del-name", default="", help="If set, also generate DEL pipeline file name")
    ap.add_argument("--mix-name", default="", help="If set, also generate MIX pipeline file name")

    # Benchmark customization (backward compatible defaults)
    ap.add_argument("--set-cmd", default="SET", help="SET-like command name, e.g. SET/HSET/RSET/LSET")
    ap.add_argument("--get-cmd", default="GET", help="GET-like command name, e.g. GET/HGET/RGET/LGET")
    ap.add_argument("--del-cmd", default="DEL", help="DEL-like command name, e.g. DEL/HDEL/RDEL/LDEL")
    ap.add_argument("--key-prefix", default="k", help="Key prefix")
    ap.add_argument("--key-width", type=int, default=7, help="Zero pad width for key index (default 7 -> k0000001)")
    ap.add_argument("--value-prefix", default="v", help="Value prefix")
    ap.add_argument("--value-size", type=int, default=0, help="If >0, generate fixed-size ASCII values")
    args = ap.parse_args()

    if args.count <= 0:
        raise SystemExit("--count must be > 0")

    out_dir: Path = args.out_dir
    write_bytes(
        out_dir / args.set_name,
        gen_set_custom(args.count, args.set_cmd, args.key_prefix, args.key_width,
                       args.value_prefix, args.value_size),
    )
    write_bytes(
        out_dir / args.get_name,
        gen_get_custom(args.count, args.get_cmd, args.key_prefix, args.key_width),
    )

    if args.del_name:
        write_bytes(
            out_dir / args.del_name,
            gen_del_custom(args.count, args.del_cmd, args.key_prefix, args.key_width),
        )

    if args.mix_name:
        write_bytes(
            out_dir / args.mix_name,
            gen_mix_sgd(
                args.count,
                args.set_cmd,
                args.get_cmd,
                args.del_cmd,
                args.key_prefix,
                args.key_width,
                args.value_prefix,
                args.value_size,
            ),
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
