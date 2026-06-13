#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CONF="${ROOT_DIR}/bench/bench.conf"
BIN_DIR="${ROOT_DIR}/build/bin"

HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-2000}"
COUNT="${COUNT:-50000}"
VALUE_SIZE="${VALUE_SIZE:-64}"

MODES=(glibc jemalloc pool)
STRUCTS=(array rbtree hash skiplist)

declare -A SETCMD=(
  [array]=SET
  [rbtree]=RSET
  [hash]=HSET
  [skiplist]=LSET
)

declare -A GETCMD=(
  [array]=GET
  [rbtree]=RGET
  [hash]=HGET
  [skiplist]=LGET
)

declare -A DELCMD=(
  [array]=DEL
  [rbtree]=RDEL
  [hash]=HDEL
  [skiplist]=LDEL
)

wait_port() {
  local host="$1"; local port="$2"; local timeout_s="${3:-5}"
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

server_start() {
  local mode="$1"
  local pidfile="$2"

  local cmd=("${BIN_DIR}/kvstore-server" "${CONF}")

  if [[ "${mode}" == "jemalloc" ]]; then
    # Option A: use LD_PRELOAD (preferred if your distro provides it)
    # You can override JEMALLOC_SO at runtime.
    local so="${JEMALLOC_SO:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}"
    if [[ -f "${so}" ]]; then
      echo "[bench] starting server with LD_PRELOAD=${so}" >&2
      LD_PRELOAD="${so}" "${cmd[@]}" >/dev/null 2>&1 &
    else
      echo "[bench] WARN: ${so} not found; starting without LD_PRELOAD" >&2
      "${cmd[@]}" >/dev/null 2>&1 &
    fi
  else
    "${cmd[@]}" >/dev/null 2>&1 &
  fi

  local pid=$!
  echo "${pid}" >"${pidfile}"
  wait_port "${HOST}" "${PORT}" 8
}

server_stop() {
  local pidfile="$1"
  if [[ -f "${pidfile}" ]]; then
    local pid
    pid="$(cat "${pidfile}")" || true
    rm -f "${pidfile}" || true
    if [[ -n "${pid}" ]]; then
      kill "${pid}" >/dev/null 2>&1 || true
      wait "${pid}" >/dev/null 2>&1 || true
    fi
  fi
}

build_variant() {
  local mode="$1"

  pushd "${ROOT_DIR}" >/dev/null
  make clean >/dev/null

  if [[ "${mode}" == "pool" ]]; then
    echo "[bench] build: MM_POOL_ENABLE=1" >&2
    make all EXTRA_CFLAGS='-DMM_POOL_ENABLE=1' >/dev/null
  elif [[ "${mode}" == "jemalloc" ]]; then
    # Two ways:
    # - Link-time: EXTRA_LDFLAGS='-ljemalloc' (requires libjemalloc-dev)
    # - Runtime: LD_PRELOAD in server_start()
    # Here we keep default link flags and rely on LD_PRELOAD (more portable across environments).
    echo "[bench] build: glibc + (jemalloc via LD_PRELOAD at runtime)" >&2
    make all >/dev/null
  else
    echo "[bench] build: glibc (no pool)" >&2
    make all >/dev/null
  fi

  popd >/dev/null
}

mem_kb() {
  local pid="$1"
  if [[ ! -r "/proc/${pid}/status" ]]; then
    echo "VmRSS_kB=0 VmHWM_kB=0"
    return 0
  fi
  local rss hwm
  rss=$(awk '/^VmRSS:/ {print $2}' "/proc/${pid}/status" | head -n1)
  hwm=$(awk '/^VmHWM:/ {print $2}' "/proc/${pid}/status" | head -n1)
  echo "VmRSS_kB=${rss:-0} VmHWM_kB=${hwm:-0}"
}

run_pipe() {
  local resp_file="$1"
  "${BIN_DIR}/kvstore-cli" --pipe --resp --quiet -h "${HOST}" -p "${PORT}" --pipe-from "${resp_file}" 2>&1 1>/dev/null | tail -n 1
}

# CSV header
printf "mode,struct,phase,count,value_size,cmds_per_s,time_ms,sent_bytes,VmRSS_kB,VmHWM_kB\n"

for mode in "${MODES[@]}"; do
  build_variant "${mode}"

  for st in "${STRUCTS[@]}"; do
    pidfile="${ROOT_DIR}/bench/.kvstore.${mode}.${st}.pid"
    server_stop "${pidfile}"
    server_start "${mode}" "${pidfile}"
    pid="$(cat "${pidfile}")"

    outdir="${ROOT_DIR}/bench/out/${mode}/${st}"
    mkdir -p "${outdir}"

    # generate RESP pipelines
    python3 "${ROOT_DIR}/tools/gen_resp_pipelines.py" \
      --count "${COUNT}" \
      --out-dir "${outdir}" \
      --set-name "set.resp" \
      --get-name "get.resp" \
      --del-name "del.resp" \
      --mix-name "mix.resp" \
      --set-cmd "${SETCMD[$st]}" \
      --get-cmd "${GETCMD[$st]}" \
      --del-cmd "${DELCMD[$st]}" \
      --key-prefix "k" \
      --key-width 7 \
      --value-prefix "v" \
      --value-size "${VALUE_SIZE}" \
      >/dev/null

    # phase 1: SET
    line_set="$(run_pipe "${outdir}/set.resp")"
    # Example: [pipe] sent_bytes=..., approx_cmds=..., time_ms=..., MB/s=..., cmds/s=...
    sent_bytes=$(echo "${line_set}" | sed -n 's/.*sent_bytes=\([0-9][0-9]*\).*/\1/p')
    time_ms=$(echo "${line_set}" | sed -n 's/.*time_ms=\([0-9][0-9]*\).*/\1/p')
    cmds_s=$(echo "${line_set}" | sed -n 's/.*cmds\/s=\([0-9.][0-9.]*\).*/\1/p')
    m="$(mem_kb "${pid}")"
    rss=$(echo "${m}" | sed -n 's/.*VmRSS_kB=\([0-9][0-9]*\).*/\1/p')
    hwm=$(echo "${m}" | sed -n 's/.*VmHWM_kB=\([0-9][0-9]*\).*/\1/p')
    printf "%s,%s,SET,%s,%s,%s,%s,%s,%s,%s\n" "${mode}" "${st}" "${COUNT}" "${VALUE_SIZE}" "${cmds_s:-0}" "${time_ms:-0}" "${sent_bytes:-0}" "${rss:-0}" "${hwm:-0}"

    # phase 2: GET
    line_get="$(run_pipe "${outdir}/get.resp")"
    sent_bytes=$(echo "${line_get}" | sed -n 's/.*sent_bytes=\([0-9][0-9]*\).*/\1/p')
    time_ms=$(echo "${line_get}" | sed -n 's/.*time_ms=\([0-9][0-9]*\).*/\1/p')
    cmds_s=$(echo "${line_get}" | sed -n 's/.*cmds\/s=\([0-9.][0-9.]*\).*/\1/p')
    m="$(mem_kb "${pid}")"
    rss=$(echo "${m}" | sed -n 's/.*VmRSS_kB=\([0-9][0-9]*\).*/\1/p')
    hwm=$(echo "${m}" | sed -n 's/.*VmHWM_kB=\([0-9][0-9]*\).*/\1/p')
    printf "%s,%s,GET,%s,%s,%s,%s,%s,%s,%s\n" "${mode}" "${st}" "${COUNT}" "${VALUE_SIZE}" "${cmds_s:-0}" "${time_ms:-0}" "${sent_bytes:-0}" "${rss:-0}" "${hwm:-0}"

    # phase 3: DEL
    line_del="$(run_pipe "${outdir}/del.resp")"
    sent_bytes=$(echo "${line_del}" | sed -n 's/.*sent_bytes=\([0-9][0-9]*\).*/\1/p')
    time_ms=$(echo "${line_del}" | sed -n 's/.*time_ms=\([0-9][0-9]*\).*/\1/p')
    cmds_s=$(echo "${line_del}" | sed -n 's/.*cmds\/s=\([0-9.][0-9.]*\).*/\1/p')
    m="$(mem_kb "${pid}")"
    rss=$(echo "${m}" | sed -n 's/.*VmRSS_kB=\([0-9][0-9]*\).*/\1/p')
    hwm=$(echo "${m}" | sed -n 's/.*VmHWM_kB=\([0-9][0-9]*\).*/\1/p')
    printf "%s,%s,DEL,%s,%s,%s,%s,%s,%s,%s\n" "${mode}" "${st}" "${COUNT}" "${VALUE_SIZE}" "${cmds_s:-0}" "${time_ms:-0}" "${sent_bytes:-0}" "${rss:-0}" "${hwm:-0}"

    # phase 4: MIX (SET+GET+DEL per key)
    mix_cmds=$((COUNT * 3))
    line_mix="$(run_pipe "${outdir}/mix.resp")"
    sent_bytes=$(echo "${line_mix}" | sed -n 's/.*sent_bytes=\([0-9][0-9]*\).*/\1/p')
    time_ms=$(echo "${line_mix}" | sed -n 's/.*time_ms=\([0-9][0-9]*\).*/\1/p')
    cmds_s=$(echo "${line_mix}" | sed -n 's/.*cmds\/s=\([0-9.][0-9.]*\).*/\1/p')
    m="$(mem_kb "${pid}")"
    rss=$(echo "${m}" | sed -n 's/.*VmRSS_kB=\([0-9][0-9]*\).*/\1/p')
    hwm=$(echo "${m}" | sed -n 's/.*VmHWM_kB=\([0-9][0-9]*\).*/\1/p')
    printf "%s,%s,MIX,%s,%s,%s,%s,%s,%s,%s\n" "${mode}" "${st}" "${mix_cmds}" "${VALUE_SIZE}" "${cmds_s:-0}" "${time_ms:-0}" "${sent_bytes:-0}" "${rss:-0}" "${hwm:-0}"

    server_stop "${pidfile}"
  done
done
