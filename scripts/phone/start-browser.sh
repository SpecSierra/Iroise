#!/bin/sh
# Runs on the phone. usage: start-browser.sh [url] [minutes]
# Starts the app from /tmp/ladybird (scripts/deploy.sh). Everything it writes goes under /tmp, and it is stopped
# again after a while.
cd /tmp/ladybird || exit 1
export XDG_RUNTIME_DIR=/run/user/100000
export WAYLAND_DISPLAY=../../display/wayland-0
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/100000/dbus/user_bus_socket
export QT_QPA_PLATFORM=wayland
export XDG_DATA_HOME=/tmp/ladybird/browser-home/data
export XDG_CACHE_HOME=/tmp/ladybird/browser-home/cache
export IROISE_ENGINE_HOME=/tmp/ladybird/home
export LADYBIRD_SFOS_LOG_FPS=1
# Without this, Qt sends warnings (QML errors included) to the journal instead of our log.
export QT_LOGGING_TO_CONSOLE=1
export IROISE_LOG_ENGINE=${IROISE_LOG_ENGINE:-}
export IROISE_GRAB_DIR=${IROISE_GRAB_DIR:-}
ulimit -c 0
mkdir -p "$XDG_DATA_HOME" "$XDG_CACHE_HOME"
url=$1; minutes=${2:-15}
case $minutes in ''|*[!0-9]*) echo "minutes must be a number: $minutes" >&2; exit 1 ;; esac
# IROISE_NO_STOP=1: the caller has stopped it already (stop.sh waits a second, which timings would count).
[ -n "$IROISE_NO_STOP" ] || sh /tmp/ladybird/stop.sh
# NB: $url unquoted on purpose: it may carry arguments before the URL (-qmljsdebugger=...). A URL with spaces or
#     glob characters has to be quoted by the caller.
( /tmp/ladybird/bin/harbour-iroise $url > /tmp/ladybird/win.log 2>&1
  echo "harbour-iroise exited with status $?" >> /tmp/ladybird/win.log ) &
# Only the newest run's timer may stop the browser; an older one would cut this run short.
[ -f /tmp/ladybird/timer.pid ] && kill "$(cat /tmp/ladybird/timer.pid)" 2>/dev/null
( sleep $(( minutes * 60 )); sh /tmp/ladybird/stop.sh ) > /dev/null 2>&1 &
echo $! > /tmp/ladybird/timer.pid
