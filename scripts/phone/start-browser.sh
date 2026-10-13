#!/bin/sh
# Runs on the phone. usage: start-browser.sh [url] [minutes]
# Starts the full browser interface from /tmp/ladybird/browser. It starts the Ladybird engine itself. Everything
# either of them writes goes under /tmp, and both are stopped again after a while.
cd /tmp/ladybird
export XDG_RUNTIME_DIR=/run/user/100000
export WAYLAND_DISPLAY=../../display/wayland-0
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/100000/dbus/user_bus_socket
export QT_QPA_PLATFORM=wayland
export XDG_DATA_HOME=/tmp/ladybird/browser-home/data
export XDG_CACHE_HOME=/tmp/ladybird/browser-home/cache
export PACIFIC_ENGINE=/tmp/ladybird/bin/Ladybird
export PACIFIC_ENGINE_HOME=/tmp/ladybird/home
export PACIFIC_DEPLOYMENT_PATH=/tmp/ladybird/browser/share/
export PACIFIC_RUNTIME_LIBRARY=/tmp/ladybird/browser/lib/libsailfishbrowser.so.1
export PACIFIC_TRANSLATION_PATH=/tmp/ladybird/browser/translations/
export LADYBIRD_SFOS_LOG_FPS=1
# Without this, Qt sends warnings (QML errors included) to the journal instead of our log.
export QT_LOGGING_TO_CONSOLE=1
export PACIFIC_LOG_ENGINE=${PACIFIC_LOG_ENGINE:-}
export PACIFIC_GRAB_DIR=${PACIFIC_GRAB_DIR:-}
ulimit -c 0
mkdir -p "$XDG_DATA_HOME" "$XDG_CACHE_HOME"
url=$1; minutes=${2:-15}
sh /tmp/ladybird/stop.sh
( /tmp/ladybird/browser/bin/pacific-browser $url > /tmp/ladybird/win.log 2>&1
  echo "pacific-browser exited with status $?" >> /tmp/ladybird/win.log ) &
# Only the newest run's timer may stop the browser; an older one would cut this run short.
[ -f /tmp/ladybird/timer.pid ] && kill "$(cat /tmp/ladybird/timer.pid)" 2>/dev/null
( sleep $(( minutes * 60 )); sh /tmp/ladybird/stop.sh ) > /dev/null 2>&1 &
echo $! > /tmp/ladybird/timer.pid
