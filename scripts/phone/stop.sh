#!/bin/sh
# Runs on the phone: stop the browser and its helper processes.
pkill -f '/tmp/ladybird/bin/' 2>/dev/null
sleep 1
pkill -9 -f '/tmp/ladybird/(bin|libexec)/' 2>/dev/null
true
