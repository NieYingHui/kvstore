import argparse
import socket
import hashlib
import os
from typing import Tuple, Optional


ENGINE_TO_CMDS = {
    "array": (b"SET", b"GET"),
    "hash": (b"HSET", b"HGET"),
    "rbtree": (b"RSET", b"RGET"),
    "skiplist": (b"LSET", b"LGET"),
}


def build_resp_array(args_bytes) -> bytes:
    out = bytearray()
    out += f"*{len(args_bytes)}\r\n".encode("ascii")
    for a in args_bytes:
        out += f"${len(a)}\r\n".encode("ascii")
        out += a
        out += b"\r\n"
    return bytes(out)


def recv_exact(sock: socket.socket, n: int) -> bytes:
    chunks = []
    got = 0
    while got < n:
        chunk = sock.recv(n - got)
        if not chunk:
            raise ConnectionError("socket closed")
        chunks.append(chunk)
        got += len(chunk)
    return b"".join(chunks)


def recv_line(sock: socket.socket) -> bytes:
    # Read until CRLF
    buf = bytearray()
    while True:
        b = sock.recv(1)
        if not b:
            raise ConnectionError("socket closed")
        buf += b
        if len(buf) >= 2 and buf[-2:] == b"\r\n":
            return bytes(buf)


def parse_resp(sock: socket.socket) -> Tuple[str, Optional[bytes]]:
    """Return (type, payload).

    type:
      'simple' '+'
      'error'  '-'
      'int'    ':'
      'bulk'   '$' (payload is bytes, None means null bulk)
      'array'  '*' (payload is raw; not used here)
    """
    first = recv_exact(sock, 1)
    if first == b"+":
        line = recv_line(sock)
        return "simple", line[:-2]
    if first == b"-":
        line = recv_line(sock)
        return "error", line[:-2]
    if first == b":":
        line = recv_line(sock)
        return "int", line[:-2]
    if first == b"$":
        line = recv_line(sock)
        n = int(line[:-2].decode("ascii"))
        if n == -1:
            return "bulk", None
        data = recv_exact(sock, n)
        crlf = recv_exact(sock, 2)
        if crlf != b"\r\n":
            raise ValueError("invalid bulk terminator")
        return "bulk", data
    if first == b"*":
        # Minimal support: just read array header and stop.
        line = recv_line(sock)
        return "array", first + line

    raise ValueError(f"unknown RESP type byte: {first!r}")


def sha256_hex(b: bytes) -> str:
    h = hashlib.sha256()
    h.update(b)
    return h.hexdigest()


def first_diff(a: bytes, b: bytes) -> Optional[int]:
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    if len(a) != len(b):
        return n
    return None


def parse_key_bytes(args: argparse.Namespace) -> bytes:
    if args.key_mode == "literal":
        return args.key.encode(args.key_encoding, errors="strict")
    if args.key_mode == "special":
        # One key that covers: leading/trailing spaces, internal spaces, LF, CRLF, TAB.
        return b"  blog key with spaces\nline2\tand\r\nCRLF  "
    if args.key_mode == "hex":
        s = (args.key_hex or "").strip().lower()
        if s.startswith("0x"):
            s = s[2:]
        if s == "":
            raise SystemExit("--key-mode hex requires --key-hex")
        try:
            return bytes.fromhex(s)
        except ValueError as e:
            raise SystemExit(f"Invalid --key-hex: {e}")
    if args.key_mode == "file":
        if not args.key_file:
            raise SystemExit("--key-mode file requires --key-file")
        with open(args.key_file, "rb") as f:
            return f.read()
    raise SystemExit(f"Unknown --key-mode: {args.key_mode}")


def print_key_info(key_b: bytes) -> None:
    preview = key_b
    if len(preview) > 120:
        preview = preview[:120] + b"..."
    print(f"Key length: {len(key_b)}")
    print(f"Key preview (python bytes): {preview!r}")
    print(f"Key sha256: {sha256_hex(key_b)}")


def main():
    ap = argparse.ArgumentParser(
        description=(
            "Manual RESP blog test: SET-like key <file>, then GET-like and verify byte-identical roundtrip. "
            "Supports kvstore's array/hash/rbtree/skiplist command spaces."
        )
    )
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=2000)
    ap.add_argument(
        "--engine",
        choices=["array", "hash", "rbtree", "skiplist"],
        default="array",
        help="Which KV space to test (affects SET/GET command names)",
    )
    ap.add_argument(
        "--set-cmd",
        default=None,
        help="Override SET-like command (bytes are UTF-8 encoded), e.g. SET/HSET/RSET/LSET",
    )
    ap.add_argument(
        "--get-cmd",
        default=None,
        help="Override GET-like command (bytes are UTF-8 encoded), e.g. GET/HGET/RGET/LGET",
    )
    ap.add_argument("--key", default="blog:1", help="Used when --key-mode=literal")
    ap.add_argument(
        "--key-mode",
        choices=["literal", "special", "hex", "file"],
        default="special",
        help="How to construct key bytes. 'special' includes spaces/newlines/tabs.",
    )
    ap.add_argument("--key-encoding", default="utf-8", help="Encoding for --key when key-mode=literal")
    ap.add_argument("--key-hex", default=None, help="Raw key bytes as hex string when --key-mode=hex")
    ap.add_argument("--key-file", default=None, help="Read raw key bytes from file when --key-mode=file")
    ap.add_argument("--file", default="../test.txt")
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument(
        "--repeat",
        type=int,
        default=1,
        help="Repeat SET/GET N times (useful for catching edge cases)",
    )
    args = ap.parse_args()

    # Resolve command bytes
    default_set, default_get = ENGINE_TO_CMDS[args.engine]
    set_cmd_b = (args.set_cmd.encode("utf-8") if args.set_cmd else default_set)
    get_cmd_b = (args.get_cmd.encode("utf-8") if args.get_cmd else default_get)

    key_b = parse_key_bytes(args)
    print_key_info(key_b)

    with open(args.file, "rb") as f:
        value_b = f.read()

    print(f"Value length: {len(value_b)}")
    print(f"Value sha256: {sha256_hex(value_b)}")

    print(f"Engine: {args.engine}")
    print(f"SET cmd: {set_cmd_b!r}")
    print(f"GET cmd: {get_cmd_b!r}")

    set_frame = build_resp_array([set_cmd_b, key_b, value_b])
    get_frame = build_resp_array([get_cmd_b, key_b])

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(args.timeout)
    try:
        sock.connect((args.host, args.port))

        for i in range(args.repeat):
            if args.repeat > 1:
                print(f"\n--- Iteration {i + 1}/{args.repeat} ---")

            # SET-like
            sock.sendall(set_frame)
            t, payload = parse_resp(sock)
            if t == "error":
                raise RuntimeError(
                    f"{set_cmd_b.decode('utf-8', errors='replace')} failed: {payload.decode('utf-8', errors='replace')}"
                )
            if t == "simple":
                print(f"SET reply: +{payload.decode('utf-8', errors='replace')}")
            elif t == "bulk" and payload is not None:
                print(f"SET reply bulk: {payload.decode('utf-8', errors='replace')}")
            else:
                print(f"SET reply type: {t}")

            # GET-like
            sock.sendall(get_frame)
            t, payload = parse_resp(sock)
            if t == "error":
                raise RuntimeError(
                    f"{get_cmd_b.decode('utf-8', errors='replace')} failed: {payload.decode('utf-8', errors='replace')}"
                )
            if t != "bulk":
                raise RuntimeError(f"Unexpected GET reply type: {t}")
            if payload is None:
                raise RuntimeError("GET returned Null Bulk ($-1), key not found")

            returned = payload
            ok_len = (len(returned) == len(value_b))
            ok_bytes = (returned == value_b)
            diff_at = first_diff(returned, value_b)

            print(f"GET bulk length: {len(returned)} (expected {len(value_b)})")
            print(f"Returned sha256: {sha256_hex(returned)}")
            print(f"Length match: {ok_len}")
            print(f"Content match (byte-equal): {ok_bytes}")

            if not ok_bytes:
                print(f"First difference offset: {diff_at}")
                if diff_at is not None:
                    lo = max(0, diff_at - 16)
                    hi = min(len(value_b), diff_at + 16)
                    print(f"Expected slice [{lo}:{hi}]: {value_b[lo:hi]!r}")
                    print(f"Returned slice [{lo}:{hi}]: {returned[lo:hi]!r}")
                raise SystemExit(2)

        print("\nOK: RESP roundtrip verified (key supports special chars; value byte-identical).")

    finally:
        sock.close()

    # Extra diagnostic: open a fresh connection and GET again (mimic kvstore-cli behavior)
    sock2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock2.settimeout(args.timeout)
    try:
        sock2.connect((args.host, args.port))
        sock2.sendall(get_frame)
        t2, payload2 = parse_resp(sock2)
        if t2 == "error":
            raise RuntimeError(
                f"{get_cmd_b.decode('utf-8', errors='replace')} (fresh conn) failed: {payload2.decode('utf-8', errors='replace')}"
            )
        if t2 != "bulk":
            raise RuntimeError(f"Unexpected GET (fresh conn) reply type: {t2}")
        if payload2 is None:
            raise RuntimeError("GET (fresh conn) returned Null Bulk ($-1)")
        print(f"GET (fresh conn) bulk length: {len(payload2)}")
    finally:
        sock2.close()


if __name__ == "__main__":
    main()


# Examples:
# python3 kvs-client/resp_blog_test.py --engine array --key blog:1 --file test.txt
# python3 kvs-client/resp_blog_test.py --engine hash --key-mode special --file test.txt
# python3 kvs-client/resp_blog_test.py --engine rbtree --key-mode special --file test.txt
# python3 kvs-client/resp_blog_test.py --engine skiplist --key-mode special --file test.txt
