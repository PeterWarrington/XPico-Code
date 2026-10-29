"""Build XPico Code inside the Windows XP VM. Runs on the guest, not the host.

    python win_guest_build.py http://10.0.2.2:<port>

tools/build_win32.py puts this file on the guest and runs it. The URL is a
throwaway HTTP server on the host, bound to loopback, which QEMU's user-mode
networking exposes to the guest as 10.0.2.2 -- the only channel in or out,
since the VM forwards nothing but telnet and the SPICE WebDAV share is only
served while UTM has the VM window open with a folder picked.

So: fetch the sources from the host, run build.bat and then build_msi.py, and
PUT the .exe and the .msi back. Nothing is read from the checkout the guest
already has in C:\\dev\\xpcode -- the point is to build exactly what is on the
host right now.

Python 3.4 is the newest Python for Windows XP, so no f-strings, no pathlib,
and no subprocess.run.
"""
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

WORK = r'C:\dev\xpcode-build'


def fetch(base, path, dest):
    url = base + '/' + path
    sys.stdout.write('  GET  %s\n' % url)
    urllib.request.urlretrieve(url, dest)


def put(base, path, src):
    with open(src, 'rb') as f:
        body = f.read()
    url = base + '/' + path
    digest = hashlib.sha1(body).hexdigest()
    sys.stdout.write('  PUT  %s (%d bytes, sha1 %s)\n'
                     % (url, len(body), digest[:12]))
    req = urllib.request.Request(url, data=body, method='PUT')
    req.add_header('Content-Type', 'application/octet-stream')
    # The host checks this before keeping the file. Nothing here is hostile --
    # it is a truncated upload we are guarding against, not an attacker.
    req.add_header('X-Artifact-SHA1', digest)
    urllib.request.urlopen(req).read()


def run(cmd, cwd):
    """Run a build step, echoing its output, and stop the build if it fails."""
    sys.stdout.write('  RUN  %s\n' % ' '.join(cmd))
    proc = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    for line in proc.stdout:
        sys.stdout.write('       ' + line.decode('cp1252', 'replace'))
    proc.wait()
    if proc.returncode != 0:
        raise SystemExit('%s failed with %d' % (cmd[-1], proc.returncode))


def main(argv):
    if len(argv) != 2:
        raise SystemExit('usage: win_guest_build.py http://10.0.2.2:<port>')
    base = argv[1].rstrip('/')

    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    os.makedirs(WORK)

    archive = os.path.join(WORK, 'sources.zip')
    fetch(base, 'sources.zip', archive)
    with zipfile.ZipFile(archive) as z:
        z.extractall(WORK)
    os.remove(archive)

    # cmd /c, because build.bat is a batch file and CreateProcess will not run
    # one directly.
    run(['cmd', '/c', 'build.bat'], WORK)
    run([sys.executable, os.path.join('installer', 'build_msi.py')], WORK)

    exe = os.path.join(WORK, 'build', 'xpico-code.exe')
    msis = glob.glob(os.path.join(WORK, 'dist', '*.msi'))
    if not os.path.exists(exe):
        raise SystemExit('build.bat produced no xpico-code.exe')
    if not msis:
        raise SystemExit('build_msi.py produced no .msi')

    put(base, 'artifacts/xpico-code.exe', exe)
    put(base, 'artifacts/' + os.path.basename(msis[0]), msis[0])
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
