#!/bin/sh
# BusyBox supervisor for /dev/nd_rpc session reset and USB reconnects.
executor=${ND_EXECUTOR:-/root/nd-runtime/nd-executor}
device=${ND_EXECUTOR_DEVICE:-/dev/nd_rpc}
pidfile=${ND_EXECUTOR_PID:-/var/run/nd-executor.pid}
log=${ND_EXECUTOR_LOG:-/tmp/nd-executor.log}
case "$1" in
  start)
    if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then exit 0; fi
    "$0" run </dev/null >>"$log" 2>&1 &
    echo $! >"$pidfile"
    ;;
  run)
    child=
    trap '[ -z "$child" ] || kill "$child" 2>/dev/null; exit 0' TERM INT
    while :; do
      if [ -c "$device" ] && [ -x "$executor" ]; then
        "$executor" --device "$device" &
        child=$!
        wait "$child"
        child=
      fi
      sleep 1
    done
    ;;
  stop)
    if [ -f "$pidfile" ]; then
      pid=$(cat "$pidfile")
      case "$pid" in ''|*[!0-9]*) exit 1;; esac
      kill "$pid" 2>/dev/null || :
      rm -f "$pidfile"
    fi
    ;;
  restart) "$0" stop; "$0" start;;
  *) echo "usage: $0 start|stop|restart" >&2; exit 2;;
esac
