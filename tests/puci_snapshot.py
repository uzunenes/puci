#!/usr/bin/env python3
"""puci_snapshot.py — render a text snapshot of the live TUI screen at a given
terminal size, by emulating the termbox escape stream from a real PTY run.

Usage:
  python3 tests/puci_snapshot.py --rows 24 --cols 80 [--prompt "hello"]
"""
import argparse, os, re, sys, time, subprocess, pty, fcntl, termios, struct, select, signal

HERE = os.path.dirname(os.path.abspath(__file__))
FAKE = os.path.join(HERE, "fake_sse.py")
PUCI = os.environ.get("PUCI_BIN") or os.path.abspath(os.path.join(HERE, "..", "build", "src", "cli", "puci"))

def start_fake(tokens=40, rate=50, text=""):
    env = dict(os.environ)
    args = [sys.executable, "-u", FAKE, "--port", "0",
            "--tokens", str(tokens), "--rate", str(rate)]
    if text:
        args += ["--text", text]
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, env=env)
    port = int(proc.stdout.readline().strip())
    time.sleep(0.2)
    return proc, port

class TermEmu:
    """Minimal ANSI screen emulator covering termbox's output:
       \x1b[2J clear, \x1b[<r>;<c>H cursor pos, \x1b[...m SGR, plain text."""
    def __init__(self, rows, cols):
        self.rows, self.cols = rows, cols
        self.grid = [[' ']*cols for _ in range(rows)]
        self.r = self.c = 0          # 0-based cursor
    def feed(self, data: bytes):
        s = data.decode('latin1')
        i = 0
        while i < len(s):
            ch = s[i]
            if ch == '\x1b':
                if i+1 < len(s) and s[i+1] == '[':
                    j = i+2
                    while j < len(s) and not ('@' <= s[j] <= '~'):
                        j += 1
                    if j < len(s):
                        self.csi(s[i+2:j], s[j])
                        i = j+1
                        continue
                elif i+1 < len(s) and s[i+1] in '()#%$':
                    # two-char escape (e.g. \x1b(B charset select); consume any
                    # trailing final byte too
                    if (s[i+1] == '(' and i+2 < len(s) and s[i+2] in 'AB0'):
                        i += 3
                    else:
                        i += 2
                    continue
                i += 1
                continue
            if ch == '\n':
                self.r += 1; self.c = 0
            elif ch == '\r':
                self.c = 0
            elif ord(ch) >= 32:
                run = self.utf8_run(s, i)
                if len(run) > 1:
                    # decode the assembled UTF-8 bytes into one codepoint
                    try:
                        self.put(run.encode('latin1').decode('utf-8'))
                    except UnicodeDecodeError:
                        self.put(run)
                else:
                    self.put(ch)
                i += len(run)-1
            i += 1
    def utf8_run(self, s, i):
        b = ord(s[i])
        if b >= 0xC0 and i+1 < len(s):
            if b & 0xE0 == 0xC0: nxt = 1
            elif b & 0xF0 == 0xE0: nxt = 2
            elif b & 0xF8 == 0xF0: nxt = 3
            else: nxt = 0
            run = s[i:i+1+nxt]
            if len(run) == 1+nxt:
                return run
        return s[i]
    def csi(self, params, final):
        ps = params.split(';')
        if final == 'H':
            try:
                r = int(ps[0]) if ps[0] else 1
                cc = int(ps[1]) if len(ps) > 1 and ps[1] else 1
            except ValueError:
                r, cc = 1, 1
            self.r = max(0, r-1); self.c = max(0, cc-1)
        elif final == 'J':
            # termbox draws a full frame after \x1b[2J but only re-emits cells
            # that changed since its previous frame, keeping unchanged cells on
            # the real terminal. To mirror that persistent screen (so borders
            # shared across frames are not erased), do not blank the grid here;
            # a fresh ''clear'' is modelled by the per-frame full repaint of the
            # changed cells that actually follow. See the feed() 'clear' call.
            pass
    def put(self, ch):
        if 0 <= self.r < self.rows and 0 <= self.c < self.cols:
            self.grid[self.r][self.c] = ch
        self.c += 1
    def snapshot(self):
        return "\n".join("".join(r).rstrip() for r in self.grid).rstrip("\n")

def run(rows, cols, prompt, base):
    env = dict(os.environ, PUCI_BASE_URL=base, PUCI_API_KEY="test",
               PUCI_MODEL="mock-model", PUCI_TUI_FORCE="1", TERM="xterm-256color")
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    def ctty():
        os.setsid()
        try: fcntl.ioctl(0, termios.TIOCSCTTY, 0)
        except Exception: pass
    p = subprocess.Popen([PUCI], env=env, stdin=slave, stdout=slave, stderr=slave,
                         preexec_fn=ctty, text=False)
    os.close(slave)
    time.sleep(0.7)
    if prompt:
        os.write(master, b"\r"); time.sleep(0.2)
        os.write(master, prompt.encode()); time.sleep(0.2)
        os.write(master, b"\r")
    time.sleep(1.5)                       # allow the turn to stream/complemplete
    buf = b""
    end = time.time() + 0.6
    while time.time() < end:
        r,_,_ = select.select([master], [], [], 0.15)
        if r:
            try: buf += os.read(master, 65536)
            except OSError: break
    os.write(master, b"q")                # clean q-exit (also restores terminal)
    time.sleep(0.2)
    if p.poll() is None:
        os.kill(p.pid, signal.SIGTERM)
    try: p.wait(timeout=3)
    except Exception: p.terminate()
    os.close(master)
    return buf

def last_frame(buf):
    """Return the raw bytes of the final rendered frame (the last region that
       starts with \x1b[2J), trimmed of any subsequent teardown escapes."""
    marker = b'\x1b[2J'
    idx = buf.rfind(marker)
    if idx < 0:
        return buf
    frame = buf[idx:]
    # cut at the leave-alternate-screen (shutdown) sequence if present
    cut = frame.find(b'\x1b[?1049l')
    if cut >= 0:
        frame = frame[:cut]
    return frame

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=24)
    ap.add_argument("--cols", type=int, default=80)
    ap.add_argument("--prompt", default="hello world")
    ap.add_argument("--debug-log", default="/tmp/puci_snapshot_debug.log")
    ap.add_argument("--text", default="")
    args = ap.parse_args()
    fake, port = start_fake(tokens=40, text=args.text)
    try:
        base = f"http://127.0.0.1:{port}/v1"
        buf = run(args.rows, args.cols, args.prompt, base)
        frame = last_frame(buf)
        with open(args.debug_log, "wb") as f:
            f.write(buf)
        emu = TermEmu(args.rows, args.cols)
        emu.feed(frame)
        print(f"== PUCI snapshot {args.rows}x{args.cols} ({'prompt: '+args.prompt if args.prompt else 'idle'}) ==")
        print(emu.snapshot())
    finally:
        fake.terminate()

if __name__ == "__main__":
    main()
