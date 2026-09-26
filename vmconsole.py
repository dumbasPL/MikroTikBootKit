#!/usr/bin/env python3
"""vmconsole.py - drive a RouterOS serial console over a QEMU unix socket.

Usage: vmconsole.py <serial-socket> "<routeros command>"
       vmconsole.py <serial-socket> --shell
       vmconsole.py <serial-socket> --install [MODE]
       vmconsole.py <serial-socket> --ros-install
       vmconsole.py <serial-socket> --ros-firstboot

Logs in as admin/admin (a fresh image walks a forced password change, the
prompts are answered with "n"/admin), runs the command and prints everything
the console sent back.  --install instead drives the bootkit installer: it
picks the EFI partition that is not the installer's own medium, selects the
install mode (MODE 1 = direct, the default, 2 = removable), confirms and
presses a key to reboot.  --ros-install drives the stock RouterOS installer
from an installer ISO: it takes the default package selection, confirms the
disk wipe and waits for the install to finish.  --ros-firstboot does the first
login of a freshly installed image: the admin password is still empty, the
licence question is answered with "y" and the agreement pager is quit with
q + Enter, and the forced password change ends with admin/admin.
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


def skip_pager(c):
    """Quit the licence agreement pager with q + Enter instead of scrolling
    through every page.  Returns True once the pager is gone; the pager footer
    is dropped and the text that follows stays in the console buffer."""
    footer = b'press Enter (q to abort)'
    end = re.compile(rb'(Please press "Enter" to continue!|Change your password|'
                     rb'new password>|\[admin@[^\]]*\]\s*>)')
    for _ in range(3):
        c.send(b'q')
        c.send(b'\r')
        if not c.expect(rb'(press Enter \(q to abort\)|Please press "Enter" to continue!|'
                        rb'Change your password|new password>|\[admin@[^\]]*\]\s*>)',
                        30):
            return False
        text = bytes(c.buf)
        del c.buf[:]
        pos = text.rfind(footer)
        tail = text[pos + len(footer):] if pos >= 0 else text
        if end.search(tail):
            c.buf += tail               # keep only what follows the pager
            return True
        # the footer is still the last thing seen: the q did not take, retry
    return False


def fresh_login(c, show=False, login_timeout=5):
    """Log into a never-booted image.  admin has no password yet and RouterOS
    walks its first-login flow: the licence question is answered with "y", the
    agreement pager is quit with q + Enter (no need to scroll it), the "no
    software key" notice is acknowledged and the forced password change is set
    to admin/admin.  Returns True once the CLI prompt is up."""
    if not c.expect(rb'(Do you want to see the software license\? \[Y/n\]:|Login:)',
                    login_timeout, show=show, fresh=True):
        c.send(b'\r')                   # provoke a login prompt
        if not c.expect(rb'(Do you want to see the software license\? \[Y/n\]:|Login:)',
                        30, show=show, fresh=True):
            return False
    if 'software license' in strip(bytes(c.buf)):
        c.send(b'y')
        del c.buf[:]
        if show:
            print('[dismissing the licence agreement pager]')
        if not skip_pager(c):
            return False
        if not c.expect(rb'(Please press "Enter" to continue!|Login:)', 60, show=show):
            return False
        if 'Please press "Enter"' in strip(bytes(c.buf)):
            c.send(b'\r')
            del c.buf[:]
            if not c.expect(LOGIN, 30, show=show):
                return False
    c.line('admin')
    if not c.expect(PASSWORD, 30, show=show):
        return False
    c.send(b'\r')                       # the password is still empty
    del c.buf[:]
    # whatever the first login puts on the console, answer it until the prompt
    for _ in range(8):
        if not c.expect(rb'(software license\? \[Y/n\]:|press Enter \(q to abort\)|'
                        rb'Please press "Enter" to continue!|new password>|'
                        rb'\[admin@[^\]]*\]\s*>)', 45, show=show):
            return False
        text = strip(bytes(c.buf))
        del c.buf[:]
        if re.search(r'\[admin@[^\]]*\]\s*>', text):
            return True
        if 'new password>' in text:
            c.line('admin')
            if not c.expect(rb'repeat new password>', 30, show=show):
                return False
            c.line('admin')
        elif 'Please press "Enter" to continue!' in text:
            c.send(b'\r')               # the "no software key" notice
        elif 'software license' in text or 'press Enter (q to abort)' in text:
            if 'software license' in text:
                c.send(b'y')
            if show:
                print('[dismissing the licence agreement pager]')
            if not skip_pager(c):
                return False
        else:
            c.send(b'\r')
    return False


def ros_firstboot(c):
    """--ros-firstboot: run the first login of a freshly installed image (see
    fresh_login).  Returns 0 on success."""
    if fresh_login(c, show=True, login_timeout=180):
        return 0
    print('[first login failed]')
    return 1


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
    # admin/admin did not work; a never-booted image is empty-password
    return fresh_login(c)


def installer(c, mode='1'):
    """Drive the bootkit installer: pick the non-installer ESP and the install
    mode (1 direct / 2 removable), confirm and press a key to reboot.
    Returns 0 on success."""
    if not c.expect(rb'select the EFI partition where RouterOS is installed', 60,
                    show=True):
        print('[no target prompt seen]')
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
    if not c.expect(rb'select mode', 60, show=True):
        print('[no mode prompt seen]')
        return 1
    print(f'[selecting mode {mode}]')
    c.send(mode.encode() + b'\r')
    if not c.expect(rb'press any key to reboot', 180, show=True):
        print('[install did not finish]')
        return 1
    c.send(b'\r')
    print('[rebooting]')
    time.sleep(2)
    return 0


def ros_installer(c):
    """Drive the stock RouterOS installer from the installer ISO: wait for the
    package menu, install the default selection, confirm the disk wipe and
    wait for the reboot prompt.  Returns 0 on success."""
    if not c.expect(rb'Welcome to MikroTik Router Software installation', 300,
                    show=True):
        print('[no installer menu seen]')
        return 1
    if not c.expect(rb'cancel and reboot', 60, show=True):
        print('[no package menu seen]')
        return 1
    print('[installing the default selection]')
    c.send(b'i')
    if not c.expect(rb'Continue\? \[y/n\]', 120, show=True):
        print('[no disk confirmation seen]')
        return 1
    print('[confirming the disk wipe]')
    c.send(b'y')
    if not c.expect(rb'Press ENTER to reboot', 900, show=True):
        print('[install did not finish]')
        return 1
    c.send(b'\r')
    print('[installed, rebooting]')
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
            return installer(c, sys.argv[3] if len(sys.argv) > 3 else '1')
        if cmd == '--ros-install':
            return ros_installer(c)
        if cmd == '--ros-firstboot':
            return ros_firstboot(c)
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
