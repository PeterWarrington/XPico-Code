"""Build the Windows release -- both .exes and the .msi -- in the UTM VM.

    python3 tools/build_win32.py

Environment:
    XPCODE_WIN_VM      the UTM VM to build in (default: "Windows XP 2")
    XPCODE_WIN_HOST    host:port of its forwarded telnet (default: 127.0.0.1:2323)
    XPCODE_WIN_USER    telnet login (default: peter)
    XPCODE_WIN_PASS    its password (default: empty, which is what the VM has)
    XPCODE_UTMCTL      path to utmctl

Windows is the native platform for XPico Code and the only one that can build
it: Tiny C Compiler emits the .exe, and installer/build_msi.py drives Windows
Installer through Python's msilib, which exists on no other operating system
(and was dropped from Python 3.13 altogether). The XP VM is therefore not a
convenience, it is the build host.

Two channels reach it, and it takes both:

  - telnet, forwarded to 127.0.0.1:2323, to run commands. There is no ssh for
    XP worth installing, and the VM already runs Microsoft's telnet service
    with a password-less local account, so this speaks telnet directly rather
    than shelling out to a client. Nothing secret crosses it.

  - HTTP, for files, since telnet cannot carry them and the guest has no scp,
    no wget and no unzip. This serves the sources from a throwaway server
    bound to the host's loopback, which QEMU's user-mode networking offers to
    the guest as 10.0.2.2, and the guest PUTs the finished artifacts back the
    same way. The SPICE WebDAV share would do the job too, but only while UTM
    has the VM window open with a folder picked, which a build script cannot
    arrange.

The guest half of this is tools/win_guest_build.py.
"""
import hashlib
import os
import re
import socket
import subprocess
import sys
import threading
import time
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, 'build', 'dist')
GUEST_SCRIPT = os.path.join(ROOT, 'tools', 'win_guest_build.py')

# Everything build.bat, embed_resources.py and build_msi.py read.
SOURCES = [
    'build.bat',
    'LICENSE',
    'res/icon.ico',
    'installer/build_msi.py',
    'tools/embed_resources.py',
]
SOURCE_DIRS = ['src']

VM = os.environ.get('XPCODE_WIN_VM', 'Windows XP 2')
UTMCTL = os.environ.get('XPCODE_UTMCTL',
                        '/Applications/UTM.app/Contents/MacOS/utmctl')
TELNET = os.environ.get('XPCODE_WIN_HOST', '127.0.0.1:2323')
USER = os.environ.get('XPCODE_WIN_USER', 'peter')
PASSWORD = os.environ.get('XPCODE_WIN_PASS', '')

# The guest reaches the host's loopback here, via QEMU's user-mode networking.
HOST_FROM_GUEST = '10.0.2.2'


def app_version():
    with open(os.path.join(ROOT, 'src', 'xpcode.h')) as f:
        return re.search(r'#define APP_VERSION "([^"]+)"', f.read()).group(1)


# --------------------------------------------------------------------------
# telnet
#
# Enough of RFC 854 to get a command prompt out of Microsoft Telnet Service:
# refuse every option the server asks us to turn on, accept the two it offers
# (echo and suppress-go-ahead), and skip subnegotiations.
# --------------------------------------------------------------------------
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
OPT_ECHO, OPT_SGA = 1, 3
# The server drives a VT100 console, so its output is full of cursor control.
ANSI = re.compile(r'\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][0-9A-Za-z]|\x1b[=>]')


class Telnet:
    def __init__(self, host, port, timeout=20):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(1.0)
        self.buf = ''

    def _pump(self):
        try:
            chunk = self.sock.recv(16384)
        except socket.timeout:
            return
        if not chunk:
            raise EOFError('telnet connection closed')
        out = bytearray()
        i = 0
        while i < len(chunk):
            byte = chunk[i]
            if byte != IAC:
                out.append(byte)
                i += 1
                continue
            command = chunk[i + 1] if i + 1 < len(chunk) else 0
            if command in (DO, DONT, WILL, WONT):
                option = chunk[i + 2] if i + 2 < len(chunk) else 0
                if command == WILL:
                    agree = DO if option in (OPT_ECHO, OPT_SGA) else DONT
                    self.sock.sendall(bytes([IAC, agree, option]))
                elif command == DO:
                    self.sock.sendall(bytes([IAC, WONT, option]))
                i += 3
            elif command == SB:
                end = chunk.find(bytes([IAC, SE]), i)
                i = len(chunk) if end < 0 else end + 2
            else:
                i += 2
        self.buf += out.decode('latin-1')

    def expect(self, pattern, timeout=120):
        deadline = time.time() + timeout
        matcher = re.compile(pattern, re.I)
        while time.time() < deadline:
            match = matcher.search(ANSI.sub('', self.buf))
            if match:
                return match
            self._pump()
        raise TimeoutError('timed out waiting for %r; last saw %r'
                           % (pattern, ANSI.sub('', self.buf)[-200:]))

    def send(self, text):
        self.sock.sendall(text.encode('latin-1'))

    def login(self, user, password):
        self.expect(r'login:', timeout=60)
        self.send(user + '\r\n')
        self.expect(r'password:', timeout=30)
        # Forget the prompts we have already answered: the server echoes the
        # login name back, so "login:" would otherwise still be sitting in the
        # buffer and read as a refusal.
        self.buf = ''
        self.send(password + '\r\n')
        # A failed login re-prompts rather than saying anything useful.
        match = self.expect(r'(login:)|([A-Za-z]:\\[^\n]*>)', timeout=60)
        if match.group(1):
            raise SystemExit('telnet login as %r was refused; set '
                             'XPCODE_WIN_USER/XPCODE_WIN_PASS' % user)
        self.buf = ''

    def run(self, command, timeout=900, echo=True):
        """Run one command, stream its output, and return its exit code.

        cmd.exe is asked to print a marker with the exit code in it, which is
        both the end-of-output signal and the result -- there is no other way
        to get a status back over a console session.
        """
        self.buf = ''
        self.send(command + '\r\n')
        self.send('echo ___RC=%ERRORLEVEL%___\r\n')
        shown = 0
        pattern = re.compile(r'___RC=(-?\d+)___')
        deadline = time.time() + timeout
        while time.time() < deadline:
            text = ANSI.sub('', self.buf).replace('\r', '')
            match = pattern.search(text)
            if echo:
                upto = match.start() if match else len(text)
                fresh = text[shown:upto]
                if '\n' in fresh:
                    keep = fresh.rsplit('\n', 1)[0]
                    for line in keep.split('\n'):
                        # The console echoes back what we typed, including the
                        # marker command itself.
                        if line.strip() and '___RC=' not in line \
                                and line.strip() != command.strip():
                            print('       ' + line.rstrip())
                    shown += len(keep) + 1
            if match:
                return int(match.group(1))
            self._pump()
        raise TimeoutError('command timed out after %ds: %s' % (timeout, command))


# --------------------------------------------------------------------------
# the file channel
# --------------------------------------------------------------------------
def source_zip():
    """Zip up what the Windows build reads, laid out as it is in the repo."""
    buf = BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
        for rel in SOURCES:
            z.write(os.path.join(ROOT, rel), rel)
        for folder in SOURCE_DIRS:
            for dirpath, _, names in os.walk(os.path.join(ROOT, folder)):
                for name in sorted(names):
                    if name.startswith('.'):
                        continue
                    full = os.path.join(dirpath, name)
                    z.write(full, os.path.relpath(full, ROOT))
    return buf.getvalue()


class Channel(ThreadingHTTPServer):
    """Serves the sources, collects the artifacts, and says nothing else."""
    daemon_threads = True

    def __init__(self, payload, guest_script, on_artifact):
        super().__init__(('127.0.0.1', 0), Handler)
        self.payload = payload
        self.guest_script = guest_script
        self.on_artifact = on_artifact
        self.received = {}
        self.failures = []


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.0'

    def log_message(self, *args):
        pass

    def _send(self, body, status=200):
        self.send_response(status)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == '/sources.zip':
            self._send(self.server.payload)
        elif self.path == '/guest.py':
            self._send(self.server.guest_script)
        else:
            self._send(b'no', 404)

    def do_PUT(self):
        name = os.path.basename(self.path)
        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length)
        if not self.path.startswith('/artifacts/') or not name:
            self._send(b'no', 404)
            return
        claimed = self.headers.get('X-Artifact-SHA1', '')
        actual = hashlib.sha1(body).hexdigest()
        if claimed and claimed != actual:
            self.server.failures.append(
                '%s: arrived corrupt (sha1 %s, expected %s)'
                % (name, actual[:12], claimed[:12]))
            self._send(b'corrupt', 400)
            return
        try:
            self.server.on_artifact(name, body)
            self.server.received[name] = len(body)
            self._send(b'ok')
        except Exception as exc:                        # noqa: BLE001
            self.server.failures.append('%s: %s' % (name, exc))
            self._send(b'failed', 500)


# --------------------------------------------------------------------------
def vm_status():
    try:
        out = subprocess.run([UTMCTL, 'status', VM], capture_output=True,
                             text=True)
    except OSError:
        raise SystemExit('utmctl not found at %s -- is UTM installed?' % UTMCTL)
    if out.returncode != 0:
        listing = subprocess.run([UTMCTL, 'list'], capture_output=True,
                                 text=True).stdout
        raise SystemExit('no UTM VM called %r. Set XPCODE_WIN_VM to one of:\n%s'
                         % (VM, listing))
    return out.stdout.strip()


def start_vm():
    status = vm_status()
    if status == 'started':
        print('  VM   %r already running' % VM)
        return
    print('  VM   starting %r (%s)' % (VM, status))
    subprocess.run([UTMCTL, 'start', VM])
    # Resuming a suspended VM does not always work -- UTM drops a saved state
    # it cannot restore, leaving the VM stopped while the start still looks
    # like it succeeded. Check, rather than wait ten minutes for telnet from a
    # machine that is not running; and if the saved state is what is broken,
    # throw it away and boot from cold instead, which always works.
    if vm_status() != 'started':
        print("  VM   %r did not come up; discarding its saved state" % VM)
        subprocess.run([UTMCTL, 'discard-state', VM],
                       capture_output=True)
        subprocess.run([UTMCTL, 'start', VM])
    if vm_status() != 'started':
        raise SystemExit('%r will not start. Open it in UTM and look at its '
                         'console.' % VM)


def wait_for_telnet(host, port, timeout=600):
    print('  VM   waiting for telnet on %s:%d' % (host, port))
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=5):
                return
        except OSError:
            time.sleep(5)
    raise SystemExit('%s:%d never answered. Check the VM console, and that '
                     'the telnet service is running in the guest.'
                     % (host, port))


def main():
    version = app_version()
    host, _, port = TELNET.partition(':')
    port = int(port or 23)

    os.makedirs(DIST, exist_ok=True)
    # The release names, which is the only thing the host renames.
    wanted = {
        'xpico-code.exe': 'XPicoCode-v%s-x86-win32.exe' % version,
        'xpico-code-win95.exe': 'XPicoCode-v%s-x86-win95.exe' % version,
        'XPicoCode-%s.msi' % version: 'XPicoCode-v%s-x86-win32.msi' % version,
    }
    written = []

    def on_artifact(name, body):
        target = wanted.get(name)
        if target is None:
            raise ValueError('unexpected artifact')
        path = os.path.join(DIST, target)
        with open(path, 'wb') as f:
            f.write(body)
        written.append(path)
        print('  GOT  %s (%d bytes)' % (os.path.relpath(path, ROOT), len(body)))

    with open(GUEST_SCRIPT, 'rb') as f:
        guest_script = f.read()

    start_vm()
    wait_for_telnet(host, port)

    channel = Channel(source_zip(), guest_script, on_artifact)
    base = 'http://%s:%d' % (HOST_FROM_GUEST, channel.server_address[1])
    print('  SRV  serving sources on 127.0.0.1:%d (%s to the guest)'
          % (channel.server_address[1], base))
    threading.Thread(target=channel.serve_forever, daemon=True).start()

    try:
        session = Telnet(host, port)
        session.login(USER, PASSWORD)
        print('  TEL  logged in as %s' % USER)

        # Bootstrap: pull the guest half over, then let it do the rest. The
        # one-liner is the only thing that has to be typed blind.
        fetch = ('python -c "import urllib.request;urllib.request.urlretrieve('
                 "'%s/guest.py',r'%%TEMP%%\\xpcode_guest.py')\"" % base)
        if session.run(fetch, timeout=120) != 0:
            raise SystemExit('could not copy the guest script into the VM')

        print('  WIN  building in the VM')
        code = session.run('python %TEMP%\\xpcode_guest.py ' + base,
                           timeout=1800)
        if code != 0:
            raise SystemExit('the Windows build failed (exit %d)' % code)
    finally:
        channel.shutdown()

    if channel.failures:
        raise SystemExit('could not save: ' + '; '.join(channel.failures))
    missing = sorted(set(wanted.values()) - {os.path.basename(p)
                                             for p in written})
    if missing:
        raise SystemExit('the VM never sent: ' + ', '.join(missing))

    print()
    print('Windows build succeeded:')
    for path in written:
        print('  %s' % os.path.relpath(path, ROOT))
    return 0


if __name__ == '__main__':
    sys.exit(main())
