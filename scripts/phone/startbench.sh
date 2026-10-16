#!/bin/sh
# Runs on the phone, after scripts/deploy.sh. usage: startbench.sh [runs] [url]
# Launch to the first page frame shown, in ms, per run (/proc/uptime: 10 ms steps).
n=${1:-6}
url=${2:-file:///tmp/ladybird/startbench.html}
printf '<html><body style="background:#3a6"><h1>bench</h1></body></html>' > /tmp/ladybird/startbench.html
i=0
while [ $i -lt $n ]; do
  sh /tmp/ladybird/stop.sh >/dev/null 2>&1; sleep 2
  t0=$(cut -d" " -f1 /proc/uptime | tr -d .)
  PACIFIC_NO_STOP=1 sh /tmp/ladybird/start-browser.sh "$url" 2 >/dev/null 2>&1
  ms=timeout; k=0
  while [ $k -lt 600 ]; do
    if grep -q "EngineView: frames are" /tmp/ladybird/win.log 2>/dev/null; then ms=$(( ($(cut -d" " -f1 /proc/uptime | tr -d .) - t0) * 10 )); break; fi
    sleep 0.05; k=$((k+1))
  done
  echo "run $i: $ms ms"
  i=$((i+1))
done
sh /tmp/ladybird/stop.sh >/dev/null 2>&1
