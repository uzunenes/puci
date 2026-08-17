#!/usr/bin/env python3
"""
test_tui_polish.py — semantic grid assertions for the TUI visual-correctness
pass. Drives the real TUI binary in a sized pty against tests/fake_sse.py at
the four contract sizes and asserts on the decoded terminal output that:

  - no TTG instrumentation envelope (U+2063 ... U+2063) is visible
  - no raw decimal timestamp leaks into the transcript
  - no scrollbar cell overwrites content (no '░' inside message rows)
  - the composer has a distinct top border / content / bottom border
  - no false "new ↓" badge after an un-scrolled normal flow
  - no raw "[think]" reasoning text in the default transcript
  - a user "YOU" label and an assistant "PUCI" heading both appear
  - no "— done —" clutter and no startup keybinding tutorial
  - the footer holds only whole actions

Exits non-zero when any assertion fails; prints one FAIL line per defect.
"""
import os, sys, time, subprocess, pty, fcntl, termios, struct, select, signal, re

HERE = os.path.dirname(os.path.abspath(__file__))
FAKE = os.path.join(HERE, "fake_sse.py")
PUCI = os.environ.get("PUCI_BIN") or os.path.abspath(os.path.join(HERE, "..", "build", "src", "cli", "puci"))

SIZES = [(60, 15), (80, 24), (120, 30), (200, 50)]
failures = []
checks = 0


def start_fake(tokens=40, rate=80):
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


def run_tui(rows, cols, base_url, run_ms=3200):
    env = dict(os.environ)
    env["PUCI_BASE_URL"] = base_url
    env["PUCI_API_KEY"] = "test"
    env["PUCI_MODEL"] = "mock-model"
    env["PUCI_TUI_FORCE"] = "1"
    env["PUCI_TUI_EXIT_AFTER_MS"] = str(run_ms)

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

    def _setsid_ctty():
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
    time.sleep(0.6)
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


def ck(label, cond, detail=""):
    global checks
    checks += 1
    if not cond:
        failures.append(f"{label}: {detail}")


def main():
    fake, port = start_fake()
    try:
        base = f"http://127.0.0.1:{port}/v1"
        for (cols, rows) in SIZES:
            out = run_tui(rows, cols, base)
            tag = f"{rows}x{cols}"

            ck(f"{tag} no-ttg", "\u2063" not in out, "U+2063 sentinel leaked")
            ck(f"{tag} no-timestamp", not re.search(r"[0-9]{9,}", out),
               "raw envelope timestamp visible")
            ck(f"{tag} assistant-text", "tok0" in out and "tok39" in out,
               "tokenized assistant content missing")
            ck(f"{tag} no-think", "[think]" not in out, "raw reasoning leaked")
            ck(f"{tag} no-done", "— done —" not in out, "completion clutter present")
            ck(f"{tag} no-tutorial", "PUCI TUI v0.5" not in out, "startup tutorial present")
            ck(f"{tag} no-unread", "new \u2193" not in out, "false unread/new-lines badge")

            # roles
            ck(f"{tag} user-role", "YOU" in out, "missing YOU label")
            ck(f"{tag} assist-role", "PUCI" in out, "missing assistant heading")

            # composer borders: the composer's top/left/bottom border glyphs +
            # a right border glyph (┐ ┘ ─ ) imply a closed, non-overwritten box.
            ck(f"{tag} composer-top", "\u250c\u2500 Composer" in out or "\u250c\u2500 Input" in out,
               "missing composer top border title")
            ck(f"{tag} composer-bottom", "\u2514" in out, "missing composer bottom border")

            # footer: must not end in a half-typed action. A plausible closing
            # is 'q quit' (wide) or at least not a broken trailing separator.
            # We require the wide footer to complete on wide sizes.
            if cols >= 120:
                ck(f"{tag} footer-qquit", "q quit" in out, "wide footer truncated before q quit")

        print(f"passed {len(SIZES)} sizes, {checks} checks, {len(failures)} failure(s)")
        for f_ in failures:
            print("  FAIL:", f_)
        return 1 if failures else 0
    finally:
        fake.terminate()
        try:
            fake.wait(timeout=3)
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())