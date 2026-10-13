#!/bin/sh
# Runs on the phone. usage: start.sh <url> [minutes]
# Starts the windowed browser from /tmp/ladybird and stops it again after a while,
# so a forgotten test run does not sit on the user's phone.
cd /tmp/ladybird
export HOME=/tmp/ladybird/home XDG_RUNTIME_DIR=/run/user/100000
export WAYLAND_DISPLAY=../../display/wayland-0
export XDG_CACHE_HOME=$HOME/.cache XDG_DATA_HOME=$HOME/.local/share XDG_CONFIG_HOME=$HOME/.config
# The J2 kernel has no Landlock; this keeps the seccomp half of the sandbox.
export LADYBIRD_UNSAFE_ALLOW_MISSING_LANDLOCK=1
export LADYBIRD_SFOS_LOG_FPS=1
mkdir -p $HOME
sh /tmp/ladybird/stop.sh
nohup ./run.sh "$1" > /tmp/ladybird/win.log 2>&1 &
# Only the newest run's timer may stop the browser; an older one would cut this run short.
[ -f /tmp/ladybird/timer.pid ] && kill "$(cat /tmp/ladybird/timer.pid)" 2>/dev/null
( sleep $(( ${2:-15} * 60 )); sh /tmp/ladybird/stop.sh ) > /dev/null 2>&1 &
echo $! > /tmp/ladybird/timer.pid
