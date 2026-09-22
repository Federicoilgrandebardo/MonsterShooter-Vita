#!/usr/bin/env python3
"""Monster Shooter for PS Vita - setup tool.

Put your own Monster Shooter APK in the same folder as this program and run it.
It checks the APK and sends the game data to your Vita, either over Wi-Fi
(VitaShell's FTP server) or to a folder / the Vita's memory card over USB.

    python3 setup_gui.py             # opens the window
    python3 setup_gui.py --selftest  # runs the checks, no window
"""

import ftplib
import queue
import sys
import threading
from pathlib import Path

import extract_apk as apk

# Everything lands under ux0:data/ on the Vita.
DATA_DIR = "monstershooter"
VPK_NAME = "monstershooter.vpk"
FTP_PORT = 1337
RETRIES = 3

# Where vitaGL / vitaShaRK look for the shader compiler, and where the kubridge
# plugin usually sits. Missing either is the classic reason a so-loader port
# shows a black screen, so the tool says so before the user finds out that way.
SHACCCG = ["ur0:/data/external/libshacccg.suprx", "ur0:/data/libshacccg.suprx"]
KUBRIDGE = ["ur0:/tai/kubridge.skprx", "ux0:/tai/kubridge.skprx"]


def app_dir():
    """The folder this program lives in - next to the .exe once packaged."""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


def find_apk(folder):
    """The first APK in folder that passes the checks, or None."""
    for p in sorted(Path(folder).glob("*.apk")):
        try:
            zf, _ = apk.open_apk(p)
        except apk.ApkError:
            continue
        zf.close()
        return p
    return None


def plan(zf, vpk=None):
    """(opener, target relative to ux0:data/, size) for every file to send."""
    items = []
    for member, target in apk.WANTED.items():
        size = zf.getinfo(member).file_size
        items.append((lambda m=member: zf.open(m), "%s/%s" % (DATA_DIR, target), size))
    if vpk:
        items.append((lambda: open(vpk, "rb"), VPK_NAME, Path(vpk).stat().st_size))
    return items


class FolderSink:
    """A folder on this computer, or the Vita's memory card in USB mode."""

    def __init__(self, folder):
        folder = Path(folder)
        # VitaShell's USB mode mounts ux0: itself, with app/ and data/ at the top.
        self.on_vita = (folder / "app").is_dir() and (folder / "data").is_dir()
        self.root = folder / "data" if self.on_vita else folder

    def size(self, rel):
        p = self.root / rel
        return p.stat().st_size if p.is_file() else None

    def put(self, rel, src, size, tick):
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        with open(p, "wb") as out:
            while True:
                chunk = src.read(1 << 16)
                if not chunk:
                    break
                out.write(chunk)
                tick(len(chunk))

    def reconnect(self):
        pass

    def close(self):
        pass


class FtpSink:
    """The Vita over Wi-Fi, through VitaShell's FTP server (press SELECT)."""

    def __init__(self, host, port=FTP_PORT, root="ux0:/data"):
        self.host, self.port, self.root = host, port, root
        self.ftp = None
        self.reconnect()

    def reconnect(self):
        self.close()
        self.ftp = ftplib.FTP()
        self.ftp.connect(self.host, self.port, timeout=15)
        self.ftp.login()               # VitaShell takes any login
        self.ftp.voidcmd("TYPE I")     # SIZE is only meaningful in binary mode

    def _cd(self, path, create=False):
        # One segment at a time, the way curl does it: the server wants the
        # device ("ux0:") on its own, not a full "ux0:/data/..." in one CWD.
        self.ftp.cwd("/")
        for seg in [s for s in path.split("/") if s]:
            try:
                self.ftp.cwd(seg)
            except ftplib.error_perm:
                if not create or seg.endswith(":"):
                    raise
                self.ftp.mkd(seg)
                self.ftp.cwd(seg)

    def _size_at(self, folder, name):
        try:
            self._cd(folder)
            return self.ftp.size(name)
        except ftplib.error_perm:
            return None

    def size(self, rel):
        folder, _, name = (self.root + "/" + rel).rpartition("/")
        return self._size_at(folder, name)

    def exists(self, path):
        folder, _, name = path.rpartition("/")
        return self._size_at(folder, name) is not None

    def put(self, rel, src, size, tick):
        folder, _, name = (self.root + "/" + rel).rpartition("/")
        self._cd(folder, create=True)
        self.ftp.storbinary("STOR " + name, src, blocksize=1 << 16,
                            callback=lambda b: tick(len(b)))

    def close(self):
        if self.ftp:
            try:
                self.ftp.quit()
            except Exception:
                self.ftp.close()
            self.ftp = None


def preflight(sink, log):
    """Read-only look for the two things the game cannot start without."""
    checks = (
        ("the shader compiler (libshacccg.suprx)", SHACCCG,
         "extract it from your own Vita with ShaRKBR33D"),
        ("the kubridge plugin", KUBRIDGE,
         "put kubridge.skprx in ur0:tai/ and add it to config.txt under *KERNEL"),
    )
    for label, paths, fix in checks:
        try:
            found = next((p for p in paths if sink.exists(p)), None)
        except (OSError, ftplib.Error):
            log("  could not check for %s" % label)
            continue
        if found:
            log("  found %s at %s" % (label, found))
        else:
            log("  WARNING: %s is missing - %s, or the game will not start." % (label, fix))
    # File presence only - a kubridge not listed in config.txt still
    # passes. Read and grep both config.txt files if that turns up in reports.


def prepare(items, sink, log, progress):
    """Send every item to sink and check its size once there.

    Files already present at full size are skipped, so pressing the button
    again after a dropped connection only sends what is still missing.
    Returns True when every file is in place.
    """
    total = sum(size for _, _, size in items)
    done = 0
    for opener, rel, size in items:
        if sink.size(rel) == size:
            done += size
            progress(done, total)
            log("  already there  %s" % rel)
            continue
        for attempt in range(1, RETRIES + 1):
            sent = [0]

            def tick(n, base=done):
                sent[0] += n
                progress(base + sent[0], total)

            try:
                with opener() as src:
                    sink.put(rel, src, size, tick)
                got = sink.size(rel)
                if got != size:
                    raise OSError("arrived as %s bytes instead of %s" % (got, size))
                break
            except (OSError, EOFError, ftplib.Error) as e:
                log("  retry %d of %d  %s  (%s)" % (attempt, RETRIES, rel, e))
                if attempt == RETRIES:
                    return False
                try:
                    sink.reconnect()
                except (OSError, ftplib.Error):
                    pass
        done += size
        progress(done, total)
        log("  sent  %s  (%s bytes)" % (rel, format(size, ",")))
    return True


def connection_help(host, port, err):
    return ("Could not reach the Vita at %s:%s (%s).\n\n"
            "On the Vita, open VitaShell and press SELECT. It shows an address like "
            "ftp://192.168.1.23:1337 - type those numbers here. The computer and the "
            "Vita must be on the same Wi-Fi." % (host, port, err))


def finished_message(ok, sink, sent_vpk):
    if not ok:
        return ("Not finished: the connection kept dropping.\n\n"
                "Press Prepare again. Files that already arrived are kept, "
                "so it only sends what is missing.")
    if isinstance(sink, FolderSink) and not sink.on_vita:
        where = sink.root / DATA_DIR
        return ("Done. The game data is in:\n%s\n\n"
                "Copy that whole folder into ux0:data/ on your Vita, then install "
                "monstershooter.vpk with VitaShell." % where)
    vpk_step = ("On the Vita, open VitaShell, go to ux0:data/ and press X on "
                "monstershooter.vpk to install it."
                if sent_vpk else "Install monstershooter.vpk with VitaShell.")
    return "Done. The game data is on your Vita.\n\nLast step: " + vpk_step


def run_gui():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    if sys.platform == "win32":
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)   # crisp on scaled screens
        except Exception:
            pass

    here = app_dir()
    vpk = here / VPK_NAME
    msgs = queue.Queue()

    root = tk.Tk()
    root.title("Monster Shooter - PS Vita setup")
    root.resizable(False, False)
    frm = ttk.Frame(root, padding=18)
    frm.grid(sticky="nsew")
    frm.columnconfigure(1, weight=1)
    bold = ("TkDefaultFont", 10, "bold")

    # 1. the APK
    ttk.Label(frm, text="1.  Your Monster Shooter APK", font=bold).grid(
        row=0, column=0, columnspan=3, sticky="w")
    apk_var = tk.StringVar()
    apk_ok = tk.StringVar()
    ttk.Label(frm, textvariable=apk_var, width=58).grid(
        row=1, column=0, columnspan=2, sticky="w", padx=(18, 0), pady=(4, 0))
    apk_path = [find_apk(here)]

    def show_apk():
        p = apk_path[0]
        if p is None:
            apk_var.set("No APK found next to this program.")
            apk_ok.set("Put your Monster Shooter .apk in this folder, or choose it:")
            return
        apk_var.set(p.name)
        try:
            zf, notes = apk.open_apk(p)
            zf.close()
            apk_ok.set("Recognised." + (" (Different build from the tested one.)" if notes else ""))
        except apk.ApkError as e:
            apk_path[0] = None
            apk_ok.set(str(e).splitlines()[0])

    def choose_apk():
        f = filedialog.askopenfilename(title="Choose the Monster Shooter APK",
                                       filetypes=[("Android package", "*.apk")])
        if f:
            apk_path[0] = Path(f)
            show_apk()

    ttk.Button(frm, text="Choose...", command=choose_apk).grid(row=1, column=2, sticky="e")
    ttk.Label(frm, textvariable=apk_ok, foreground="#555").grid(
        row=2, column=0, columnspan=3, sticky="w", padx=(18, 0))

    # 2. where the Vita is
    ttk.Label(frm, text="2.  Where is your Vita?", font=bold).grid(
        row=3, column=0, columnspan=3, sticky="w", pady=(16, 0))
    mode = tk.StringVar(value="ftp")
    ttk.Radiobutton(frm, text="Over Wi-Fi  (in VitaShell, press SELECT)", variable=mode,
                    value="ftp").grid(row=4, column=0, columnspan=3, sticky="w", padx=(18, 0), pady=(4, 0))
    net = ttk.Frame(frm)
    net.grid(row=5, column=0, columnspan=3, sticky="w", padx=(40, 0))
    ttk.Label(net, text="Address").grid(row=0, column=0)
    host = tk.StringVar()
    ttk.Entry(net, textvariable=host, width=16).grid(row=0, column=1, padx=(6, 10))
    ttk.Label(net, text="Port").grid(row=0, column=2)
    port = tk.StringVar(value=str(FTP_PORT))
    ttk.Entry(net, textvariable=port, width=6).grid(row=0, column=3, padx=(6, 10))
    ttk.Label(net, text="e.g. 192.168.1.23", foreground="#777").grid(row=0, column=4)

    ttk.Radiobutton(frm, text="To a folder, or the Vita's memory card over USB", variable=mode,
                    value="folder").grid(row=6, column=0, columnspan=3, sticky="w", padx=(18, 0), pady=(8, 0))
    folder = tk.StringVar()
    ttk.Label(frm, textvariable=folder, width=50).grid(row=7, column=0, columnspan=2, sticky="w", padx=(40, 0))

    def choose_folder():
        d = filedialog.askdirectory(title="Choose a folder, or the Vita's drive")
        if d:
            folder.set(d)
            mode.set("folder")

    ttk.Button(frm, text="Choose...", command=choose_folder).grid(row=7, column=2, sticky="e")

    # 3. the VPK
    send_vpk = tk.BooleanVar(value=vpk.is_file())
    ttk.Checkbutton(frm, variable=send_vpk,
                    text=("Also send monstershooter.vpk, found next to this program"
                          if vpk.is_file() else "No monstershooter.vpk next to this program"),
                    state="normal" if vpk.is_file() else "disabled").grid(
        row=8, column=0, columnspan=3, sticky="w", pady=(16, 0))

    go = ttk.Button(frm, text="Prepare my Vita")
    go.grid(row=9, column=0, columnspan=3, sticky="ew", pady=(14, 8), ipady=6)
    bar = ttk.Progressbar(frm, mode="determinate")
    bar.grid(row=10, column=0, columnspan=3, sticky="ew")
    log = tk.Text(frm, height=10, width=76, state="disabled", relief="flat",
                  background="#f4f4f4", font=("TkFixedFont", 9))
    log.grid(row=11, column=0, columnspan=3, sticky="ew", pady=(8, 0))

    def say(text):
        log.configure(state="normal")
        log.insert("end", text + "\n")
        log.see("end")
        log.configure(state="disabled")

    def work(apk_file, how, h, p, dest, with_vpk):
        out = lambda s: msgs.put(("log", s))
        try:
            zf, notes = apk.open_apk(apk_file)
        except apk.ApkError as e:
            msgs.put(("done", False, str(e)))
            return
        with zf:
            for n in notes:
                out("note: " + n)
            try:
                if how == "ftp":
                    out("Connecting to %s:%s ..." % (h, p))
                    sink = FtpSink(h, p)
                    out("Checking the Vita:")
                    preflight(sink, out)
                else:
                    sink = FolderSink(dest)
            except (OSError, ftplib.Error) as e:
                msgs.put(("done", False, connection_help(h, p, e)))
                return
            out("Sending the game data:")
            ok = prepare(plan(zf, vpk if with_vpk else None), sink, out,
                         lambda d, t: msgs.put(("progress", d, t)))
            sink.close()
        msgs.put(("done", ok, finished_message(ok, sink, with_vpk)))

    def start():
        if apk_path[0] is None:
            messagebox.showerror("No APK", "Choose your Monster Shooter APK first.")
            return
        how = mode.get()
        if how == "ftp":
            if not host.get().strip():
                messagebox.showerror("No address",
                                     "Type the address VitaShell shows when you press SELECT.")
                return
            try:
                p = int(port.get())
            except ValueError:
                messagebox.showerror("Wrong port", "The port is a number, usually 1337.")
                return
        elif not folder.get():
            messagebox.showerror("No folder", "Choose a folder, or the Vita's drive.")
            return
        else:
            p = None
        go.state(["disabled"])
        bar["value"] = 0
        threading.Thread(target=work, daemon=True,
                         args=(apk_path[0], how, host.get().strip(), p,
                               folder.get(), send_vpk.get() and vpk.is_file())).start()

    go.configure(command=start)

    def poll():
        try:
            while True:
                m = msgs.get_nowait()
                if m[0] == "log":
                    say(m[1])
                elif m[0] == "progress":
                    bar["maximum"] = m[2]
                    bar["value"] = m[1]
                else:
                    go.state(["!disabled"])
                    say("")
                    say(m[2])
                    (messagebox.showinfo if m[1] else messagebox.showerror)(
                        "Monster Shooter setup", m[2])
        except queue.Empty:
            pass
        root.after(100, poll)

    show_apk()
    root.after(100, poll)
    root.mainloop()


def _selftest():
    """Everything except the window and a real Vita: sinks, resume, retries."""
    import io
    import tempfile
    import zipfile

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        good = tmp / "good.apk"
        with zipfile.ZipFile(good, "w") as z:
            for i, member in enumerate(apk.WANTED):
                z.writestr(member, bytes([i]) * (1000 + i))
        (tmp / "junk.apk").write_bytes(b"not a zip")
        vpk = tmp / VPK_NAME
        vpk.write_bytes(b"v" * 500)
        quiet = lambda *a: None

        # The first APK in the folder is junk: the good one must still be found.
        assert find_apk(tmp) == good, "must skip an unusable APK"

        # The Vita's memory card over USB: app/ and data/ at the top.
        card = tmp / "card"
        (card / "app").mkdir(parents=True)
        (card / "data").mkdir()
        zf, _ = apk.open_apk(good)
        with zf:
            sink = FolderSink(card)
            assert sink.on_vita, "must recognise the Vita's drive"
            assert prepare(plan(zf, vpk), sink, quiet, quiet)
            for target in apk.WANTED.values():
                assert (card / "data" / DATA_DIR / target).is_file(), target
            assert (card / "data" / VPK_NAME).is_file()

            # Running it again sends nothing: everything is already there.
            lines = []
            assert prepare(plan(zf, vpk), FolderSink(card), lines.append, quiet)
            assert all("already there" in l for l in lines), "second run must skip"

            # A plain folder gets monstershooter/ to copy over by hand.
            plain = tmp / "plain"
            plain.mkdir()
            assert prepare(plan(zf), FolderSink(plain), quiet, quiet)
            assert (plain / DATA_DIR / "libClawNativeApp.so").is_file()
            assert "Copy that whole folder" in finished_message(True, FolderSink(plain), False)

            # A connection that drops once must be retried, not reported as done.
            class Flaky(FolderSink):
                fails = 1

                def put(self, rel, src, size, tick):
                    if Flaky.fails:
                        Flaky.fails -= 1
                        raise EOFError("dropped")
                    super().put(rel, src, size, tick)

            flaky_dir = tmp / "flaky"
            flaky_dir.mkdir()
            assert prepare(plan(zf), Flaky(flaky_dir), quiet, quiet), "must recover"

            # One that never comes back must give up and say so.
            class Dead(FolderSink):
                def put(self, *a):
                    raise EOFError("dropped")

            dead_dir = tmp / "dead"
            dead_dir.mkdir()
            assert not prepare(plan(zf), Dead(dead_dir), quiet, quiet), "must give up"

        # FTP path handling, against a stand-in that behaves like VitaShell's server.
        class FakeFTP:
            def __init__(self):
                self.cwd_path, self.dirs, self.files = [], {("ux0:", "data")}, {}

            def cwd(self, seg):
                if seg == "/":
                    self.cwd_path = []
                    return
                nxt = tuple(self.cwd_path + [seg])
                if len(nxt) > 1 and nxt not in self.dirs:
                    raise ftplib.error_perm("550 no such directory")
                self.cwd_path = list(nxt)

            def mkd(self, seg):
                assert not seg.endswith(":"), "must never try to create a device"
                self.dirs.add(tuple(self.cwd_path + [seg]))

            def size(self, name):
                key = tuple(self.cwd_path + [name])
                if key not in self.files:
                    raise ftplib.error_perm("550 no such file")
                return len(self.files[key])

            def storbinary(self, cmd, src, blocksize, callback):
                data = src.read()
                callback(data)
                self.files[tuple(self.cwd_path + [cmd.split(" ", 1)[1]])] = data

        class FakeSink(FtpSink):
            def reconnect(self):
                self.ftp = self.ftp or FakeFTP()

        fs = FakeSink("vita")
        fs.put("monstershooter/assets/data.pak", io.BytesIO(b"x" * 7), 7, quiet)
        assert fs.size("monstershooter/assets/data.pak") == 7
        assert ("ux0:", "data", "monstershooter", "assets") in fs.ftp.dirs
        assert not fs.exists("ur0:/data/libshacccg.suprx")

    print("selftest ok")


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
        _selftest()
    else:
        run_gui()
