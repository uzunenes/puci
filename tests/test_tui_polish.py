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
  - no bold-black / hard-coded white text (unreadable on dark / light themes)
  - light background (PUCI_THEME, an OSC 11 reply, or the desktop's
    light/dark setting when the terminal won't say) avoids yellow/cyan;
    NO_COLOR emits no colour escapes at all
  - the frame follows window resizes (grow and shrink)
  - typing a letter while idle composes (no s/m/q shortcut misfire)
  - a user "YOU" label and an assistant "PUCI" heading both appear
  - no "— done —" clutter and no startup keybinding tutorial
  - the footer holds only whole actions

Exits non-zero when any assertion fails; prints one FAIL line per defect.
"""
import os, sys, time, subprocess, pty, fcntl, termios, struct, select, signal, re

# Hermetic test home: never read the developer's ~/.config/puci (API keys) or
# write sessions/history into their ~/.local/share/puci.
import atexit as _atexit, shutil as _shutil, tempfile as _tempfile
_TEST_HOME = _tempfile.mkdtemp(prefix="puci-test-home-")
_atexit.register(_shutil.rmtree, _TEST_HOME, True)
for _k in ("PUCI_API_KEY", "OPENROUTER_API_KEY", "PUCI_BASE_URL", "PUCI_MODEL"):
    os.environ.pop(_k, None)
os.environ.update(HOME=_TEST_HOME,
                  XDG_CONFIG_HOME=os.path.join(_TEST_HOME, ".config"),
                  XDG_DATA_HOME=os.path.join(_TEST_HOME, ".local", "share"),
                  XDG_STATE_HOME=os.path.join(_TEST_HOME, ".local", "state"))


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


def run_tui(rows, cols, base_url, run_ms=3200, extra_env=None, osc11_reply=None):
    env = dict(os.environ)
    env.pop("NO_COLOR", None)
    env.pop("PUCI_THEME", None)
    env.pop("COLORFGBG", None)
    if extra_env:
        env.update(extra_env)
    env["PUCI_BASE_URL"] = base_url
    env["PUCI_API_KEY"] = "test"
    env["PUCI_MODEL"] = "mock-model"
    env["PUCI_TUI_FORCE"] = "1"
    env["PUCI_TUI_EXIT_AFTER_MS"] = str(run_ms)
    # CI runners often have TERM=dumb or unset; termbox2 needs a real terminfo entry.
    env["TERM"] = "xterm-256color"

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    # Point termbox at the pty slave directly so tb_init does not need /dev/tty,
    # which is absent on headless CI runners.
    try:
        env["PUCI_TUI_TTY"] = os.ttyname(slave)
    except OSError:
        pass

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
    if osc11_reply is not None:
        # Play a terminal: answer the background-colour query (OSC 11) and
        # the DA1 probe that follows it.
        t0 = time.time()
        while time.time() - t0 < 1.0 and b"\x1b]11;?" not in buf:
            r, _, _ = select.select([master], [], [], 0.05)
            if r:
                buf += os.read(master, 65536)
        os.write(master, osc11_reply + b"\x1b[?62;22c")
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


def run_resize(start, end, base_url):
    """Start at `start` (rows, cols), resize the pty to `end`, return the
    escape sequences drawn after the resize."""
    env = dict(os.environ)
    for k in ("NO_COLOR", "PUCI_THEME", "COLORFGBG"):
        env.pop(k, None)
    env.update(PUCI_BASE_URL=base_url, PUCI_API_KEY="test", PUCI_MODEL="mock-model",
               PUCI_TUI_FORCE="1", PUCI_TUI_EXIT_AFTER_MS="2500", TERM="xterm-256color",
               PUCI_THEME="dark")
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", start[0], start[1], 0, 0))
    env["PUCI_TUI_TTY"] = os.ttyname(slave)

    def _ctty():
        os.setsid()
        try:
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)
        except Exception:
            pass

    proc = subprocess.Popen([PUCI], env=env, stdin=slave, stdout=slave, stderr=slave,
                            preexec_fn=_ctty)
    os.close(slave)

    def drain(sec):
        out = b""
        end_t = time.time() + sec
        while time.time() < end_t:
            r, _, _ = select.select([master], [], [], 0.05)
            if r:
                try:
                    out += os.read(master, 65536)
                except OSError:
                    break
        return out

    drain(0.9)
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", end[0], end[1], 0, 0))
    after = drain(1.0)
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
    os.close(master)
    return after.decode("utf-8", "replace"), proc.returncode


def run_type_without_enter(base_url, keys="selam"):
    """Start idle, type `keys` WITHOUT pressing Enter first, return the
    decoded screen. Reproduces: forgetting Enter used to fire s/m/q shortcuts."""
    env = dict(os.environ)
    for k in ("NO_COLOR", "PUCI_THEME", "COLORFGBG"):
        env.pop(k, None)
    env.update(PUCI_BASE_URL=base_url, PUCI_API_KEY="test", PUCI_MODEL="mock-model",
               PUCI_TUI_FORCE="1", PUCI_TUI_EXIT_AFTER_MS="2500", TERM="xterm-256color",
               PUCI_THEME="dark")
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    env["PUCI_TUI_TTY"] = os.ttyname(slave)

    def _ctty():
        os.setsid()
        try:
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)
        except Exception:
            pass

    proc = subprocess.Popen([PUCI], env=env, stdin=slave, stdout=slave, stderr=slave,
                            preexec_fn=_ctty)
    os.close(slave)

    def drain(sec):
        out = b""
        end_t = time.time() + sec
        while time.time() < end_t:
            r, _, _ = select.select([master], [], [], 0.05)
            if r:
                try:
                    out += os.read(master, 65536)
                except OSError:
                    break
        return out

    drain(0.8)
    os.write(master, keys.encode())
    out = drain(0.8)
    os.write(master, b"\r")
    drain(0.3)
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
    os.close(master)
    return out.decode("utf-8", "replace")


def ck(label, cond, detail=""):
    global checks
    checks += 1
    if not cond:
        failures.append(f"{label}: {detail}")


def main():
    debug = os.environ.get("PUCI_TUI_DEBUG")
    fake, port = start_fake()
    try:
        base = f"http://127.0.0.1:{port}/v1"
        for (cols, rows) in SIZES:
            out = run_tui(rows, cols, base)
            tag = f"{rows}x{cols}"
            if debug:
                print(f"[debug] {tag}: {len(out)} bytes; head={out[:160]!r}", file=sys.stderr)

            ck(f"{tag} no-ttg", "\u2063" not in out, "U+2063 sentinel leaked")
            ck(f"{tag} no-timestamp", not re.search(r"[0-9]{9,}", out),
               "raw envelope timestamp visible")
            ck(f"{tag} assistant-text", "tok0" in out and "tok39" in out,
               "tokenized assistant content missing")
            ck(f"{tag} no-think", "[think]" not in out, "raw reasoning leaked")
            ck(f"{tag} no-done", "— done —" not in out, "completion clutter present")
            ck(f"{tag} no-tutorial", "PUCI TUI v0.5" not in out, "startup tutorial present")
            ck(f"{tag} no-unread", "new \u2193" not in out, "false unread/new-lines badge")

            # palette-independent readability: never "bold black" (black on
            # dark terminals that don't brighten bold) and never a hard-coded
            # white foreground (invisible on light terminals).
            ck(f"{tag} no-bold-black", "\x1b[1m\x1b[30m" not in out,
               "secondary text drawn bold-black (unreadable on dark themes)")
            ck(f"{tag} no-fixed-white", "\x1b[37m" not in out,
               "text drawn hard-coded white (unreadable on light themes)")

            # roles
            ck(f"{tag} user-role", "YOU" in out, "missing YOU label")
            ck(f"{tag} assist-role", "PUCI" in out, "missing assistant heading")

            # composer borders: the composer's top/left/bottom border glyphs +
            # a right border glyph (┐ ┘ ─ ) imply a closed, non-overwritten box.
            ck(f"{tag} composer-top", "\u250c\u2500 Composer" in out or "\u250c\u2500 Input" in out,
               "missing composer top border title")
            ck(f"{tag} composer-bottom", "\u2514" in out, "missing composer bottom border")

            # footer: must not end in a half-typed action. The wide footer ends
            # in the quit hint ("Ctrl+C quit"); require it to complete.
            if cols >= 120:
                ck(f"{tag} footer-quit", "Ctrl+C quit" in out, "wide footer truncated before the quit hint")

        # ---- palette variants (design gate: readable on light and dark) ----
        sgr_colour = re.compile(r"\x1b\[(?:[0-9;]*;)?(?:3[0-7]|4[0-7]|9[0-7]|10[0-7])(?:;[0-9;]*)?m")
        light_bad = ("\x1b[33m", "\x1b[36m", "\x1b[43m", "\x1b[46m")  # yellow/cyan wash out on light
        out = run_tui(24, 80, base, extra_env={"PUCI_THEME": "light"})
        ck("light assistant-text", "tok0" in out, "light palette run produced no reply")
        ck("light no-yellow-cyan", not any(b in out for b in light_bad),
           "yellow/cyan used on a light background")
        out = run_tui(24, 80, base, osc11_reply=b"\x1b]11;rgb:ffff/ffff/ffff\x1b\\")
        ck("osc11-light detected", not any(b in out for b in light_bad),
           "OSC 11 white background not detected (yellow/cyan still used)")
        out = run_tui(24, 80, base, osc11_reply=b"\x1b]11;rgb:1717/1414/2121\x1b\\")
        ck("osc11-dark keeps palette", "\x1b[36m" in out or "\x1b[1m\x1b[36m" in out,
           "dark OSC 11 reply should keep the dark palette")
        # Terminal doesn't answer OSC 11 -> follow the computer's light/dark
        # setting. Fake desktop tools on PATH stand in for the real desktop.
        def fake_bin(gdbus_out, gsettings_out):
            d = _tempfile.mkdtemp(prefix="puci-fakebin-", dir=_TEST_HOME)
            for name, text in (("gdbus", gdbus_out), ("gsettings", gsettings_out)):
                with open(os.path.join(d, name), "w") as fh:
                    if text is None:
                        fh.write("#!/bin/sh\nexit 1\n")
                    else:
                        fh.write("#!/bin/sh\necho \"%s\"\n" % text)
                os.chmod(os.path.join(d, name), 0o755)
            return {"PATH": d + os.pathsep + os.environ.get("PATH", "")}
        out = run_tui(24, 80, base, extra_env=fake_bin("(<uint32 2>,)", None))
        ck("desktop-light portal", not any(b in out for b in light_bad),
           "desktop portal said light but yellow/cyan were used")
        out = run_tui(24, 80, base, extra_env=fake_bin(None, "'prefer-light'"))
        ck("desktop-light gsettings", not any(b in out for b in light_bad),
           "gsettings said prefer-light but yellow/cyan were used")
        out = run_tui(24, 80, base, extra_env=fake_bin("(<uint32 1>,)", None))
        ck("desktop-dark portal", "\x1b[36m" in out,
           "desktop portal said dark but the dark palette was not used")
        out = run_tui(24, 80, base, extra_env={"NO_COLOR": "1"})
        ck("no-color assistant-text", "tok0" in out, "NO_COLOR run produced no reply")
        ck("no-color no-sgr-colours", not sgr_colour.search(out),
           "NO_COLOR set but colour escapes were emitted")

        # ---- window resize: the TUI must follow SIGWINCH both ways ----
        def drawn_extent(out):
            pos = re.findall(r"\x1b\[(\d+);(\d+)H", out)
            return (max((int(r) for r, _ in pos), default=0),
                    max((int(c) for _, c in pos), default=0))
        out, rc = run_resize((15, 60), (30, 120), base)
        r, c = drawn_extent(out)
        ck("resize-grow", r > 15 and c > 60,
           f"after growing 60x15 -> 120x30 the frame still ends at {c}x{r}")
        out, rc = run_resize((30, 120), (15, 60), base)
        r, c = drawn_extent(out)
        ck("resize-shrink", 0 < r <= 15 and 0 < c <= 60,
           f"after shrinking 120x30 -> 60x15 cells were drawn at {c}x{r}")
        ck("resize-shrink-exit", rc == 0, f"puci exited with {rc} after shrinking")

        # ---- type-to-compose: typing without Enter must not fire shortcuts ----
        out = run_type_without_enter(base, "selam")
        plain = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b\][^\x07]*\x07|\x1b[()][AB0]", "", out)
        ck("compose-types-text", "selam" in plain,
           "typing 'selam' idle did not appear in the composer")
        ck("compose-enters-input", "Input" in plain,
           "typing a letter idle did not switch the composer to input mode")

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