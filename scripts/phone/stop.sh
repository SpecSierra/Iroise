#!/bin/sh
# Runs on the phone: stop the app from /tmp/ladybird and every engine process it started (all the same executable).
pkill -f '^/tmp/ladybird/bin/harbour-iroise' 2>/dev/null
sleep 1
pkill -9 -f '^/tmp/ladybird/bin/harbour-iroise' 2>/dev/null
true
