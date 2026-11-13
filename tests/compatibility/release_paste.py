"""Exercise the actual Release desktop with an isolated profile and independent receivers."""
import argparse
from contextlib import contextmanager, closing
import ctypes
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import time
from urllib.request import urlopen

import websocket
import pywintypes
import win32api
import win32clipboard
import win32con
import win32gui as gui
import win32process
import winerror


SAMPLE = "样例"
EXPECTED = ["左样例右", "other", 1, 0, 1, 0]
ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "build/app"


def wait(read, why, seconds=8):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        value = read()
        if value:
            return value
        time.sleep(.01)
    raise RuntimeError(why)


@contextmanager
def clipboard(owner=0):
    def opened():
        try:
            win32clipboard.OpenClipboard(owner)
        except pywintypes.error as error:
            if error.winerror != winerror.ERROR_ACCESS_DENIED:
                raise
            return False
        return True
    wait(opened, "Fixture clipboard was held by another reader", 1)
    try:
        yield
    finally:
        win32clipboard.CloseClipboard()


def owned_window(pid, cls, title=None):
    found = []
    def inspect(hwnd, _):
        if (win32process.GetWindowThreadProcessId(hwnd)[1] == pid
                and (not cls or gui.GetClassName(hwnd) == cls)
                and (title is None or title in gui.GetWindowText(hwnd) and gui.IsWindowVisible(hwnd))):
            found.append(hwnd)
    gui.EnumWindows(inspect, None)
    return next(iter(found), None)


def activate(hwnd):
    if gui.GetForegroundWindow() != hwnd:
        win32api.keybd_event(win32con.VK_MENU, 0, 0, 0)
        win32api.keybd_event(win32con.VK_MENU, 0, win32con.KEYEVENTF_KEYUP, 0)
        gui.SetForegroundWindow(hwnd)
    wait(lambda: gui.GetForegroundWindow() == hwnd, "Fixture receiver did not acquire focus")


@contextmanager
def app(args, cls, title=None):
    with subprocess.Popen(args) as process:
        hwnd = None
        try:
            def started():
                if process.poll() is not None:
                    raise RuntimeError(f"{Path(args[0]).name} exited with {process.returncode}")
                return owned_window(process.pid, cls, title)
            hwnd = wait(started, f"Missing fixture window {cls}", 15)
            yield process.pid, hwnd
        finally:
            if hwnd and gui.IsWindow(hwnd):
                gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
            try:
                process.wait(5)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(5)


def seed(profile):
    owner = gui.CreateWindow("STATIC", "Synthetic producer", 0, 0, 0, 0, 0, -3, 0, 0, None)
    try:
        for text in (SAMPLE, "fixture head"):
            with clipboard(owner):
                win32clipboard.EmptyClipboard()
                win32clipboard.SetClipboardText(text, win32con.CF_UNICODETEXT)
            with subprocess.Popen([BIN / "cliprope_headless.exe", profile, "capture"],
                                  creationflags=subprocess.CREATE_NO_WINDOW) as process:
                def finished():
                    gui.PumpWaitingMessages()
                    return process.poll() is not None
                try:
                    wait(finished, "Fixture capture timed out", 15)
                finally:
                    if process.poll() is None:
                        process.kill()
                if process.returncode != 0:
                    raise RuntimeError("Fixture capture failed")
    finally:
        gui.DestroyWindow(owner)
    rows = order(profile)
    if len(rows) != 2:
        raise RuntimeError("Expected two captured fixture cards")
    return rows[1]


def order(profile):
    with closing(sqlite3.connect(profile / "history.db")) as db:
        return [r[0] for r in db.execute("SELECT id FROM entries WHERE rope_id='main' ORDER BY sort_key,id")]


def paste_card(pid, workspace, receiver, index, between=None):
    # Locate the two paper bodies by Windows hit testing in a fresh default-layout profile.
    dpi = ctypes.windll.user32.GetDpiForWindow(workspace)
    x, y = gui.ClientToScreen(workspace, (0, round(170 * dpi / 96)))
    spans = []
    for dx in range(0, gui.GetClientRect(workspace)[2], 4):
        if gui.WindowFromPoint((x + dx, y)) == workspace:
            if not spans or spans[-1][-1] != dx - 4:
                spans.append([])
            spans[-1].append(dx)
    bodies = [span for span in spans if span[-1] - span[0] > 50 * dpi / 96]
    if len(bodies) != 2:
        raise RuntimeError(f"Expected two unobscured fixture cards, found {len(bodies)}")
    body = bodies[index]
    point = (x + body[len(body) // 2], y)
    win32api.SetCursorPos(point)
    if gui.GetForegroundWindow() != receiver or gui.WindowFromPoint(point) != workspace:
        raise RuntimeError("Workspace or receiver lost fixture ownership")
    win32api.mouse_event(win32con.MOUSEEVENTF_LEFTDOWN, 0, 0)
    try:
        def captured():
            hit = gui.WindowFromPoint(point)
            return (hit and win32process.GetWindowThreadProcessId(hit)[1] == pid
                    and gui.GetClassName(hit) == "ClipRope.DragInput")
        wait(captured, "First card press did not establish capture", .4)
    finally:
        win32api.mouse_event(win32con.MOUSEEVENTF_LEFTUP, 0, 0)
    wait(lambda: gui.WindowFromPoint(point) == workspace, "Card did not regain mouse input", .25)
    time.sleep(min(.085, ctypes.windll.user32.GetDoubleClickTime() / 5000))
    if between:
        between()
    if gui.GetForegroundWindow() != receiver:
        raise RuntimeError("Receiver lost focus between clicks")
    win32api.mouse_event(win32con.MOUSEEVENTF_LEFTDOWN, 0, 0)
    win32api.mouse_event(win32con.MOUSEEVENTF_LEFTUP, 0, 0)


def run_receiver(folder, name, hwnd, prepare, observe, probe, negative_cases=(), change=None):
    profile = folder / name
    selected = seed(profile)
    activate(hwnd)
    prepare("first")
    args = [BIN / "ClipRope.exe", "--profile", profile, "--show", "--export-diagnostics-on-exit"]
    log = profile / "qualification.log"
    if probe:
        args += ["--probe", log]
    with app(args, "ClipRope.Listener") as (pid, _):
        workspace = wait(lambda: owned_window(pid, "ClipRope.Workspace"), "Workspace did not open")
        for opening in ("first", "reopen", *negative_cases):
            if opening != "first":
                activate(hwnd)
                prepare(opening)
                for key in (17, 16, 86):
                    win32api.keybd_event(key, 0, 0, 0)
                for key in (86, 16, 17):
                    win32api.keybd_event(key, 0, win32con.KEYEVENTF_KEYUP, 0)
            wait(lambda: gui.IsWindowVisible(workspace), "Workspace did not become visible")
            gui.SetWindowPos(workspace, win32con.HWND_TOPMOST, 0, 0, 0, 0, 0x13)
            previous = order(profile)
            between = (lambda: change(opening)) if opening in ("change", "interrupt") else None
            paste_card(pid, workspace, hwnd, previous.index(selected), between)
            wait(lambda: not gui.IsWindowVisible(workspace), "Gesture did not collapse workspace")
            promoted = order(profile) == [selected] + [entry for entry in previous if entry != selected]
            expected = ["左X右", "other", 0, 0, 0, 0] if opening in negative_cases else EXPECTED
            try:
                if opening in negative_cases:
                    # Cover the complete delivery deadline, including late provider replies.
                    time.sleep(1.6)
                wait(lambda: observe() == expected, "Receiver did not match paste expectation", 2)
                time.sleep(.12)
            finally:
                observed = observe()
                # Inspect publication after delivery so the observer cannot lock out the receiver.
                with clipboard():
                    owner = ctypes.windll.user32.GetClipboardOwner()
                    published = bool(owner and win32process.GetWindowThreadProcessId(owner)[1] == pid
                                     and win32clipboard.GetClipboardData(win32con.CF_UNICODETEXT) == SAMPLE)
                if probe:
                    print("\n".join(line for line in log.read_text().splitlines()
                                    if line.startswith(("invoke ", "focus-target ", "focus-restore ", "deferred ", "command failed"))), flush=True)
                print(f"{name}.{opening} collapsed=True promoted={promoted} published={published} "
                      f"foreground={gui.GetForegroundWindow() == hwnd} "
                      f"receiver={ascii(observed)}", flush=True)
            if not promoted or not published or observed != expected:
                raise RuntimeError(f"{name}.{opening} failed publication, ordering or receiver acceptance")
            yield f"{name}.{opening}"


def custom(folder, probe):
    framework = Path(os.environ["WINDIR"]) / "Microsoft.NET/Framework64/v4.0.30319"
    binary, report = folder / "receiver.exe", folder / "fields.txt"
    refs = ["PresentationCore", "PresentationFramework", "WindowsBase", "UIAutomationProvider", "UIAutomationTypes"]
    subprocess.run([framework / "csc.exe", "/nologo", "/target:winexe", "/reference:System.Xaml.dll",
                    "/out:" + str(binary), *["/reference:" + str(framework / "WPF" / (r + ".dll")) for r in refs],
                    Path(__file__).with_name("paste_receiver.cs")], check=True, timeout=30)
    with app([binary, report], None, "ClipRope paste receiver") as (_, hwnd):
        def prepare(_):
            if not gui.SendMessage(hwnd, 0x8001, 0, 0):
                raise RuntimeError("Custom field failed to acquire focus")
        def observe():
            gui.SendMessage(hwnd, 0x8002, 0, 0)
            a, b, *counts = report.read_text(encoding="utf-8-sig").splitlines()
            return [a, b, *map(int, counts)]
        yield from run_receiver(folder, "custom", hwnd, prepare, observe, probe)


def edge(folder, probe):
    browser = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft/Edge/Application/msedge.exe"
    page = folder / "fields.html"
    page.write_text("""<meta charset=utf-8><title>ClipRope paste receiver</title><input id=a><input id=b>
    <script>const fields=[a,b]; let keys, pastes;
    function reset(mode){keys=[0,0];pastes=[0,0];a.type=mode==='password'?'password':'text';
        a.readOnly=mode==='readonly';a.value='左X右';b.value='other';a.focus();a.setSelectionRange(1,2)}
    fields.forEach((f,i)=>{f.onkeydown=e=>{if(e.ctrlKey && e.key.toLowerCase()==='v')keys[i]++};
      f.onpaste=()=>pastes[i]++});</script>""", encoding="utf-8")
    data = folder / "edge-profile"
    with app([browser, f"--user-data-dir={data}", "--remote-debugging-port=0", "--no-first-run",
              "--disable-sync", "--disable-features=msEdgeFirstRunExperience", page.as_uri()],
             "Chrome_WidgetWin_1", "ClipRope paste receiver") as (_, hwnd):
        ports = data / "DevToolsActivePort"
        wait(ports.is_file, "Isolated Edge debugging port missing")
        with urlopen("http://127.0.0.1:" + ports.read_text().splitlines()[0] + "/json/list", timeout=5) as response:
            target = next(p for p in json.load(response) if p["url"] == page.as_uri())
        with closing(websocket.create_connection(target["webSocketDebuggerUrl"], suppress_origin=True, timeout=5)) as ws:
            serial = 0
            def call(method, **params):
                nonlocal serial
                serial += 1
                ws.send(json.dumps(dict(id=serial, method=method, params=params)))
                while True:
                    reply = json.loads(ws.recv())
                    if reply.get("id") == serial:
                        return reply["result"]
            def js(expr):
                return call("Runtime.evaluate", expression=expr, returnByValue=True)["result"].get("value")
            def prepare(opening):
                call("Page.bringToFront")
                for kind in ("mousePressed", "mouseReleased"):
                    call("Input.dispatchMouseEvent", type=kind, x=20, y=15, button="left", clickCount=1)
                if not js(f"reset({json.dumps(opening)}); document.hasFocus() && document.activeElement===a"):
                    raise RuntimeError("Edge field did not acquire focus")
            def change(mode):
                win32api.keybd_event(win32con.VK_SHIFT, 0, 0, 0)
                win32api.keybd_event(win32con.VK_SHIFT, 0, win32con.KEYEVENTF_KEYUP, 0)
                if not js("b.focus();document.activeElement===b"):
                    raise RuntimeError("Second field did not receive focus")
                if mode == "interrupt" and not js("a.focus();a.setSelectionRange(1,2);document.activeElement===a"):
                    raise RuntimeError("First field did not regain focus")
            yield from run_receiver(folder, "edge", hwnd, prepare, lambda: js("[a.value,b.value,...keys,...pastes]"),
                                    probe, ("readonly", "password", "change", "interrupt"), change)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--interactive", required=True, action="store_true")
    parser.add_argument("--receiver", choices=("edge", "custom"), required=True)
    parser.add_argument("--probe", action="store_true")
    args = parser.parse_args()
    if gui.FindWindow("ClipRope.Listener", None):
        raise RuntimeError("Exit the daily instance before using the interactive paste fixture")
    ctypes.windll.user32.SetProcessDpiAwarenessContext(-4)
    with tempfile.TemporaryDirectory(prefix="ClipRope-paste-") as temp:
        try:
            cases = list((edge if args.receiver == "edge" else custom)(Path(temp), args.probe))
        except Exception:
            log = Path(temp) / args.receiver / "qualification.log"
            if args.probe and log.is_file():
                print(log.read_text()[-2400:], flush=True)
            report = Path(temp) / args.receiver / "diagnostics.txt"
            if report.is_file():
                print(next((line for line in report.read_text().splitlines()
                            if line.startswith("Target verification timeouts")), "No timeout counter"), flush=True)
            raise
    print(json.dumps(cases))
