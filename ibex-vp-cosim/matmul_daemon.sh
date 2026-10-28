#!/usr/bin/env bash
# Manage the matmul accelerator daemon.
#
#   matmul_daemon.sh start        detach a daemon (no-op if one is already up)
#   matmul_daemon.sh stop         SIGTERM it; removes socket and pidfile
#   matmul_daemon.sh restart
#   matmul_daemon.sh status       ping it over the socket
#   matmul_daemon.sh log          show the daemon log (log -f to follow)
#   matmul_daemon.sh foreground   run attached to this terminal (Ctrl+C stops)
#
# Socket: $IBEX_MATMUL_SOCKET (default /tmp/ibex-matmul-accel.sock).
# Pidfile and log sit beside it: SOCKET.pid, SOCKET.log.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/env.sh"
DAEMON="$ROOT/ibex-vp-cosim/build/matmul_daemon"
SOCK="${IBEX_MATMUL_SOCKET:-/tmp/ibex-matmul-accel.sock}"
PIDFILE="$SOCK.pid"
LOG="$SOCK.log"

ensure_built() {
  make -s -C "$ROOT/ibex-vp-cosim" matmul-daemon
}

# Prefer the daemon's own answer over the socket; fall back to the pidfile (for
# a hung daemon) only if that pid really is a matmul_daemon process.
daemon_pid() {
  local pid
  pid="$("$DAEMON" --socket "$SOCK" --ping 2>/dev/null |
    sed -n 's/^matmul daemon pid \([0-9][0-9]*\) .*/\1/p')"
  if [[ -z "$pid" && -f "$PIDFILE" ]]; then
    pid="$(cat "$PIDFILE" 2>/dev/null || true)"
    if ! [[ "$pid" =~ ^[0-9]+$ ]] ||
      ! ps -p "$pid" -o command= 2>/dev/null | grep -q matmul_daemon; then
      pid=""
    fi
  fi
  [[ -n "$pid" ]] && echo "$pid"
}

is_up() {
  "$DAEMON" --socket "$SOCK" --ping >/dev/null 2>&1
}

start() {
  ensure_built
  if is_up; then
    echo "matmul daemon already running on $SOCK"
    return 0
  fi
  "$DAEMON" --socket "$SOCK" --daemon --pidfile "$PIDFILE" --log "$LOG"
  echo "matmul daemon started: pid $(daemon_pid), socket $SOCK, log $LOG"
}

stop() {
  local pid
  ensure_built
  if ! pid="$(daemon_pid)"; then
    rm -f "$PIDFILE"
    echo "matmul daemon not running"
    return 0
  fi
  kill -TERM "$pid"
  for _ in $(seq 50); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  if kill -0 "$pid" 2>/dev/null; then
    # The next start replaces the stale socket this leaves behind.
    echo "pid $pid ignored SIGTERM; killing" >&2
    kill -KILL "$pid"
    rm -f "$PIDFILE"
  fi
  echo "matmul daemon stopped (pid $pid)"
}

case "${1:-}" in
  start) start ;;
  stop) stop ;;
  restart) stop && start ;;
  status) ensure_built && "$DAEMON" --socket "$SOCK" --ping ;;
  log)
    if [[ "${2:-}" == "-f" ]]; then exec tail -n 40 -f "$LOG"; fi
    cat "$LOG"
    ;;
  foreground)
    ensure_built
    exec "$DAEMON" --socket "$SOCK"
    ;;
  *)
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
