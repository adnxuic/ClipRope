"""Exercise the production installer with disposable registry, program and shell paths."""
from contextlib import closing
import csv
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import time
import winreg


def wait_for(condition):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if condition():
            return
        time.sleep(0.05)
    raise RuntimeError("Timed out waiting for the setup fixture.")


def run(executable, arguments, expected=0):
    # NSIS consumes the literal remainder after /D= or _?=, including spaces.
    result = subprocess.run(f'"{executable}" {arguments}', timeout=30)
    if result.returncode != expected:
        raise RuntimeError(f"{executable.name} {arguments}: expected {expected}, got {result.returncode}")


def exercise(compiler, fixture, namespace):
    repo = Path(__file__).resolve().parents[2]
    script = repo / "tests/integration/package_directory.nsi"
    local = fixture / "local"
    payload = fixture / "payload"
    local.mkdir()
    payload.mkdir()
    helper = repo / "build/app/cliprope_profile_support.exe"
    subprocess.run([helper, "--check-new-install", fixture / "prospective", local], check=True)
    assert not any(local.iterdir()) and not (fixture / "prospective").exists()
    defines = ["/V2", "/INPUTCHARSET", "UTF8", f"/DFIXTURE={fixture}", f"/DAPP_BIN={payload}"]
    subprocess.run([compiler, *defines, "/DSHORTCUT_PROBE", script], check=True)
    versions = ("1.0", "2.0")
    packages = []
    for version in versions:
        (payload / "cliprope_headless.exe").write_bytes(version.encode())
        package = fixture / f"Setup-{version}.exe"
        subprocess.run([compiler, *defines, f"/DSOURCE={repo}", f"/DREGISTRY={namespace}",
                        f"/DTOKEN={namespace.split('.')[-1]}", f"/DSUPPORT={helper}", f"/DOUTPUT={package}",
                        f"/DVERSION={version}", f"/DREVISION={version}", f"/DAPP_ICON={repo / 'resources/icons/cliprope.ico'}",
                        f"/DNOTICES={repo / 'packaging/windows/ThirdPartyNotices.txt'}", script], check=True)
        packages.append(package)
    old, new = packages

    def read(key, name):
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, namespace + "\\" + key, 0,
                                winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as item:
                return winreg.QueryValueEx(item, name)[0]
        except FileNotFoundError:
            return None

    shortcut = fixture / "ClipRope.lnk"
    def verify(root, version, startup):
        current = root / "current"
        image = current / "ClipRope.exe"
        assert read("Product", "InstallLocation") == str(root)
        assert read("Product", "DisplayVersion") == version
        assert read("Product", "DisplayIcon") == str(image)
        assert read("Product", "QuietUninstallString") == f'"{current / "Uninstall.exe"}" /S'
        assert read("Startup", "Owner") == str(image)
        assert read("Run", "ClipRope") == (f'"{image}"' if startup else None)
        assert (current / "cliprope_headless.exe").read_bytes() == version.encode()
        receipt = fixture / "launch.ini"
        receipt.write_bytes(b"\xff\xfe")
        os.startfile(shortcut)
        wait_for(lambda: receipt.stat().st_size > 2)
        text = receipt.read_text(encoding="utf-16")
        assert f"Executable={image}" in text, text

    def uninstall(root):
        run(root / "current/Uninstall.exe", "/S")
        wait_for(lambda: not root.exists() and read("Product", "InstallLocation") is None)
        assert not shortcut.exists() and read("Startup", "Owner") is None and read("Run", "ClipRope") is None

    default = local / "Programs/ClipRope"
    custom = fixture / "My Apps 应用程序" / "ClipRope"
    ignored = fixture / "unused"
    history = fixture / "moved history"
    history.mkdir()
    sentinel = history / "history.db"
    sentinel.write_bytes(b"retained fixture history")
    settings = local / "ClipRope.Settings"
    settings.mkdir()
    with closing(sqlite3.connect(settings / "location.db")) as db:
        db.executescript("CREATE TABLE location(active,pending,previous); CREATE TABLE transfers(target);")
        db.execute("INSERT INTO location VALUES(?,?,?)", (str(history), str(fixture / "pending"), ""))
        db.commit()
    original_locator = (settings / "location.db").read_bytes()
    unrelated = fixture / "occupied"
    unrelated.mkdir()
    (unrelated / "notes.txt").write_bytes(b"preserve")
    junction = fixture / "junction"
    subprocess.run(["cmd", "/d", "/c", "mklink", "/J", junction, history], check=True)
    try:
        for target in ("", "relative", Path(fixture.anchor), unrelated, settings / "program", local / "ClipRope", history / "program",
                       fixture / "pending/program", junction / "deeper/app", fixture / ("x" * 230)):
            run(new, f"/S /D={target}", 2)
            assert read("Product", "InstallLocation") is None and not shortcut.exists()
    finally:
        junction.rmdir()
    assert (unrelated / "notes.txt").read_bytes() == b"preserve"
    assert not (history / "program").exists() and not (history / "deeper").exists()
    denied = fixture / "denied"
    denied.mkdir()
    sid = next(csv.reader(subprocess.check_output(["whoami", "/user", "/fo", "csv", "/nh"],
                                                  text=True, encoding="oem").splitlines()))[1]
    subprocess.run(["icacls", denied, "/deny", f"*{sid}:(AD)"], check=True)
    try:
        run(new, f"/S /D={denied / 'program'}", 2)
        assert not (denied / "program").exists()
    finally:
        subprocess.run(["icacls", denied, "/remove:d", "*" + sid], check=True)
    assert not shortcut.exists() and read("Product", "InstallLocation") is None
    assert original_locator == (settings / "location.db").read_bytes()
    assert {p.name for p in settings.iterdir()} == {"location.db"}

    for root in (default, custom):
        if root == custom:
            root.mkdir(parents=True)  # An explicitly selected empty folder.
        run(old, "/S /NOSTARTUP" + (f" /D={custom}" if root == custom else ""))
        verify(root, versions[0], False)
        if root == default:
            # Older default installations could predate the separate startup owner marker.
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, namespace + "\\Startup", 0,
                                winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY) as key:
                winreg.DeleteValue(key, "Owner")
        run(new, f"/S /D={ignored}")
        verify(root, versions[1], False)
        run(new, f"/S /STARTUP /ROLLBACK /D={ignored}")
        verify(root, versions[0], True)
        assert not ignored.exists()
        uninstall(root)
        assert sentinel.read_bytes() == b"retained fixture history"
    run(new, f"/S /D={custom}\\")  # A trailing separator on a now absent leaf.
    verify(custom, versions[1], True)
    uninstall(custom)
    print("Default and custom installation, shortcut launch, upgrade, rollback, rejection and reinstall passed.")
    if capture := os.environ.get("CLIPROPE_SETUP_CAPTURES"):
        from package_directory_visuals import review_wizard
        review_wizard(new, unrelated, custom, Path(capture), "cancel")
        assert not custom.exists() and not shortcut.exists() and read("Product", "InstallLocation") is None
        review_wizard(old, unrelated, custom, Path(capture), "install")
        verify(custom, versions[0], True)
        review_wizard(new, unrelated, custom, Path(capture), "upgrade")
        verify(custom, versions[1], True)
        uninstall(custom)


if __name__ == "__main__":
    exercise(sys.argv[1], Path(sys.argv[2]), sys.argv[3])
