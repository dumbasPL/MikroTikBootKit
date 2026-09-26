#!/usr/bin/env python3
"""vmconsole.py - drive a RouterOS serial console over a QEMU unix socket.

Usage: vmconsole.py <serial-socket> "<routeros command>"
       vmconsole.py <serial-socket> --shell
       vmconsole.py <serial-socket> --install

Logs in as admin/admin (a fresh image walks a forced password change, the
prompts are answered with "n"/admin), runs the command and prints everything
the console sent back.  --install instead drives the bootkit installer: it
picks the EFI partition that is not the installer's own medium, confirms and
presses a key to reboot.
"""
import re
import socket
import sys
import time

ANSI = re.compile(rb'\x1b\[[0-9;?]*[A-Za-z]|\x1b\][^\x07]*\x07|\x1b[()][0-9A-B]|\x1b[=>]')
PROMPT = rb'\[admin@[^\]]*\]\s*>'
LOGIN = rb'Login:'
PASSWORD = rb'Password:'


def strip(b: bytes) -> str:
    return ANSI.sub(b'', b).decode('utf-8', 'replace')


class Console:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        deadline = time.time() + 10
        while True:
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > deadline:
                    raise
                time.sleep(0.1)
        self.sock.settimeout(0.2)
        self.buf = bytearray()

    def close(self):
        self.sock.close()

    def send(self, data: bytes):
        self.sock.sendall(data)

    def line(self, s: str):
        self.send(s.encode() + b'\r')

    def _read(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                chunk = self.sock.recv(4096)
                if not chunk:
                    return
                self.buf += chunk
            except socket.timeout:
                pass

    def expect(self, pat: bytes, timeout=20.0, show=True, fresh=False):
        """Wait for pat, returning True when found.  With fresh=True only
        text that arrives from now on is searched/shown.  The whole buffer is
        searched, shown marks how much of it was already printed to stdout."""
        if fresh:
            del self.buf[:]
        rx = re.compile(pat, re.I)
        end = time.time() + timeout
        shown = 0
        while time.time() < end:
            if rx.search(bytes(self.buf)):
                if show:
                    sys.stdout.write(strip(bytes(self.buf[shown:])))
                    sys.stdout.flush()
                return True
            self._read(0.2)
            if show and len(self.buf) > shown:
                sys.stdout.write(strip(bytes(self.buf[shown:])))
                sys.stdout.flush()
                shown = len(self.buf)
        return False

    def drain(self, seconds=1.0, show=True):
        self._read(seconds)
        out = strip(bytes(self.buf))
        del self.buf[:]
        if show:
            sys.stdout.write(out)
            sys.stdout.flush()
        return out


def login(c):
    """Return True once the CLI prompt "[admin@...] >" is up."""
    c.drain(0.4, show=False)
    for _ in range(3):
        c.send(b'\r')
        if c.expect(PROMPT, 3, show=False):
            return True
        if not c.expect(LOGIN, 3, show=False):
            continue
        c.line('admin')
        if not c.expect(PASSWORD, 8, show=False):
            continue
        c.line('admin')
        # a forced password change; decline it, then confirm the prompt
        for _ in range(6):
            c._read(1.0)
            text = strip(bytes(c.buf)).lower()
            del c.buf[:]
            if 'change the password' in text or 'new password' in text:
                c.send(b'n\r')
            elif re.search(r'\[admin@[^\]]*\]\s*>', text):
                return True
        if c.expect(PROMPT, 3, show=False):
            return True
    return False


def installer(c):
    """Drive the bootkit installer: pick the non-installer ESP, confirm the
    install and press a key to reboot.  Returns 0 on success."""
    if not c.expect(rb'select the target EFI partition', 60, show=True):
        print('[no selection prompt seen]')
        return 1
    text = strip(bytes(c.buf))
    target = None
    fallback = None
    for m in re.finditer(r'efiboot:\s+(\d+)\)\s*(.*)$', text, re.M):
        fallback = m.group(1)
        if 'this installer' in m.group(2):
            continue
        target = m.group(1)
    if target is None:
        target = fallback          # only the installer's own volume is there
    if target is None:
        print('[no target partition found]')
        return 1
    print(f'[selecting partition {target}]')
    c.send(target.encode() + b'\r')
    if not c.expect(rb'press any key to reboot', 120, show=True):
        print('[install did not finish]')
        return 1
    c.send(b'\r')
    print('[rebooting]')
    time.sleep(2)
    return 0


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    path, cmd = sys.argv[1], sys.argv[2]
    c = Console(path)
    try:
        if cmd == '--install':
            return installer(c)
        if cmd == '--shell':
            import threading

            def pump():
                while True:
                    try:
                        d = c.sock.recv(4096)
                    except socket.timeout:
                        continue
                    if not d:
                        break
                    sys.stdout.write(strip(d))
                    sys.stdout.flush()
            threading.Thread(target=pump, daemon=True).start()
            print('[interactive, Ctrl-C quits]')
            try:
                while True:
                    s = sys.stdin.readline()
                    if s == '':
                        break
                    c.send(s.encode())
            except KeyboardInterrupt:
                pass
            return 0
        if not login(c):
            print('[login failed]')
            return 1
        c.drain(0.8, show=False)
        c.line(cmd)
        c.drain(4.0)
        return 0
    finally:
        c.close()


if __name__ == '__main__':
    sys.exit(main())
