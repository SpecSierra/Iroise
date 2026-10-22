#!/usr/bin/env python3
"""Runs the instrumented engine (scripts/build.sh with IROISE_PGO=generate) through what a phone's browser does
most, so that its profile tells gcc which code is hot: real sites loaded and scrolled, then Speedometer for the
DOM and JavaScript work of web apps.

usage: IROISE_ENGINE=<instrumented Ladybird executable> pgo-train.py [seconds for Speedometer, default 420]

The sites are fetched live: nothing of theirs is kept. One that fails to load only teaches less. The engine runs
without its sandbox, which would keep its processes from writing the profile, and is stopped the way the app stops
it, so that every process writes its part.
"""

import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tests', 'embed'))
from embed import Engine  # noqa: E402

# What people read on a phone: news, social, video, reference, shopping, code.
SITES = [
    'https://en.wikipedia.org/wiki/Sailfish_OS',
    'https://edition.cnn.com/',
    'https://www.bbc.com/news',
    'https://www.theverge.com/',
    'https://www.reddit.com/r/sailfishos/',
    'https://m.youtube.com/',
    'https://news.ycombinator.com/',
    'https://github.com/LadybirdBrowser/ladybird',
    'https://www.lemonde.fr/',
    'https://www.amazon.com/',
    'https://stackoverflow.com/questions',
    'https://forum.sailfishos.org/latest',
]
SPEEDOMETER = 'https://browserbench.org/Speedometer3.1/?startAutomatically'


def visit(engine, url, scrolls=8):
    started = time.monotonic()
    try:
        engine.load(url)
    except Exception as error:  # A slow or failing site still ran code: carry on with what it has.
        print(f'  {url}: did not finish loading ({type(error).__name__})', flush=True)
    engine.pump(3)
    for _ in range(scrolls):
        engine.drag((540, 1600), (540, 500), seconds=0.25, then=0.4)
    for _ in range(2):
        engine.drag((540, 500), (540, 1600), seconds=0.25, then=0.4)
    print(f'  {url}: {time.monotonic() - started:.0f} s', flush=True)


def main():
    speedometer_seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 420
    engine = Engine(arguments=['--disable-sandbox'])
    try:
        for url in SITES:
            try:
                visit(engine, url)
            except ConnectionError:  # The engine went away: what it wrote stays, a new one goes on.
                print(f'  {url}: the engine went away; starting another', flush=True)
                engine.close(gracefully=True)
                engine.remove()
                engine = Engine(arguments=['--disable-sandbox'])
        print(f'  Speedometer for {speedometer_seconds} s', flush=True)
        try:
            engine.load(SPEEDOMETER)
        except Exception as error:
            print(f'  Speedometer did not finish loading ({type(error).__name__})', flush=True)
        engine.pump(speedometer_seconds)
    finally:
        engine.close(gracefully=True)
        engine.remove()


if __name__ == '__main__':
    main()
