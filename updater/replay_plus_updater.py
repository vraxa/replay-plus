#!/usr/bin/env python3
"""Replay Plus Updater for macOS and Linux (skate. runs there through CrossOver, Whisky, Proton, Wine...).

Installs and updates Replay Plus (a ReSkate.dll with the Animator Studio built in) and the ReSkate launcher that goes
with it, inside the Wine bottle or prefix the game is in. Replay Plus comes from its own GitHub releases, the launcher
from ReSkate's own releases (the version that build was made for), and every file is checked against the SHA-256 the
Replay Plus release pins before anything in the game folder is touched. Same as the Windows updater.

Command line: --folder <ReSkate folder>, --prerelease, --from <folder with replay-plus.json and ReSkate.dll>,
--install / --restore (no window; exit 0 done, 1 failed). The log is updater.log in the data folder.
"""
import argparse
import hashlib
import json
import os
import queue
import re
import shutil
import ssl
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from pathlib import Path

VERSION = "1.1"
REPO = "vraxa/replay-plus"
STUDIO_REPO = "FourthStudio/ReSkate-Animator-Studio"
RESKATE_REPO = "Dingo-Shenanigans/ReSkate"
API = "https://api.github.com/repos/"
AGENT = "ReplayPlusUpdater"
RECORD = "replay-plus-updater.json"
ORIGINAL = ".before-replay-plus"


def data_folder():
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "Replay Plus Updater"
    if sys.platform == "win32":
        return Path(os.environ.get("LOCALAPPDATA", Path.home())) / "ReSkate" / "Replay Plus"
    return Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share") / "replay-plus-updater"


def write_log(line):
    try:
        folder = data_folder()
        folder.mkdir(parents=True, exist_ok=True)
        with open(folder / "updater.log", "a", encoding="utf-8") as f:
            f.write(datetime.now().strftime("%Y-%m-%d %H:%M:%S") + "  " + line + "\n")
    except OSError:
        pass


def ssl_context():
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except Exception:
        return ssl.create_default_context()


SSL = ssl_context()


def open_url(url):
    headers = {"User-Agent": AGENT, "Accept": "application/vnd.github+json" if "api.github.com" in url else "*/*"}
    token = os.environ.get("REPLAY_PLUS_GITHUB_TOKEN")
    if token and "api.github.com" in url:
        headers["Authorization"] = "Bearer " + token
    return urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=60, context=SSL)


def get_text(url):
    with open_url(url) as response:
        return response.read().decode("utf-8")


def download(url, path, size, progress):
    part = path.with_name(path.name + ".part")
    with open_url(url) as response, open(part, "wb") as out:
        total = int(response.headers.get("Content-Length") or size or 0)
        done = 0
        while True:
            chunk = response.read(1 << 16)
            if not chunk:
                break
            out.write(chunk)
            done += len(chunk)
            progress(done, total)
    os.replace(part, path)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def numbers(v):
    return [int(n) for n in re.findall(r"\d+", (v or "").lstrip("vV"))]


def compare(a, b):
    x, y = numbers(a), numbers(b)
    for i in range(max(len(x), len(y))):
        p, q = (x[i] if i < len(x) else 0), (y[i] if i < len(y) else 0)
        if p != q:
            return -1 if p < q else 1
    return 0


class Pin:
    """A file one Replay Plus release pins: where it is and what it must hash to."""

    def __init__(self, d):
        self.repo = d.get("repo") or REPO
        self.tag = d.get("tag") or ""
        self.name = d.get("name") or ""
        self.sha256 = (d.get("sha256") or "").lower()
        self.size = int(d.get("size") or 0)
        if not self.name or len(self.sha256) != 64:
            raise ValueError("replay-plus.json names a file without its SHA-256")

    @property
    def url(self):
        return "https://github.com/%s/releases/download/%s/%s" % (self.repo, self.tag, urllib.parse.quote(self.name))


class Build:
    """A Replay Plus release, from its replay-plus.json."""

    def __init__(self, text, tag, prerelease=False):
        d = json.loads(text)
        if d.get("schema") != 1:
            raise ValueError("replay-plus.json is not one this updater knows (a newer updater may be needed)")
        self.tag, self.prerelease = tag, prerelease
        self.replay_plus, self.studio, self.reskate = d.get("replay_plus"), d.get("studio"), d.get("reskate")
        self.updater = d.get("updater")
        self.dll = Pin(d["dll"]) if d.get("dll") else None
        self.launcher = Pin(d["launcher"]) if d.get("launcher") else None
        self.launcher_json = Pin(d["launcher_json"]) if d.get("launcher_json") else None
        if not self.dll or not self.replay_plus:
            raise ValueError("replay-plus.json has no ReSkate.dll in it")

    @property
    def title(self):
        return "Replay Plus %s (Animator Studio %s, ReSkate %s)" % (self.replay_plus, self.studio, self.reskate)


OURS = re.compile(rb"(\d+(?:\.\d+)+)\+studio-(\d+(?:\.\d+)+)\.reskate-([0-9A-Za-z.\-]+?)\x00")
STUDIO = re.compile(rb"(\d+\.\d+\.\d+)\+reskate\.([0-9A-Za-z.\-]+?)\x00")


class Installed:
    def __init__(self, dll):
        self.has_dll = self.ours = False
        self.sha256 = self.replay_plus = self.studio = self.reskate = None
        if not dll.exists():
            self.describe = "no ReSkate.dll here yet"
            return
        self.has_dll = True
        data = dll.read_bytes()
        self.sha256 = hashlib.sha256(data).hexdigest()
        m = OURS.search(data)
        if m:
            self.ours = True
            self.replay_plus, self.studio, self.reskate = (g.decode() for g in m.groups())
            self.describe = "Replay Plus %s (Animator Studio %s, ReSkate %s)" % (self.replay_plus, self.studio, self.reskate)
            return
        m = STUDIO.search(data)
        if m:
            self.studio, self.reskate = (g.decode() for g in m.groups())
            self.describe = "Animator Studio %s (ReSkate %s), no Replay Plus" % (self.studio, self.reskate)
            return
        self.describe = "ReSkate without the Animator Studio or Replay Plus"


def is_reskate_folder(folder):
    return bool(folder) and (Path(folder) / "ReSkateLauncher.exe").is_file()


def candidate_roots():
    home = Path.home()
    roots = []
    if sys.platform == "darwin":
        roots += list((home / "Library/Application Support/CrossOver/Bottles").glob("*/drive_c"))
        roots += list((home / "Library/Containers/com.isaacmarovitz.Whisky/Bottles").glob("*/drive_c"))
        roots += list((home / "Library/Application Support/com.isaacmarovitz.Whisky/Bottles").glob("*/drive_c"))
    else:
        for steam in (home / ".local/share/Steam", home / ".steam/steam",
                      home / ".var/app/com.valvesoftware.Steam/.local/share/Steam"):
            roots += list((steam / "steamapps/compatdata").glob("*/pfx/drive_c"))
        roots += list((home / ".local/share/bottles/bottles").glob("*/drive_c"))
        roots += list((home / ".var/app/com.usebottles.bottles/data/bottles/bottles").glob("*/drive_c"))
    roots += [home / ".wine/drive_c", home / "Games", home / "Desktop", home / "Downloads"]
    return [r for r in roots if r.is_dir()]


def find_reskate(seconds=4.0):
    """The first folder with ReSkateLauncher.exe in the usual bottles and prefixes (a few levels deep)."""
    deadline = time.monotonic() + seconds
    skip = {"windows", "programdata", "$recycle.bin", "appdata", "node_modules", ".cache"}
    for root in candidate_roots():
        stack = [(root, 0)]
        while stack:
            if time.monotonic() > deadline:
                return None
            folder, depth = stack.pop()
            if is_reskate_folder(folder):
                return str(folder)
            if depth >= 5:
                continue
            try:
                for entry in os.scandir(folder):
                    if entry.is_dir(follow_symlinks=False) and entry.name.lower() not in skip:
                        stack.append((Path(entry.path), depth + 1))
            except OSError:
                pass
    return None


def running():
    """skate. or the ReSkate launcher, if either runs (under Wine their command lines name the .exe)."""
    try:
        if sys.platform == "win32":
            out = subprocess.run(["tasklist", "/fo", "csv", "/nh"], capture_output=True, text=True).stdout
        else:
            out = subprocess.run(["ps", "-A", "-o", "command="], capture_output=True, text=True).stdout
    except OSError:
        return None
    for line in out.splitlines():
        low = line.lower()
        if "reskatelauncher.exe" in low:
            return "the ReSkate launcher"
        if re.search(r'(^|[\\/" ])skate\.exe', low):
            return "skate."
    return None


def studio_dll_pin(release):
    """An Animator Studio release's ReSkate.dll: where it is and its SHA-256 (GitHub's own, else its .sha256 file)."""
    tag = release["tag_name"]
    assets = release.get("assets") or []
    dll_asset = next((a for a in assets if a.get("name") == "ReSkate.dll"), None)
    sum_asset = next((a for a in assets if a.get("name") == "ReSkate.dll.sha256"), None)
    if not dll_asset:
        raise RuntimeError("the Animator Studio's release %s has no ReSkate.dll" % tag)
    digest = dll_asset.get("digest") or ""
    sha = digest[7:] if digest.startswith("sha256:") else None
    if not sha and sum_asset:
        m = re.search(r"[0-9a-fA-F]{64}", get_text(sum_asset["browser_download_url"]))
        sha = m.group(0) if m else None
    if not sha or len(sha) != 64:
        raise RuntimeError("couldn't get the Animator Studio's checksum, so nothing was changed")
    return Pin({"repo": STUDIO_REPO, "tag": tag, "name": "ReSkate.dll", "sha256": sha, "size": dll_asset.get("size") or 0}), tag


class Updater:
    def __init__(self, folder=None, prerelease=False, from_folder=None, log=None, progress=None):
        self.folder = Path(folder) if folder else None
        self.prerelease = prerelease
        self.from_folder = Path(from_folder) if from_folder else None
        self.log = log or (lambda text: None)
        self.progress = progress or (lambda done, total, name: None)

    # ---- What is out there -------------------------------------------------------------------------------------
    def latest(self):
        if self.from_folder:
            return Build((self.from_folder / "replay-plus.json").read_text(encoding="utf-8"), "local")
        if not self.prerelease:
            release = json.loads(get_text(API + REPO + "/releases/latest"))
            if not self.has_asset(release, "replay-plus.json"):
                raise ValueError("the newest Replay Plus release (%s) can't be installed by this updater" % release.get("tag_name"))
        else:
            release = next((r for r in json.loads(get_text(API + REPO + "/releases?per_page=10"))
                            if not r.get("draft") and self.has_asset(r, "replay-plus.json")), None)
            if not release:
                raise ValueError("no Replay Plus release the updater can install")
        tag = release["tag_name"]
        text = get_text("https://github.com/%s/releases/download/%s/replay-plus.json" % (REPO, tag))
        return Build(text, tag, bool(release.get("prerelease")))

    @staticmethod
    def has_asset(release, name):
        return any(a.get("name") == name for a in release.get("assets") or [])

    @staticmethod
    def latest_studio():
        try:
            return json.loads(get_text(API + STUDIO_REPO + "/releases/latest")).get("tag_name")
        except Exception:
            return None

    def inspect(self):
        return Installed(self.folder / "ReSkate.dll")

    # ---- Installing ----------------------------------------------------------------------------------------------
    def install(self, build):
        busy = running()
        if busy:
            raise RuntimeError("close %s first" % busy)
        cache = data_folder() / "downloads" / build.tag
        cache.mkdir(parents=True, exist_ok=True)
        dll, launcher, launcher_json = (self.folder / "ReSkate.dll", self.folder / "ReSkateLauncher.exe",
                                        self.folder / "launcher.json")
        self.log("Getting " + build.title)
        new_dll = self.fetch(build.dll, cache, ours=True)
        new_launcher = new_json = None
        if build.launcher and not self.matches(launcher, build.launcher):
            self.log("Getting the ReSkate %s launcher from ReSkate's own release" % build.launcher.tag)
            new_launcher = self.fetch(build.launcher, cache)
        if build.launcher_json and not self.matches(launcher_json, build.launcher_json):
            new_json = self.fetch(build.launcher_json, cache)

        busy = running()
        if busy:
            raise RuntimeError("close %s first" % busy)
        record = self.read_record() or self.keep_originals(dll, launcher, launcher_json)
        self.put(new_dll, dll)
        if new_launcher:
            self.put(new_launcher, launcher)
        if new_json:
            self.put(new_json, launcher_json)
        before = self.set_updates(self.folder / "ReSkateLauncher.settings.json", False)
        record.setdefault("updates_before", before)
        record.update(installed=build.replay_plus, studio=build.studio, reskate=build.reskate, tag=build.tag,
                      when=datetime.now().strftime("%Y-%m-%d %H:%M"))
        self.write_record(record)
        self.log("Done: %s is installed. Start ReSkate like you normally do." % build.title)

    def fetch(self, pin, cache, ours=False):
        path = cache / pin.name
        if ours and self.from_folder:
            path = self.from_folder / pin.name
            self.log("Taking %s from %s" % (pin.name, self.from_folder))
        elif not self.matches(path, pin):
            download(pin.url, path, pin.size, lambda done, total: self.progress(done, total, pin.name))
        if not self.matches(path, pin):
            if cache in path.parents:
                path.unlink(missing_ok=True)
            raise RuntimeError("%s didn't download right (its SHA-256 doesn't match), so nothing was changed. Try again" % pin.name)
        self.log("%s: checked (SHA-256 %s...)" % (pin.name, pin.sha256[:12]))
        return path

    @staticmethod
    def matches(path, pin):
        if not path.is_file():
            return False
        if pin.size and path.stat().st_size != pin.size:
            return False
        return sha256_of(path) == pin.sha256

    def put(self, source, target):
        if target.exists():
            shutil.copyfile(target, target.with_name(target.name + ".previous"))
        shutil.copyfile(source, target)
        self.log("Installed " + target.name)

    def keep_originals(self, dll, launcher, launcher_json):
        originals = {}
        if dll.exists():
            if not Installed(dll).ours:
                shutil.copyfile(dll, dll.with_name(dll.name + ORIGINAL))
                originals["ReSkate.dll"] = dll.name + ORIGINAL
            else:
                for name in ("ReSkate.dll.studio", "ReSkate.dll.original"):
                    kept = self.folder / name
                    if kept.exists() and not Installed(kept).ours:
                        originals["ReSkate.dll"] = name
                        break
        for path in (launcher, launcher_json):
            if path.exists():
                shutil.copyfile(path, path.with_name(path.name + ORIGINAL))
                originals[path.name] = path.name + ORIGINAL
        self.log("Kept what you had before: " + (", ".join(originals.values()) or "nothing to keep"))
        return {"originals": originals}

    def remove_replay_plus(self):
        """Replay Plus out, the Animator Studio kept: the Studio's own ReSkate.dll of the version this Replay Plus was
        built on (so it fits the launcher), from its own release and checked against its SHA-256. The launcher, the
        settings and what was kept from before Replay Plus stay as they are."""
        busy = running()
        if busy:
            raise RuntimeError("close %s first" % busy)
        installed = self.inspect()
        if not installed.ours:
            raise RuntimeError("Replay Plus isn't installed here")
        try:
            release = json.loads(get_text(API + STUDIO_REPO + "/releases/tags/" + urllib.parse.quote(installed.studio)))
        except urllib.error.HTTPError:
            release = json.loads(get_text(API + STUDIO_REPO + "/releases/latest"))
        pin, tag = studio_dll_pin(release)
        cache = data_folder() / "downloads" / ("studio-" + tag)
        cache.mkdir(parents=True, exist_ok=True)
        self.log("Getting Animator Studio %s from its own release" % tag)
        studio_dll = self.fetch(pin, cache)
        check = Installed(studio_dll)
        if check.ours or not check.studio:
            raise RuntimeError("that download isn't the Animator Studio's ReSkate.dll, so nothing was changed")
        busy = running()
        if busy:
            raise RuntimeError("close %s first" % busy)
        self.put(studio_dll, self.folder / "ReSkate.dll")
        record = self.read_record()
        if record is not None:
            record.update(installed=None, now="Animator Studio " + check.studio)
            self.write_record(record)
        self.log("Done: Replay Plus is out and Animator Studio %s is in. Your presets and settings are still there if you want it back." % check.studio)

    def update_studio(self):
        """The newest Animator Studio on its own (without Replay Plus): its ReSkate.dll from its own release, checked,
        and the ReSkate launcher it is built on (its version string names it) from ReSkate's release of that version,
        checked against that release's launcher.json. "updates" stays off, as the Studio needs."""
        busy = running()
        if busy:
            raise RuntimeError("close %s first" % busy)
        if self.inspect().ours:
            raise RuntimeError("Replay Plus has the Studio built in, so it gets newer Studios with its own updates")
        pin, tag = studio_dll_pin(json.loads(get_text(API + STUDIO_REPO + "/releases/latest")))
        cache = data_folder() / "downloads" / ("studio-" + tag)
        cache.mkdir(parents=True, exist_ok=True)
        self.log("Getting Animator Studio %s from its own release" % tag)
        studio_dll = self.fetch(pin, cache)
        check = Installed(studio_dll)
        if check.ours or not check.studio:
            raise RuntimeError("that download isn't the Animator Studio's ReSkate.dll, so nothing was changed")
        new_launcher = new_json = None
        launcher_path = self.folder / "ReSkateLauncher.exe"
        if check.reskate:
            json_path, reskate_tag = cache / "launcher.json", None
            for t in (check.reskate, "v" + check.reskate):
                try:
                    download("https://github.com/%s/releases/download/%s/launcher.json" % (RESKATE_REPO, t), json_path, 0,
                             lambda done, total: None)
                    reskate_tag = t
                    break
                except urllib.error.HTTPError:
                    pass
            launcher = json.loads(json_path.read_text(encoding="utf-8-sig")).get("launcher") if reskate_tag else None
            if not launcher or len(launcher.get("sha256") or "") != 64:
                self.log("(couldn't find the ReSkate %s launcher, so the launcher stays as it is)" % check.reskate)
            else:
                launcher_pin = Pin({"repo": RESKATE_REPO, "tag": reskate_tag, "name": "ReSkateLauncher.exe",
                                    "sha256": launcher["sha256"], "size": launcher.get("size") or 0})
                if not self.matches(launcher_path, launcher_pin):
                    self.log("Getting the ReSkate %s launcher from ReSkate's own release" % check.reskate)
                    new_launcher = self.fetch(launcher_pin, cache)
                new_json = json_path
        busy = running()
        if busy:
            raise RuntimeError("close %s first" % busy)
        self.put(studio_dll, self.folder / "ReSkate.dll")
        if new_launcher:
            self.put(new_launcher, launcher_path)
        if new_json:
            self.put(new_json, self.folder / "launcher.json")
        self.set_updates(self.folder / "ReSkateLauncher.settings.json", False)
        self.log("Done: Animator Studio %s is in%s. Start ReSkate like you normally do." % (
            check.studio, ", with the ReSkate %s launcher" % check.reskate if new_launcher else ""))

    def can_restore(self):
        record = self.read_record() or {}
        return bool(record.get("originals")) or (self.folder / "ReSkate.dll.studio").exists()

    def restore(self):
        busy = running()
        if busy:
            raise RuntimeError("close %s first" % busy)
        record = self.read_record() or {}
        originals = record.get("originals") or {}
        if not originals:
            if not (self.folder / "ReSkate.dll.studio").exists():
                raise RuntimeError("there's nothing from before Replay Plus to put back")
            originals = {"ReSkate.dll": "ReSkate.dll.studio"}
        for name, kept in originals.items():
            kept_path = self.folder / kept
            if not kept_path.exists():
                self.log("(%s isn't there any more, skipped)" % kept)
                continue
            self.put(kept_path, self.folder / name)
        if record.get("updates_before") is True:
            self.set_updates(self.folder / "ReSkateLauncher.settings.json", True)
        (self.folder / RECORD).unlink(missing_ok=True)
        self.log("Put back what you had before Replay Plus.")

    def read_record(self):
        try:
            return json.loads((self.folder / RECORD).read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None

    def write_record(self, record):
        (self.folder / RECORD).write_text(json.dumps(record), encoding="utf-8")

    def set_updates(self, path, value):
        """"updates" in ReSkateLauncher.settings.json, changed in place. Without it the launcher counts updates as on
        and would put its own ReSkate.dll back. Returns what it was (None: not there)."""
        word = "true" if value else "false"
        if not path.exists():
            path.write_bytes(('{\r\n    "updates": %s\r\n}\r\n' % word).encode("utf-8"))
            self.log('Made ReSkateLauncher.settings.json with "updates": ' + word)
            return None
        raw = path.read_bytes()
        bom = raw.startswith(b"\xef\xbb\xbf")
        text = raw[3 if bom else 0:].decode("utf-8")
        m = re.search(r'"updates"\s*:\s*(true|false)', text)
        if m:
            before = m.group(1) == "true"
            if before == value:
                return before
            text = text[:m.start(1)] + word + text[m.end(1):]
        else:
            before = None
            brace = text.find("{")
            if brace < 0:
                raise RuntimeError("ReSkateLauncher.settings.json isn't readable")
            empty = text[brace + 1:].strip().startswith("}")
            newline = "\r\n" if "\r\n" in text else "\n"
            text = text[:brace + 1] + newline + '    "updates": ' + word + ("" if empty else ",") + text[brace + 1:]
        path.write_bytes((b"\xef\xbb\xbf" if bom else b"") + text.encode("utf-8"))
        self.log('Set "updates" to %s in ReSkateLauncher.settings.json%s' % (word, "" if value else " (so the launcher doesn't swap the dll back)"))
        return before


def sentence(text):
    text = str(text) or "something went wrong."
    text = text[0].upper() + text[1:]
    return text if text.endswith(".") else text + "."


def explain(error):
    if isinstance(error, PermissionError):
        return "The updater isn't allowed to write to that folder."
    if isinstance(error, urllib.error.URLError):
        return "Couldn't reach GitHub (%s)." % getattr(error, "reason", error)
    return sentence(error)


# ---- The window ------------------------------------------------------------------------------------------------------
def run_window(updater, saved_path):
    import tkinter as tk
    import tkinter.font as tkfont
    from tkinter import filedialog, messagebox, ttk

    root = tk.Tk()
    root.title("Replay Plus Updater " + VERSION)
    root.minsize(560, 380)
    root.geometry("660x430")
    events = queue.Queue()

    frame = ttk.Frame(root, padding=14)
    frame.pack(fill="both", expand=True)
    frame.columnconfigure(1, weight=1)
    frame.rowconfigure(6, weight=1)
    bold = tkfont.nametofont("TkDefaultFont").copy()
    bold.configure(weight="bold")
    labels = {}
    for row, name in enumerate(("ReSkate folder", "You have", "Newest")):
        ttk.Label(frame, text=name, foreground="#777").grid(row=row, column=0, sticky="w", padx=(0, 12), pady=4)
        labels[name] = ttk.Label(frame, text="", font=bold)
        labels[name].grid(row=row, column=1, sticky="w", pady=4)
    change = ttk.Button(frame, text="Change...")
    change.grid(row=0, column=2, sticky="e")
    notice = ttk.Label(frame, text="", foreground="#a05a00", wraplength=600, justify="left")
    notice.grid(row=3, column=0, columnspan=3, sticky="w", pady=(4, 4))
    buttons = ttk.Frame(frame)
    buttons.grid(row=4, column=0, columnspan=3, sticky="w", pady=(4, 8))
    install = ttk.Button(buttons, text="Install")
    remove = ttk.Button(buttons, text="Remove Replay Plus")
    studio_button = ttk.Button(buttons, text="Update Animator Studio")
    restore = ttk.Button(buttons, text="Put back what I had before")
    open_folder = ttk.Button(buttons, text="Open the folder")

    def lay_out(shown):
        for b in (install, remove, studio_button, restore, open_folder):
            b.pack_forget()
        for b in (install, remove, studio_button, restore, open_folder):
            if b in shown:
                b.pack(side="left", padx=(0, 8))

    lay_out((install, restore, open_folder))
    bar = ttk.Progressbar(frame, maximum=1000)
    bar.grid(row=5, column=0, columnspan=3, sticky="ew", pady=(0, 8))
    log = tk.Text(frame, height=10, wrap="word", state="disabled", relief="solid", borderwidth=1,
                  font=tkfont.nametofont("TkDefaultFont"), padx=6, pady=4)
    log.grid(row=6, column=0, columnspan=3, sticky="nsew")
    state = {"latest": None, "busy": False, "said": False, "installed": None, "studio": None}

    def short(path, most=58):
        path = str(path)
        return path if len(path) <= most else "..." + path[-(most - 3):]

    def say(text):
        write_log(text)
        events.put(("log", text))

    updater.log = say
    updater.progress = lambda done, total, name: events.put(("progress", (done, total, name)))

    def pump():
        try:
            while True:
                kind, value = events.get_nowait()
                if kind == "log":
                    log.configure(state="normal")
                    log.insert("end", value + "\n")
                    log.see("end")
                    log.configure(state="disabled")
                elif kind == "progress":
                    done, total, name = value
                    bar["value"] = int(done * 1000 / total) if total else 0
                    install.configure(text="%s %.1f / %.1f MB" % (name, done / 1048576, total / 1048576))
                elif kind == "state":
                    show(*value)
                elif kind == "found":
                    if value:
                        updater.folder = Path(value)
                        labels["ReSkate folder"].configure(text=short(value))
                        say("Found it: " + value)
                        work()
                    else:
                        say("Couldn't find it on its own, so point me at the folder with ReSkateLauncher.exe in it.")
                        pick_folder()
        except queue.Empty:
            pass
        root.after(80, pump)

    def set_buttons(on):
        for b in (install, remove, studio_button, restore, change):
            b.state(["!disabled"] if on else ["disabled"])

    def show(installed, newest, studio, problem, error):
        state["busy"], state["latest"], state["installed"], state["studio"] = False, newest, installed, studio
        ours, has_studio = bool(installed and installed.ours), bool(installed and installed.studio)
        labels["You have"].configure(text=installed.describe if installed else "?")
        labels["Newest"].configure(text=(newest.title + (" (pre-release)" if newest.prerelease else "")) if newest else "?")
        notes = [problem] if problem else []
        if ours and newest and studio and compare(studio, newest.studio) > 0:
            notes.append("Animator Studio %s is out. Replay Plus for it isn't ready yet, so this one stays on %s." % (studio, newest.studio))
        if not ours and has_studio and studio and compare(studio, installed.studio) > 0:
            notes.append("Animator Studio %s is out (you have %s). Update Animator Studio gets it." % (studio, installed.studio))
        if not ours and has_studio and newest and compare(newest.studio, installed.studio) < 0:
            notes.append("Replay Plus is on Animator Studio %s for now, so installing it would go back from your %s." % (newest.studio, installed.studio))
        if newest and newest.updater and compare(newest.updater, VERSION) > 0:
            notes.append("There's a newer updater (%s) on the releases page." % newest.updater)
        notice.configure(text="\n".join(notes))
        up_to_date = bool(installed and newest and installed.ours and installed.sha256 == newest.dll.sha256)
        install.configure(text="Reinstall" if up_to_date else "Update" if ours else "Install Replay Plus")
        studio_button.configure(text="Update Animator Studio" if has_studio else "Get Animator Studio")
        lay_out((install, remove, restore, open_folder) if ours else
                (install, studio_button, restore, open_folder) if installed else (install, restore, open_folder))
        set_buttons(True)
        if not newest:
            install.state(["disabled"])
        if not updater.can_restore():
            restore.state(["disabled"])
        if not ours:
            remove.state(["disabled"])
        if not studio or (has_studio and compare(studio, installed.studio) <= 0):
            studio_button.state(["disabled"])
        if error is None and newest and installed and not state["said"]:
            state["said"] = True
            say("You're up to date." if up_to_date else "There's an update." if installed.ours else "Ready to install.")

    def work(action=None):
        if state["busy"]:
            return
        state["busy"] = True
        set_buttons(False)
        bar["value"] = 0

        def run():
            error = problem = installed = newest = studio = None
            try:
                if action:
                    action()
            except Exception as e:  # noqa: BLE001 (every failure is shown to the player)
                error = explain(e)
                say("Didn't work: " + error)
            try:
                installed = updater.inspect()
            except Exception as e:  # noqa: BLE001
                problem = "Couldn't read ReSkate.dll: %s" % e
            try:
                newest, studio = updater.latest(), updater.latest_studio()
            except Exception as e:  # noqa: BLE001
                problem = "Couldn't check GitHub for the newest Replay Plus: %s" % e
            events.put(("state", (installed, newest, studio, problem, error)))

        threading.Thread(target=run, daemon=True).start()

    def pick_folder():
        if sys.platform.startswith("linux"):
            try:  # Tk's own file dialog: show its "hidden files" switch (prefixes like ~/.wine are hidden)
                root.tk.call("namespace", "eval", "::tk::dialog::file::", "")
                root.tk.call("set", "::tk::dialog::file::showHiddenBtn", "1")
                root.tk.call("set", "::tk::dialog::file::showHiddenVar", "0")
            except tk.TclError:
                pass
        hint = " (Cmd+Shift+. shows hidden folders like Library)" if sys.platform == "darwin" else ""
        chosen = filedialog.askdirectory(title="The folder with ReSkateLauncher.exe in it" + hint,
                                         initialdir=str(updater.folder or Path.home()))
        if not chosen:
            if not is_reskate_folder(updater.folder):
                root.destroy()
            return
        if not is_reskate_folder(chosen):
            messagebox.showerror("Replay Plus Updater", "There's no ReSkateLauncher.exe in that folder. Pick the folder you start ReSkate from.")
            return pick_folder()
        updater.folder = Path(chosen)
        try:
            saved_path.parent.mkdir(parents=True, exist_ok=True)
            saved_path.write_text(chosen, encoding="utf-8")
        except OSError:
            pass
        labels["ReSkate folder"].configure(text=short(chosen))
        work()

    def open_it():
        if not is_reskate_folder(updater.folder):
            return
        opener = ["open"] if sys.platform == "darwin" else ["explorer"] if sys.platform == "win32" else ["xdg-open"]
        subprocess.Popen(opener + [str(updater.folder)])

    def ask_remove():
        installed = state["installed"]
        version = " " + installed.studio if installed and installed.studio else ""
        if messagebox.askyesno("Replay Plus Updater", "Take Replay Plus out and put the plain Animator Studio%s back in? "
                                                      "Your presets and settings stay." % version):
            work(updater.remove_replay_plus)

    def ask_studio():
        if messagebox.askyesno("Replay Plus Updater", "Get Animator Studio %s and the ReSkate launcher it goes with?" % (state["studio"] or "")):
            work(updater.update_studio)

    def ask_restore():
        if messagebox.askyesno("Replay Plus Updater", "Put back the ReSkate.dll and launcher you had before Replay Plus?"):
            work(updater.restore)

    change.configure(command=pick_folder)
    install.configure(command=lambda: work(lambda: updater.install(state["latest"])))
    remove.configure(command=ask_remove)
    studio_button.configure(command=ask_studio)
    restore.configure(command=ask_restore)
    open_folder.configure(command=open_it)

    def start():
        if is_reskate_folder(updater.folder):
            labels["ReSkate folder"].configure(text=short(updater.folder))
            work()
        else:
            say("Looking for your ReSkate folder...")
            threading.Thread(target=lambda: events.put(("found", find_reskate())), daemon=True).start()

    root.after(80, pump)
    root.after(150, start)
    root.mainloop()


def main():
    parser = argparse.ArgumentParser(description="Installs and updates Replay Plus for ReSkate.")
    parser.add_argument("--folder")
    parser.add_argument("--from", dest="from_folder")
    parser.add_argument("--prerelease", action="store_true")
    parser.add_argument("--install", action="store_true")
    parser.add_argument("--restore", action="store_true")
    parser.add_argument("--uninstall", action="store_true")
    parser.add_argument("--update-studio", action="store_true")
    parser.add_argument("--version", action="version", version="Replay Plus Updater " + VERSION)
    parser.add_argument("--check-ui", action="store_true", help=argparse.SUPPRESS)  # (the builds' own test: Tk works)
    args, _ = parser.parse_known_args()  # (macOS may add its own arguments to an app)

    if args.check_ui:
        import tkinter as tk
        root = tk.Tk()
        root.update()
        print("ui ok, Tk", tk.TkVersion)
        root.destroy()
        return 0

    saved_path = data_folder() / "folder.txt"
    folder = args.folder
    if not folder:
        try:
            saved = saved_path.read_text(encoding="utf-8").strip()
            folder = saved if is_reskate_folder(saved) else None
        except OSError:
            folder = None
    updater = Updater(folder, args.prerelease, args.from_folder)
    write_log("---- Replay Plus Updater %s (%s)%s%s%s, folder %s" % (
        VERSION, sys.platform, " --install" if args.install else " --restore" if args.restore else " --uninstall" if args.uninstall else " --update-studio" if args.update_studio else "",
        " --prerelease" if args.prerelease else "", " --from " + args.from_folder if args.from_folder else "",
        folder or "(not found yet)"))

    if args.install or args.restore or args.uninstall or args.update_studio:
        updater.log = lambda text: (write_log(text), print(text))
        try:
            if not is_reskate_folder(updater.folder):
                raise RuntimeError("no ReSkate folder (pass --folder)")
            if args.install:
                updater.install(updater.latest())
            elif args.uninstall:
                updater.remove_replay_plus()
            elif args.update_studio:
                updater.update_studio()
            else:
                updater.restore()
            return 0
        except Exception as e:  # noqa: BLE001
            write_log("Didn't work: " + explain(e))
            print("Didn't work: " + explain(e), file=sys.stderr)
            return 1
    run_window(updater, saved_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
