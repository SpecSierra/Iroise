#!/bin/sh
# Runs on the phone. usage: start-silica.sh <url> [minutes]
# Starts the Silica app, which starts the Ladybird engine itself, and stops both again after a while.
cd /tmp/ladybird
export XDG_RUNTIME_DIR=/run/user/100000
export WAYLAND_DISPLAY=../../display/wayland-0
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/100000/dbus/user_bus_socket
export QT_QPA_PLATFORM=wayland
export LADYBIRD_SFOS_ENGINE=/tmp/ladybird/bin/Ladybird
export LADYBIRD_SFOS_ENGINE_HOME=/tmp/ladybird/home
export LADYBIRD_SFOS_LOG_FPS=1
ulimit -c 0
url=$1; minutes=${2:-15}
sh /tmp/ladybird/stop.sh
nohup /tmp/ladybird/silica/ladybird-silica "$url" > /tmp/ladybird/win.log 2>&1 &
# Only the newest run's timer may stop the browser; an older one would cut this run short.
[ -f /tmp/ladybird/timer.pid ] && kill "$(cat /tmp/ladybird/timer.pid)" 2>/dev/null
( sleep $(( minutes * 60 )); sh /tmp/ladybird/stop.sh ) > /dev/null 2>&1 &
echo $! > /tmp/ladybird/timer.pid
