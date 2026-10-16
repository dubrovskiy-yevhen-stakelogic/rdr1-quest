#!/system/bin/sh
# GPU boost helper for Red Dead Redemption VR on Meta Quest 3 and Quest 3S.
#
# Android apps cannot raise the GPU clock themselves; the ADB shell can. This
# helper runs in the background as the ADB shell user and raises the GPU level
# only while the game runs and asks for it in its GRAPHICS menu. When the game
# closes or the boost is switched off, the normal dynamic clocks return, so the
# Home menu and other apps are not affected. A headset restart stops the
# helper, so it is started again after every restart:
#
#   adb shell sh /data/local/tmp/rdrvr_boost.sh start    start in the background
#   adb shell sh /data/local/tmp/rdrvr_boost.sh stop     stop and restore the clocks
#   adb shell sh /data/local/tmp/rdrvr_boost.sh status   show the current state
#
# Every 2 seconds the helper reads the level the game asks for (0, 6 or 7)
# from boost.req in the game's settings folder. While the game process exists
# and asks for 6 (640 MHz) or 7 (690 MHz), debug.oculus.gpuLevel is set to that
# level; 7 also sets debug.oculus.cpuLevel 8 (CPU 2.36 GHz), because near busy
# places the game waits for the CPU as much as for the GPU. Otherwise the levels
# set by this helper are cleared again. Levels set by another tool are never
# cleared. The current time is written to debug.rdrvr.boostd on every pass so
# the game can see that the helper runs.

PACKAGE=com.rockstargames.rdr
REQUEST_FILE=/sdcard/Android/data/$PACKAGE/files/rdrvr/boost.req
PID_FILE=/data/local/tmp/rdrvr_boost.pid
LOG_FILE=/data/local/tmp/rdrvr_boost.log
applied=
cpu_applied=

# Appends one line to the log; the log starts over when it exceeds 64 KB.
log() {
  if [ -f "$LOG_FILE" ] && [ $(( $(wc -c < "$LOG_FILE") )) -gt 65536 ]; then
    : > "$LOG_FILE"
  fi
  echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG_FILE"
}

# True when the process with the given pid runs this script. The command line
# in /proc is NUL-separated, so xargs -0 turns it into plain words.
is_helper() {
  case $(xargs -0 2>/dev/null < "/proc/$1/cmdline") in
    *rdrvr_boost*) return 0 ;;
  esac
  return 1
}

# Prints the pid of a running helper other than this process.
running_pid() {
  p=$(cat "$PID_FILE" 2>/dev/null)
  case $p in ''|*[!0-9]*) return 1 ;; esac
  [ "$p" != "$$" ] && is_helper "$p" || return 1
  echo "$p"
}

# Asks the helper with the given pid to exit and waits up to 5 seconds.
stop_helper() {
  kill "$1" 2>/dev/null
  n=0
  while [ $n -lt 5 ] && is_helper "$1"; do
    sleep 1
    n=$((n + 1))
  done
  ! is_helper "$1"
}

finish() {
  trap - TERM INT
  if [ -n "$applied" ]; then
    setprop debug.oculus.gpuLevel ""
    log "GPU level cleared"
  fi
  if [ -n "$cpu_applied" ]; then
    setprop debug.oculus.cpuLevel ""
    log "CPU level cleared"
  fi
  setprop debug.rdrvr.boostd ""
  if [ "$(cat "$PID_FILE" 2>/dev/null)" = "$$" ]; then
    rm -f "$PID_FILE"
  fi
  log "exit: $1"
  exit 0
}

run() {
  old=$(running_pid)
  echo $$ > "$PID_FILE"
  if [ -n "$old" ]; then
    stop_helper "$old"
  fi
  trap 'finish "stopped"' TERM INT
  log "start: pid $$${old:+, replaced pid $old}"
  # The loop also ends when a newer helper takes over the pid file.
  while [ "$(cat "$PID_FILE" 2>/dev/null)" = "$$" ]; do
    want=
    for package in com.rockstargames.rdr com.netflix.NGP.Kamo; do
      if pidof "$package" >/dev/null 2>&1; then
        request=/sdcard/Android/data/$package/files/rdrvr/boost.req
        case $(cat "$request" 2>/dev/null) in
          6) [ "$want" = 7 ] || want=6 ;;
          7) want=7 ;;
        esac
      fi
    done
    if [ "$want" != "$applied" ]; then
      setprop debug.oculus.gpuLevel "$want" || log "could not set the GPU level"
      applied=$want
      log "GPU level ${want:-cleared}"
    fi
    cpu_want=
    [ "$want" = 7 ] && cpu_want=8
    if [ "$cpu_want" != "$cpu_applied" ]; then
      setprop debug.oculus.cpuLevel "$cpu_want" || log "could not set the CPU level"
      cpu_applied=$cpu_want
      log "CPU level ${cpu_want:-cleared}"
    fi
    setprop debug.rdrvr.boostd "$(date +%s)"
    sleep 2
  done
  finish "replaced by a newer helper"
}

case ${1:-run} in
  run)
    run
    ;;
  start)
    setsid nohup sh "$0" run >/dev/null 2>&1 </dev/null &
    echo "GPU boost helper starting"
    ;;
  stop)
    if pid=$(running_pid); then
      if ! stop_helper "$pid"; then
        echo "GPU boost helper (pid $pid) did not stop"
        exit 1
      fi
      echo "GPU boost helper stopped"
    else
      rm -f "$PID_FILE"
      echo "GPU boost helper was not running"
    fi
    level=$(getprop debug.oculus.gpuLevel)
    echo "debug.oculus.gpuLevel: ${level:-not set}"
    level=$(getprop debug.oculus.cpuLevel)
    echo "debug.oculus.cpuLevel: ${level:-not set}"
    ;;
  status)
    if pid=$(running_pid); then
      echo "helper: running, pid $pid"
    else
      echo "helper: not running"
    fi
    level=$(getprop debug.oculus.gpuLevel)
    echo "debug.oculus.gpuLevel: ${level:-not set}"
    level=$(getprop debug.oculus.cpuLevel)
    echo "debug.oculus.cpuLevel: ${level:-not set}"
    beat=$(getprop debug.rdrvr.boostd)
    case $beat in
      ''|*[!0-9]*) echo "heartbeat: none" ;;
      *) echo "heartbeat: $(( $(date +%s) - beat )) s ago" ;;
    esac
    for package in com.rockstargames.rdr com.netflix.NGP.Kamo; do
      if pidof "$package" >/dev/null 2>&1; then game=running; else game="not running"; fi
      request=/sdcard/Android/data/$package/files/rdrvr/boost.req
      echo "$package: $game, request: $(cat "$request" 2>/dev/null || echo none)"
    done
    if [ -f "$LOG_FILE" ]; then
      echo "last log lines:"
      tail -n 5 "$LOG_FILE"
    fi
    [ -n "$pid" ]
    ;;
  *)
    echo "usage: sh $0 [run|start|stop|status]"
    exit 2
    ;;
esac
