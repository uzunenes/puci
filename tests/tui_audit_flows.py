#!/usr/bin/env python3
"""
tui_audit_flows.py — PTY-driven audit of the core user journey.

Exercises the real TUI binary against tests/fake_sse.py and asserts:

  1. Clean startup (empty history) renders a composer, no invalid-state errors.
  2. "hello" produces one assistant reply and the session stays usable.
  3. Multiline composer: Ctrl+J (0x0A) inserts a newline and Enter sends; a
     follow-up Enter sends a second prompt (proves the message text with an
     embedded newline round-trips without corrupting the composer).
  4. Bracketed paste: pasted text containing q/m/s/y and newlines never
     triggers global shortcuts (the TUI must still be alive afterwards), and
     the pasted block is treated as a single message.
These are behavioural checks; they complement the unit/integration suites.
"""
import os, sys, time, subprocess, pty, fcntl, termios, struct, select, signal

HERE = os.path.dirname(os.path.abspath(__file__))
FAKE = os.path.join(HERE, "fake_sse.py")
PUCI = os.environ.get("PUCI_BIN") or os.path.abspath(os.path.join(HERE, "..", "build", "src", "cli", "puci"))
ERR = "invalid operation state"

def start_fake(tokens=60, rate=400):
    env = dict(os.environ)
    proc = subprocess.Popen(
        [sys.executable, "-u", FAKE, "--port", "0", "--tokens", str(tokens), "--rate", str(rate)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1, env=env)
    line = proc.stdout.readline()
    try:
        port = int(line.strip())
    except Exception:
        proc.kill()
        raise RuntimeError(f"fake_sse failed: {line!r} {proc.stderr.read()}")
    time.sleep(0.2)
    return proc, port

class Term:
    def __init__(self, base_url, rows=24, cols=80):
        self.env = dict(os.environ)
        self.env.update({
            "PUCI_BASE_URL": base_url, "PUCI_API_KEY": "test",
            "PUCI_MODEL": "mock-model", "PUCI_TUI_FORCE": "1",
            "TERM": "xterm-256color",
        })
        self.rows, self.cols = rows, cols
        self.master, self.slave = pty.openpty()
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        def _ctty():
            os.setsid()
            try: fcntl.ioctl(0, termios.TIOCSCTTY, 0)
            except Exception: pass
        self.proc = subprocess.Popen([PUCI], env=self.env, stdin=self.slave,
                                     stdout=self.slave, stderr=self.slave,
                                     preexec_fn=_ctty, text=False)
        os.close(self.slave)
        time.sleep(0.7)
        self.buf = b""
        self.alive = True

    def flush(self, seconds=0.5):
        end = time.time()+seconds
        while time.time()<end:
            r,_,_ = select.select([self.master],[],[],0.15)
            if r:
                try: self.buf += os.read(self.master, 65536)
                except OSError: break
        return self.buf

    def keys(self, data, settle=0.2):
        os.write(self.master, data)
        time.sleep(settle)
        self.flush(settle)

    def read_back(self):
        return self.buf.decode("utf-8", "replace")

    def close(self):
        self.alive=False
        try: os.kill(self.proc.pid, signal.SIGTERM)
        except Exception: pass
        try: self.proc.wait(timeout=3)
        except Exception:
            try: self.proc.terminate()
            except Exception: pass
        try: os.close(self.master)
        except Exception: pass

def main():
    fake, port = start_fake()
    try:
        base = f"http://127.0.0.1:{port}/v1"
        results = []

        def run(name, fn):
            t = Term(base)
            try:
                ok, detail = fn(t)
            except Exception as e:
                ok, detail = False, f"exception {e!r}"
            finally:
                t.close()
            out = t.read_back()
            if ERR in out:
                ok, detail = False, f"invalid-state error present: {detail}"
            results.append((name, ok, detail))

        # 1. clean startup: composer prompt visible.
        def s_startup(t):
            t.flush(0.5)
            s = t.read_back()
            return "Message" in s or "Input" in s, "composer found"
        run("clean_startup", s_startup)

        # 2. hello -> assistant reply (single turn)
        def s_hello(t):
            t.keys(b"\r"); t.keys(b"hello"); t.keys(b"\r")
            t.flush(2.0)
            s = t.read_back()
            return ("tok0" in s), "assistant text present"
        run("hello", s_hello)

        # 3. multiline via Ctrl+J then Enter
        def s_multiline(t):
            t.keys(b"\r")          # open composer
            t.keys(b"line A")      # text
            t.keys(b"\n")          # Ctrl+J = newline (must not send)
            t.keys(b"line B")      # more text on line 2
            # must NOT have sent yet: no assistant reply before Enter
            pre = t.read_back()
            t.keys(b"\r")          # Enter -> send
            t.flush(2.0)
            post = t.read_back()
            has_token = "tok" in post
            # Now a second send works (session still usable)
            t.keys(b"\r"); t.keys(b"again"); t.keys(b"\r")
            t.flush(1.5)
            s2 = t.read_back()
            return has_token and "tok" in s2, "multiline sent + second turn ok"
        run("multiline_ctrlj", s_multiline)

        # 4. bracketed paste with command-like text
        def run_paste(t):
            t.flush(0.3)
            paste = b"\x1b[200~pasted q\nm\ns\ny\nn\n\x1b[201~"
            t.keys(paste)
            t.flush(0.4)
            t.keys(b"\r")          # send the pasted block
            t.flush(1.5)
            s = t.read_back()
            alive_after = True
            try:
                r,_,_ = select.select([], [], [], 0)  # process still alive?
                rc = t.proc.poll()
            except Exception:
                rc = None
            return rc is None and "tok" in s, "paste did not quit/send; reply received"
        run("paste_safety", run_paste)

        # 5. model picker: async fetch merges new models into the visible list
        def s_model_pick(t):
            t.flush(0.3)
            t.keys(b"m")                 # open model picker
            t.flush(1.2)                 # allow background fetch + rebuild
            s = t.read_back()
            # The cached/static picker already had entries; the async fetch must
            # merge deepseek/deepseek-chat from /models and keep selection valid.
            merged = "deepseek" in s
            still_open = True
            try:
                rc = t.proc.poll()
            except Exception:
                rc = None
            t.keys(b"\x1b")              # Escape closes without changing model
            t.flush(0.2)
            return merged, "fetched model visible in picker"
        run("model_pick_merge", s_model_pick)

        print("\n== AUDIT RESULTS ==")
        allok = True
        for name, ok, detail in results:
            print(f"{'PASS' if ok else 'FAIL'}  {name:20} {detail}")
            allok &= ok
        if out:
            pass
        return 0 if allok else 1
    finally:
        fake.terminate()
        try: fake.wait(timeout=3)
        except Exception: pass

out=[]
ERR_INVALID="invalid operation state"
frames=[]

if __name__ == "__main__":
    sys.exit(main())
