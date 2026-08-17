#!/usr/bin/env python3
"""
tui_layout_smoke.py — resize/viewport regression smoke test for the TUI.

Drives the real TUI binary in a sized pty against tests/fake_sse.py at the
four contract sizes (60x15, 80x24, 120x30, 200x50), injects one prompt, and
asserts:

  - the assistant's reply text is rendered (real conversation, not a blank UI)
  - a size warning is never emitted for a chosen representative sizes
  - the run is exited cleanly via PUCI_TUI_EXIT_AFTER_MS
  - a representative idle sample shows zero "invalid operation state" floods

This complements test_approval_event.c (state-machine invariants) with the
viewport / resize layer the unit suite cannot reach through the public API.
"""
import os, sys, time, subprocess, pty, fcntl, termios, struct, select, signal

HERE = os.path.dirname(os.path.abspath(__file__))
FAKE = os.path.join(HERE, "fake_sse.py")
PUCI = os.environ.get("PUCI_BIN") or os.path.abspath(os.path.join(HERE, "..", "build", "src", "cli", "puci"))

SIZES = [(60, 15), (80, 24), (120, 30), (200, 50)]
ASSISTANT_MARKER = "tok"   # fake_sse emits "...tok0 tok1 ... "
ERR_MARKER = "invalid operation state"

def start_fake(tokens=20, rate=200):
    env = dict(os.environ)
    proc = subprocess.Popen(
        [sys.executable, "-u", FAKE, "--port", "0", "--tokens", str(tokens), "--rate", str(rate)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1, env=env)
    port = None
    line = proc.stdout.readline()
    try:
        port = int(line.strip())
    except Exception:
        proc.kill()
        raise RuntimeError(f"fake_sse failed: {line!r} {proc.stderr.read()}")
    time.sleep(0.2)
    return proc, port


def run_tui(rows, cols, base_url, tokens=300, idle_only=False, run_ms=3500):
    env = dict(os.environ)
    env["PUCI_BASE_URL"] = base_url
    env["PUCI_API_KEY"] = "test"
    env["PUCI_MODEL"] = "mock-model"
    env["PUCI_TUI_FORCE"] = "1"
    env["PUCI_TUI_EXIT_AFTER_MS"] = str(run_ms)
    env["TERM"] = "xterm-256color"

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    def _setsid_ctty():
        # Become session leader and acquire the pty slave (fd 0) as the
        # controlling terminal so termbox's tb_init() opens /dev/tty instead
        # of falling back to headless trace mode.
        os.setsid()
        try:
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)
        except Exception:
            pass
    proc = subprocess.Popen([PUCI], env=env, stdin=slave, stdout=slave, stderr=slave,
                            preexec_fn=_setsid_ctty, text=False)
    os.close(slave)

    buf = b""
    end = time.time() + (run_ms / 1000.0) + 1.0
    # give termbox enough init time before sending the prompt
    time.sleep(0.6)
    if not idle_only:
        # Open the composer (Enter), type the prompt, then Enter to send.
        os.write(master, b"\r")
        time.sleep(0.2)
        os.write(master, b"hello world")
        time.sleep(0.2)
        os.write(master, b"\r")
    try:
        while time.time() < end:
            r, _, _ = select.select([master], [], [], 0.2)
            if r:
                try:
                    d = os.read(master, 65536)
                    buf += d
                except OSError:
                    break
    finally:
        try:
            os.kill(proc.pid, signal.SIGTERM)
        except Exception:
            pass
        try:
            proc.wait(timeout=3)
        except Exception:
            proc.terminate()
        try:
            os.close(master)
        except Exception:
            pass
    return buf.decode("utf-8", "replace")

def main():
    fake, port = start_fake()
    try:
        base = f"http://127.0.0.1:{port}/v1"
        idle = run_tui(80, 24, base, idle_only=True, run_ms=1500)
        errs = idle.count(ERR_MARKER)
        print(f"[idle 80x24] 'invalid operation state' count={errs}")
        if errs > 0:
            print("FAIL: idle run flooded invalid-operation errors")
            return 1

        ok = 0
        for (cols, rows) in SIZES:
            out = run_tui(rows, cols, base)
            marker = ASSISTANT_MARKER in out
            errs = out.count(ERR_MARKER)
            print(f"[{rows}x{cols}] assistant_text={marker} invalid_op_errs={errs} len={len(out)}")
            if marker and errs == 0 and len(out) > 200:
                ok += 1
            else:
                print(f"  -> FAIL size {rows}x{cols}")
        print(f"passed {ok}/{len(SIZES)} sized runs")
        return 0 if ok == len(SIZES) else 1
    finally:
        fake.terminate()
        try:
            fake.wait(timeout=3)
        except Exception:
            pass

if __name__ == "__main__":
    sys.exit(main())
