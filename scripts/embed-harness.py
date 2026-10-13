#!/usr/bin/env python3
"""Drive the engine through the embedding protocol, without the browser interface or a phone.

Usage: embed-harness.py ENGINE URL [engine arguments...]

Starts the engine as the interface would (ui/protocol/EmbedProtocol.h), creates one view, loads URL, taps once at
(300, 400) and prints every message that comes back except frames. On a host without a GPU the engine paints into
plain bitmaps and sends no frames, which does not matter for what this is for: navigation, downloads, dialogs.
Run it in an empty directory: it creates embed.sock, embed.log and a dlhome/ that stands in for $HOME, and the
host build needs --disable-sandbox.
"""
import os, socket, struct, subprocess, sys, time, select
HDR = '<IiiiiIdII'
def msg(t, view=1, a=0, b=0, c=0, d=0, scale=0.0, text=b''):
    return struct.pack(HDR, t, a, b, c, d, 0, scale, len(text), view) + text
path = os.path.abspath('embed.sock')
if os.path.exists(path): os.unlink(path)
srv = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET); srv.bind(path); srv.listen(1)
home = os.path.abspath('dlhome')
os.makedirs(home + '/Downloads', exist_ok=True)
env = dict(os.environ, LADYBIRD_SFOS_EMBED_SOCKET=path, HOME=home, XDG_DOWNLOAD_DIR=home + '/Downloads', LADYBIRD_UI_SFOS='1')
proc = subprocess.Popen([sys.argv[1]] + sys.argv[3:], env=env, stdout=open('embed.log', 'w'), stderr=subprocess.STDOUT)
srv.settimeout(20); conn, _ = srv.accept()
conn.send(msg(7, a=1032, b=2110, scale=2.5))                     # create view
conn.send(msg(1, text=sys.argv[2].encode()))                     # load
names = {101:'FRAME',102:'URL',103:'TITLE',104:'LOADING',105:'HISTORY',114:'DOWNLOAD',115:'PAGE_COLOR',107:'CRASHED'}
def pump(seconds):
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([conn], [], [], 0.2)
        if not r: continue
        data = conn.recv(70000)
        if not data: print('engine went away'); return
        t, a, b, c, d, e, scale, tl, view = struct.unpack(HDR, data[:40])
        if t != 101: print(names.get(t, t), 'a=%d b=%d' % (a, b), data[40:40+tl][:200].decode(errors='replace'))
pump(4)
print('-- tap')
conn.send(msg(2, a=0, b=0, c=300, d=400)); conn.send(msg(2, a=2, b=0, c=300, d=400))
pump(12)
proc.terminate()
