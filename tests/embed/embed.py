"""The engine, driven as Pacific's interface drives it: over the embedding protocol (ui/protocol/EmbedProtocol.h), with
no interface, no Qt and no phone. Used by the tests in this directory (scripts/test-front-end.sh).

The engine is the bundle's own executable in its engine role (scripts/bundle.sh lays the bundle out). On a host
without the phone's GPU it paints into plain bitmaps and sends no frames, which these tests do not need.
"""

import http.server
import json
import os
import re
import select
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PAGES = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pages')
HEADER = '<IiiiiIdII'
HEADER_SIZE = struct.calcsize(HEADER)
PAGE_MESSAGE_MARKER = '\u0001pacific'

# The message types and their arguments, from the protocol header itself, so that the tests cannot drift from it.
_protocol = open(os.path.join(ROOT, 'ui', 'protocol', 'EmbedProtocol.h')).read()
_constants = {name: int(number) for name, number in re.findall(r'LB_EMBED_(\w+) = (\d+)', _protocol)}
# NB: The enum ends at a "};" of its own line; its comments hold JSON that has some too.
_types = re.search(r'enum lb_embed_message_type \{(.*?)\n\};', _protocol, re.DOTALL).group(1)
NAMES = {int(number): name for name, number in re.findall(r'LB_EMBED_(\w+) = (\d+)', _types)}
TYPES = {name: number for number, name in NAMES.items()}


def constant(name):
    return _constants[name]


class Message:
    def __init__(self, data):
        self.type, self.a, self.b, self.c, self.d, self.e, self.scale, length, self.view = struct.unpack(HEADER, data[:HEADER_SIZE])
        self.text = data[HEADER_SIZE:HEADER_SIZE + length].decode(errors='replace')
        self.time = time.monotonic()

    @property
    def name(self):
        return NAMES.get(self.type, str(self.type))

    def __repr__(self):
        return f'{self.name}(a={self.a}, b={self.b}, text={self.text[:80]!r})'


class Engine:
    """One engine with one view, sized like the Jolla Phone 2's screen at a device pixel ratio of 2, so that device
    pixels are twice CSS pixels."""

    WIDTH = 1080
    HEIGHT = 2000
    SCALE = 2.0
    VIEW = 1

    def __init__(self, arguments=()):
        executable = os.environ.get('PACIFIC_ENGINE', os.path.join(ROOT, 'dist', 'harbour-pacific', 'bin', 'harbour-pacific'))
        self.directory = tempfile.mkdtemp(prefix='pacific-embed-')
        self.home = os.path.join(self.directory, 'home')
        self.downloads = os.path.join(self.home, 'Downloads')
        os.makedirs(self.downloads)
        path = os.path.join(self.directory, 'embed.sock')
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        listener.bind(path)
        listener.listen(1)
        # NB: A runtime directory of its own too: CI's service user has none, and the engine would make one in /run.
        runtime = os.path.join(self.directory, 'runtime')
        os.makedirs(runtime, mode=0o700)
        environment = dict(os.environ, LADYBIRD_SFOS_EMBED_SOCKET=path, LADYBIRD_UNSAFE_ALLOW_MISSING_LANDLOCK='1',
            XDG_RUNTIME_DIR=runtime, HOME=self.home, XDG_CACHE_HOME=self.home + '/.cache', XDG_CONFIG_HOME=self.home + '/.config',
            XDG_DATA_HOME=self.home + '/.local/share', XDG_DOWNLOAD_DIR=self.downloads)
        self.log_path = os.path.join(self.directory, 'engine.log')
        with open(self.log_path, 'w') as log:
            self.process = subprocess.Popen([executable, '--engine-role', 'Ladybird', *arguments], env=environment,
                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        listener.settimeout(30)
        try:
            self.connection, _ = listener.accept()
        except socket.timeout:
            raise TimeoutError('the engine did not connect within 30 s:\n' + self.log()[-4000:]) from None
        finally:
            listener.close()
        self.messages = []
        self._seen = 0
        self.send('CREATE_VIEW', a=self.WIDTH, b=self.HEIGHT, scale=self.SCALE)
        self.send('SET_VISIBLE', a=1)

    def close(self, gracefully=False, timeout=60):
        """Stops the engine. Gracefully: only closes its socket, which ends it the way the app does on the phone,
        and waits for every process it started to end too."""
        if gracefully:
            self.connection.close()
            end = time.monotonic() + timeout
            while self._session_processes() and time.monotonic() < end:
                time.sleep(0.2)
        try:
            os.killpg(self.process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            self.process.wait(10)
        except subprocess.TimeoutExpired:
            os.killpg(self.process.pid, signal.SIGKILL)
        self.connection.close()

    def log(self):
        with open(self.log_path, errors='replace') as log:
            return log.read()

    def remove(self):
        shutil.rmtree(self.directory, ignore_errors=True)

    # Talking to the engine.

    def send(self, type, a=0, b=0, c=0, d=0, scale=0.0, text='', view=None):
        data = text.encode()
        self.connection.send(struct.pack(HEADER, TYPES[type], a, b, c, d, 0, scale, len(data), self.VIEW if view is None else view) + data)

    def pump(self, seconds):
        """Takes in what the engine sends for that long."""
        end = time.monotonic() + seconds
        while True:
            left = end - time.monotonic()
            readable, _, _ = select.select([self.connection], [], [], max(0, min(left, 0.05)))
            if readable:
                data = self.connection.recv(1 << 20)
                if not data:
                    raise ConnectionError('the engine went away:\n' + self.log()[-4000:])
                message = Message(data)
                if message.name != 'FRAME':
                    self.messages.append(message)
            elif left <= 0:
                return

    def wait_for(self, name, predicate=lambda message: True, timeout=15):
        """The first message of that type, from those not looked at yet, that the predicate accepts."""
        end = time.monotonic() + timeout
        while True:
            for index in range(self._seen, len(self.messages)):
                message = self.messages[index]
                if message.name == name and predicate(message):
                    self._seen = index + 1
                    return message
            if time.monotonic() > end:
                raise TimeoutError(f'no {name} within {timeout} s; since the last one looked at: {self.messages[self._seen:][-20:]}')
            self.pump(0.05)

    def received(self, name, predicate=lambda message: True):
        """Whether such a message came, of all of them."""
        return any(message.name == name and predicate(message) for message in self.messages)

    # What the interface does.

    def load(self, url):
        """Loads the URL and waits for the load to end. What the load brought (its title, its history) is looked at
        from its start again, so that wait_for finds it."""
        # NB: Each of these follows the one before, so that a late message of the page before is not taken for one
        #     of this load's.
        self.send('LOAD', text=url)
        self.wait_for('URL', lambda message: message.text.split('#')[0] == url.split('#')[0])
        start = self._seen - 1
        self.wait_for('COMMIT')
        self.wait_for('LOADING', lambda message: message.a == 0)
        # The page's own scripts settle.
        self.pump(0.3)
        self._seen = start

    _script_number = 0

    def eval(self, expression, timeout=10):
        """The value of a JavaScript expression in the page, as JSON."""
        Engine._script_number += 1
        tag = f'\u0001result{Engine._script_number}:'
        self.send('RUN_SCRIPT', text=f'{json.dumps(tag)} + JSON.stringify((() => ({expression}))())')
        message = self.wait_for('SCRIPT_RESULT', lambda message: message.text.startswith(tag), timeout)
        return json.loads(message.text[len(tag):])

    def touch(self, phase, x, y, finger=0):
        self.send('TOUCH', a=constant('TOUCH_' + phase), b=finger, c=round(x), d=round(y))

    def tap(self, x, y):
        self.touch('DOWN', x, y)
        self.pump(0.05)
        self.touch('UP', x, y)
        self.pump(0.3)

    def drag(self, start, end, seconds=0.3, steps=20, then=0.5):
        """A finger from `start` to `end`, in device pixels, moving evenly for that long; then the engine has `then`
        seconds of its own."""
        (x0, y0), (x1, y1) = start, end
        self.touch('DOWN', x0, y0)
        for step in range(1, steps + 1):
            self.pump(seconds / steps)
            self.touch('MOTION', x0 + (x1 - x0) * step / steps, y0 + (y1 - y0) * step / steps)
        self.pump(seconds / steps)
        self.touch('UP', x1, y1)
        self.pump(then)

    def _session_processes(self):
        processes = []
        for pid in os.listdir('/proc'):
            if not pid.isdigit():
                continue
            try:
                with open(f'/proc/{pid}/stat') as file:
                    fields = file.read().rsplit(')', 1)[1].split()
            except OSError:
                continue
            if int(fields[3]) == self.process.pid and fields[0] != 'Z':
                processes.append(int(pid))
        return processes

    def page_processes(self):
        """The engine's WebContent processes, by pid."""
        pids = []
        for pid in os.listdir('/proc'):
            if not pid.isdigit():
                continue
            try:
                with open(f'/proc/{pid}/cmdline', 'rb') as file:
                    arguments = file.read().split(b'\0')
                with open(f'/proc/{pid}/stat') as file:
                    session = int(file.read().rsplit(')', 1)[1].split()[3])
            except OSError:
                continue
            if session == os.getsid(self.process.pid) and b'WebContent' in arguments:
                pids.append(int(pid))
        return pids


def page(name):
    return 'file://' + os.path.join(PAGES, name)


class Server:
    """Serves the pages over HTTP, for what a file: URL cannot do (downloads)."""

    def __init__(self):
        handler = lambda *arguments: _Handler(*arguments, directory=PAGES)
        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.base = f'http://127.0.0.1:{self.server.server_address[1]}/'

    def url(self, name):
        return self.base + name

    def close(self):
        self.server.shutdown()


class _Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        if self.path.endswith('.bin'):
            self.send_header('Content-Disposition', 'attachment')
        super().end_headers()

    def log_message(self, *arguments):
        pass
