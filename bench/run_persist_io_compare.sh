#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN_DIR="${ROOT_DIR}/build/bin"
CONF="${ROOT_DIR}/bench/persist_io.conf"

HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-2000}"
COUNT="${COUNT:-50000}"
VALUE_SIZE="${VALUE_SIZE:-256}"
ITERS="${ITERS:-5}"
CHUNK="${CHUNK:-8MiB}"
RAND_OPS="${RAND_OPS:-20000}"
RAND_BLOCK="${RAND_BLOCK:-4KiB}"
WRITE_SIZE="${WRITE_SIZE:-256MiB}"
SYNC_POLICY="${SYNC_POLICY:-none}"   # none|fsync|msync|msync+fsync
DROP_CACHES="${DROP_CACHES:-0}"      # 1 => try sudo drop_caches between runs
WORKLOADS="${WORKLOADS:-read_rand}"  # persistence mmap is used for load/recover => focus on random read

OUT_DIR="${OUT_DIR:-${ROOT_DIR}/bench/out/persist_io_compare}"
PERSIST_DIR="${PERSIST_DIR:-${ROOT_DIR}/bench/out/persist}"

PIDFILE="${OUT_DIR}/kvstore.pid"
LOGFILE="${OUT_DIR}/kvstore.log"

mkdir -p "${OUT_DIR}"
mkdir -p "${PERSIST_DIR}"

have() { command -v "$1" >/dev/null 2>&1; }

wait_port() {
  local host="$1"; local port="$2"; local timeout_s="${3:-8}"
  python3 - <<PY
import socket, time, sys
host, port = "${host}", int("${port}")
deadline = time.time() + float("${timeout_s}")
while time.time() < deadline:
    try:
        s = socket.create_connection((host, port), timeout=0.2)
        s.close()
        sys.exit(0)
    except OSError:
        time.sleep(0.05)
sys.exit(1)
PY
}

server_stop() {
  if [[ -f "${PIDFILE}" ]]; then
    local pid
    pid="$(cat "${PIDFILE}" || true)"
    rm -f "${PIDFILE}" || true
    if [[ -n "${pid}" ]]; then
      kill "${pid}" >/dev/null 2>&1 || true
      wait "${pid}" >/dev/null 2>&1 || true
    fi
  fi
}

cleanup() {
  server_stop
}
trap cleanup EXIT

echo "[persist-io] build kvstore (make all)" >&2
pushd "${ROOT_DIR}" >/dev/null
make all >/dev/null
popd >/dev/null

if [[ ! -x "${BIN_DIR}/kvstore-server" || ! -x "${BIN_DIR}/kvstore-cli" ]]; then
  echo "[persist-io] ERROR: missing binaries under ${BIN_DIR}" >&2
  exit 1
fi

if [[ ! -f "${CONF}" ]]; then
  echo "[persist-io] ERROR: missing config: ${CONF}" >&2
  exit 1
fi

server_stop

echo "[persist-io] start server (conf=${CONF})" >&2
"${BIN_DIR}/kvstore-server" "${CONF}" >/dev/null 2>"${LOGFILE}" &
echo $! >"${PIDFILE}"
wait_port "${HOST}" "${PORT}" 10

# Generate SET pipeline
PIPE_DIR="${OUT_DIR}/pipes"
mkdir -p "${PIPE_DIR}"

echo "[persist-io] generate RESP pipelines (COUNT=${COUNT}, VALUE_SIZE=${VALUE_SIZE})" >&2
python3 "${ROOT_DIR}/tools/gen_resp_pipelines.py" \
  --count "${COUNT}" \
  --out-dir "${PIPE_DIR}" \
  --set-name "set.resp" \
  --get-name "get.resp" \
  --del-name "del.resp" \
  --mix-name "mix.resp" \
  --set-cmd "SET" \
  --get-cmd "GET" \
  --del-cmd "DEL" \
  --key-prefix "k" \
  --key-width 7 \
  --value-prefix "v" \
  --value-size "${VALUE_SIZE}" \
  >/dev/null

echo "[persist-io] load data via pipeline (SET x ${COUNT})" >&2
"${BIN_DIR}/kvstore-cli" --pipe --resp --quiet -h "${HOST}" -p "${PORT}" --pipe-from "${PIPE_DIR}/set.resp" >/dev/null

# Trigger persistence
echo "[persist-io] trigger BGSAVE" >&2
"${BIN_DIR}/kvstore-cli" --resp -h "${HOST}" -p "${PORT}" BGSAVE >/dev/null || true

echo "[persist-io] trigger BGREWRITEAOF" >&2
"${BIN_DIR}/kvstore-cli" --resp -h "${HOST}" -p "${PORT}" BGREWRITEAOF >/dev/null || true

DUMP_RDB="${PERSIST_DIR}/dump.rdb"
AOF_FILE="${PERSIST_DIR}/appendonly.aof"
AOF_TMP="${AOF_FILE}.tmp"
AOF_INCR="${AOF_FILE}.incr"

echo "[persist-io] wait persistence files in ${PERSIST_DIR}" >&2
python3 - <<PY
import os, time, sys
rdb = "${DUMP_RDB}"
aof = "${AOF_FILE}"
aof_tmp = "${AOF_TMP}"
aof_incr = "${AOF_INCR}"
deadline = time.time() + 30

def nonempty(p):
    return os.path.exists(p) and os.path.getsize(p) > 0

while time.time() < deadline:
    ok_rdb = nonempty(rdb)
    ok_aof = nonempty(aof)
    # rewrite finalization: tmp/incr removed
    clean = (not os.path.exists(aof_tmp)) and (not os.path.exists(aof_incr))
    if ok_rdb and ok_aof and clean:
        sys.exit(0)
    time.sleep(0.2)

print("timeout waiting persistence files", file=sys.stderr)
print("rdb=", os.path.exists(rdb), "size=", os.path.getsize(rdb) if os.path.exists(rdb) else 0, file=sys.stderr)
print("aof=", os.path.exists(aof), "size=", os.path.getsize(aof) if os.path.exists(aof) else 0, file=sys.stderr)
print("aof_tmp=", os.path.exists(aof_tmp), "aof_incr=", os.path.exists(aof_incr), file=sys.stderr)
sys.exit(1)
PY

# Stop server to avoid ongoing writes during measurement
server_stop

DROP_CMD=""
if [[ "${DROP_CACHES}" == "1" ]]; then
  # Best-effort: requires root; if sudo missing/denied, we'll continue without dropping cache.
  if have sudo; then
    DROP_CMD="sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'"
  else
    echo "[persist-io] WARN: sudo not found; skip drop_caches" >&2
  fi
fi

echo "[persist-io] run micro-benchmark (RDB)" >&2
python3 "${ROOT_DIR}/bench/persist_io_bench.py" \
  --read-file "${DUMP_RDB}" \
  --iters "${ITERS}" \
  --chunk "${CHUNK}" \
  --rand-ops "${RAND_OPS}" \
  --rand-block "${RAND_BLOCK}" \
  --write-size "${WRITE_SIZE}" \
  --sync "${SYNC_POLICY}" \
  --workloads "${WORKLOADS}" \
  ${DROP_CMD:+--drop-caches-cmd "${DROP_CMD}"} \
  --out "${OUT_DIR}/io_compare_rdb.csv"

python3 "${ROOT_DIR}/bench/plot_io_compare.py" \
  "${OUT_DIR}/io_compare_rdb.csv" \
  --ops read_rand \
  --methods mmap,pread \
  --out "${OUT_DIR}/io_compare_rdb.png"

echo "[persist-io] run micro-benchmark (AOF)" >&2
python3 "${ROOT_DIR}/bench/persist_io_bench.py" \
  --read-file "${AOF_FILE}" \
  --iters "${ITERS}" \
  --chunk "${CHUNK}" \
  --rand-ops "${RAND_OPS}" \
  --rand-block "${RAND_BLOCK}" \
  --write-size "${WRITE_SIZE}" \
  --sync "${SYNC_POLICY}" \
  --workloads "${WORKLOADS}" \
  ${DROP_CMD:+--drop-caches-cmd "${DROP_CMD}"} \
  --out "${OUT_DIR}/io_compare_aof.csv"

python3 "${ROOT_DIR}/bench/plot_io_compare.py" \
  "${OUT_DIR}/io_compare_aof.csv" \
  --ops read_rand \
  --methods mmap,pread \
  --out "${OUT_DIR}/io_compare_aof.png"

echo "[persist-io] done" >&2
echo "[persist-io] outputs:" >&2
echo "  ${OUT_DIR}/io_compare_rdb.csv" >&2
echo "  ${OUT_DIR}/io_compare_rdb.png" >&2
echo "  ${OUT_DIR}/io_compare_aof.csv" >&2
echo "  ${OUT_DIR}/io_compare_aof.png" >&2
