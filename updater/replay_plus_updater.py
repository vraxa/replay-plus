#!/usr/bin/env python3
"""Replay Plus Updater for macOS and Linux (skate. runs there through CrossOver, Whisky, Proton, Wine...).

Puts Replay Plus (a ReSkate.dll with the Animator Studio and Replay Plus built in) on the newest Animator Studio or on
the one you have, keeps either up to date, takes Replay Plus out again, or goes back to vanilla ReSkate, inside the
Wine bottle or prefix the game is in. Replay Plus builds come from its own GitHub releases, one per Animator Studio
version, the Studio from Fourth Studio's releases and ReSkate and its launcher from ReSkate's, and every download is
checked against its SHA-256 before anything in the game folder is touched. Same as the Windows updater, except that
one can also build Replay Plus for a Studio there's no ready-made build for (that needs Windows' build tools).

Command line: --folder <ReSkate folder>, --data <folder for the log and downloads>, --prerelease, --from <folder with
replay-plus-builds.json or replay-plus.json and the files they name>, and, without the window (exit 0 done, 1 failed):
--install, --update-plus, --update-studio, --uninstall, --vanilla, --restore, --status. The log is updater.log in the
data folder.
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

VERSION = "1.2"
REPO = "vraxa/replay-plus"
STUDIO_REPO = "FourthStudio/ReSkate-Animator-Studio"
RESKATE_REPO = "Dingo-Shenanigans/ReSkate"
API = "https://api.github.com/repos/"
AGENT = "ReplayPlusUpdater"
RECORD = "replay-plus-updater.json"
ORIGINAL = ".before-replay-plus"
BUILDS_MANIFEST = "replay-plus-builds.json"
OLD_MANIFEST = "replay-plus.json"

DATA = None


def data_folder():
    if DATA:
        return Path(DATA)
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


def get_json(url):
    return json.loads(get_text(url))


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


def latest_tag(repo):
    """A repository's newest release (not a pre-release) without GitHub's API, which allows only 60 calls an hour from
    one address: github.com's /releases/latest sends you on to that release's page."""
    request = urllib.request.Request("https://github.com/%s/releases/latest" % repo, headers={"User-Agent": AGENT}, method="HEAD")
    with urllib.request.urlopen(request, timeout=60, context=SSL) as response:
        m = re.search(r"/releases/tag/([^/?#]+)$", response.geturl())
    if not m:
        raise RuntimeError("%s has no releases" % repo)
    return urllib.parse.unquote(m.group(1))


def missing(error):
    return isinstance(error, urllib.error.HTTPError) and error.code == 404


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


def sentence(text):
    text = str(text) or "something went wrong."
    text = text[0].upper() + text[1:]
    return text if text.endswith(".") else text + "."


def cache(repo, tag):
    folder = data_folder() / "downloads" / repo.replace("/", "_") / re.sub(r"[^0-9A-Za-z._+\-]", "_", tag or "local")
    folder.mkdir(parents=True, exist_ok=True)
    return folder


class Pin:
    """A file a release pins: where it is and what it must hash to."""

    def __init__(self, d, tag=None):
        self.repo = d.get("repo") or REPO
        self.tag = d.get("tag") or tag or ""
        self.name = d.get("name") or ""
        self.sha256 = (d.get("sha256") or "").lower()
        self.size = int(d.get("size") or 0)
        if not self.name or len(self.sha256) != 64:
            raise ValueError("that Replay Plus release names a file without its SHA-256")

    @property
    def url(self):
        return "https://github.com/%s/releases/download/%s/%s" % (self.repo, urllib.parse.quote(self.tag), urllib.parse.quote(self.name))


def pin_of(d, tag):
    return Pin(d, tag) if d else None


class Build:
    """One Replay Plus build: a ReSkate.dll with Replay Plus on one Animator Studio version, and its launcher."""

    def __init__(self, replay_plus, studio, reskate, dll, launcher=None, launcher_json=None, studio_tag=None, studio_commit=None):
        self.replay_plus, self.studio, self.reskate = replay_plus, studio, reskate
        self.dll, self.launcher, self.launcher_json = dll, launcher, launcher_json
        self.studio_tag, self.studio_commit = studio_tag, studio_commit
        self.file = None

    @property
    def title(self):
        return "Replay Plus %s (Animator Studio %s, ReSkate %s)" % (self.replay_plus, self.studio, self.reskate)


class Release:
    """A Replay Plus release, from its replay-plus-builds.json (schema 2: a build for each Animator Studio version, and
    the patches the Windows updater builds others with) or, before that, its replay-plus.json (schema 1: one build)."""

    def __init__(self, text, tag, prerelease=False):
        d = json.loads(text)
        schema = d.get("schema")
        if schema not in (1, 2):
            raise ValueError("that Replay Plus release is newer than this updater, so get the newest updater from the releases page")
        self.tag, self.prerelease = tag, prerelease
        self.replay_plus, self.updater = d.get("replay_plus"), d.get("updater")
        self.builds, self.patches = [], []
        if schema == 1:
            self.builds.append(Build(self.replay_plus, d.get("studio"), d.get("reskate"), pin_of(d.get("dll"), tag),
                                     pin_of(d.get("launcher"), tag), pin_of(d.get("launcher_json"), tag)))
        else:
            for b in d.get("builds") or []:
                self.builds.append(Build(b.get("replay_plus") or self.replay_plus, b.get("studio"), b.get("reskate"),
                                         pin_of(b.get("dll"), tag), pin_of(b.get("launcher"), tag),
                                         pin_of(b.get("launcher_json"), tag), b.get("studio_tag"), b.get("studio_commit")))
            self.patches = [Pin(p, tag) for p in d.get("patches") or []]
        self.builds = [b for b in self.builds if b.dll and b.studio and b.replay_plus]
        if not self.builds and not self.patches:
            raise ValueError("that Replay Plus release has no ReSkate.dll in it")
        if not self.replay_plus and self.newest:
            self.replay_plus = self.newest.replay_plus

    def for_studio(self, studio):
        """The newest Replay Plus made for that Studio version, or None."""
        best = None
        for b in self.builds:
            if studio and compare(b.studio, studio) == 0 and (best is None or compare(b.replay_plus, best.replay_plus) > 0):
                best = b
        return best

    @property
    def newest(self):
        best = None
        for b in self.builds:
            c = 1 if best is None else compare(b.studio, best.studio)
            if c > 0 or (c == 0 and compare(b.replay_plus, best.replay_plus) > 0):
                best = b
        return best

    @property
    def studios(self):
        found = []
        for b in self.builds:
            if b.studio not in found:
                found.append(b.studio)
        return ", ".join(sorted(found, key=numbers, reverse=True))


# The version a DLL was built as, a C string in it: "1.1+studio-2.1.5.reskate-20261116" for Replay Plus,
# "2.1.5+reskate.20261116" for the Animator Studio; vanilla ReSkate has only its own version ("20261116").
OURS = re.compile(rb"(?<![0-9.])(\d+(?:\.\d+)+)\+studio-(\d+(?:\.\d+)+)\.reskate-([0-9A-Za-z.\-]+?)\x00")
STUDIO = re.compile(rb"(?<![0-9.])(\d+\.\d+\.\d+)\+reskate\.([0-9A-Za-z.\-]+?)\x00")
RESKATE = re.compile(rb"\x00(20\d{6})\x00")


class Installed:
    def __init__(self, dll):
        self.has_dll = self.ours = self.vanilla = False
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
        versions = sorted({m.group(1).decode() for m in RESKATE.finditer(data)})
        self.reskate = versions[0] if len(versions) == 1 else None
        self.describe = self.plain()

    @property
    def has_studio(self):
        return self.studio is not None

    def plain(self):
        if self.vanilla:
            return "ReSkate %s (vanilla)" % self.reskate
        if self.reskate:
            return "ReSkate %s, without the Animator Studio or Replay Plus" % self.reskate
        return "a ReSkate.dll without the Animator Studio or Replay Plus"


class ReSkateRelease:
    """ReSkate's release of a version: its launcher.json pins the ReSkate.dll and the launcher in it."""

    def __init__(self, tag, json_file, version):
        self.tag, self.json_file = tag, json_file
        d = json.loads(json_file.read_text(encoding="utf-8-sig"))
        self.runtime = self.file_pin(d.get("runtime"), "ReSkate.dll")
        self.launcher = self.file_pin(d.get("launcher"), "ReSkateLauncher.exe")
        self.version = (d.get("runtime") or {}).get("version") or version
        self.game_build = (d.get("game") or {}).get("build_id")

    def file_pin(self, d, name):
        if not d or len(d.get("sha256") or "") != 64:
            return None
        url = d.get("url") or ""
        return Pin({"repo": RESKATE_REPO, "tag": self.tag, "name": url[6:] if url.startswith("asset:") else name,
                    "sha256": d["sha256"], "size": d.get("size") or 0})


class Choice:
    """What there is for a Studio version: a ready-made build, or nothing (and why)."""

    def __init__(self, studio, build=None, why=None):
        self.studio, self.build, self.why = studio, build, why

    @property
    def possible(self):
        return self.build is not None


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


def running(folder=None):
    """skate. or the ReSkate launcher, if either runs (on Windows: from that folder, or from where Windows won't say;
    under Wine their command lines name the .exe)."""
    try:
        if sys.platform == "win32":
            out = subprocess.run(["powershell", "-NoProfile", "-Command",
                                  "Get-CimInstance Win32_Process -Filter \"Name='Skate.exe' OR Name='ReSkateLauncher.exe'\" | "
                                  "ForEach-Object { $_.Name + '|' + $_.ExecutablePath }"],
                                 capture_output=True, text=True, creationflags=0x08000000).stdout
            for line in out.splitlines():
                name, _, exe = line.strip().partition("|")
                if not name:
                    continue
                here = not exe or not folder or os.path.normcase(os.path.dirname(os.path.abspath(exe))) == os.path.normcase(os.path.abspath(str(folder)))
                if here:
                    return "skate." if name.lower() == "skate.exe" else "the ReSkate launcher"
            return None
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
            builds = self.from_folder / BUILDS_MANIFEST
            return Release((builds if builds.exists() else self.from_folder / OLD_MANIFEST).read_text(encoding="utf-8-sig"), "local")
        if not self.prerelease:  # the newest release without GitHub's API: its manifest, the new kind first
            newest = latest_tag(REPO)
            for name in (BUILDS_MANIFEST, OLD_MANIFEST):
                try:
                    return Release(get_text("https://github.com/%s/releases/download/%s/%s" % (REPO, urllib.parse.quote(newest), name)), newest)
                except urllib.error.HTTPError as e:
                    if not missing(e):
                        raise
            raise ValueError("the newest Replay Plus release (%s) can't be installed by this updater" % newest)
        release = next((r for r in get_json(API + REPO + "/releases?per_page=10")
                        if not r.get("draft") and self.manifest_of(r)), None)
        if not release:
            raise ValueError("no Replay Plus release the updater can install")
        tag = release["tag_name"]
        text = get_text("https://github.com/%s/releases/download/%s/%s" % (REPO, urllib.parse.quote(tag), self.manifest_of(release)))
        return Release(text, tag, bool(release.get("prerelease")))

    @staticmethod
    def manifest_of(release):
        names = {a.get("name") for a in release.get("assets") or []}
        return BUILDS_MANIFEST if BUILDS_MANIFEST in names else OLD_MANIFEST if OLD_MANIFEST in names else None

    @staticmethod
    def latest_studio():
        try:
            return latest_tag(STUDIO_REPO)
        except Exception:
            return None

    def latest_reskate(self):
        return self.reskate_at(latest_tag(RESKATE_REPO), exact=True)

    def reskate_at(self, version, exact=False):
        """ReSkate's release of a version: its tag is the version itself or "v" + it ("20261116", "v2.1.4-4")."""
        for tag in ([version] if exact else [version, "v" + version]):
            path = cache(RESKATE_REPO, tag) / "launcher.json"
            try:
                download("https://github.com/%s/releases/download/%s/launcher.json" % (RESKATE_REPO, urllib.parse.quote(tag)),
                         path, 0, lambda done, total: None)
            except urllib.error.HTTPError as e:
                if missing(e):
                    continue
                raise
            return ReSkateRelease(tag, path, version)
        return None

    # ---- What is installed ------------------------------------------------------------------------------------
    def inspect(self):
        result = Installed(self.folder / "ReSkate.dll")
        if result.has_dll and not result.ours and not result.has_studio:
            try:  # vanilla ReSkate: the launcher.json beside it pins this very file
                runtime = json.loads((self.folder / "launcher.json").read_text(encoding="utf-8-sig")).get("runtime") or {}
                if (runtime.get("sha256") or "").lower() == result.sha256:
                    result.vanilla = True
                    result.reskate = runtime.get("version") or result.reskate
            except (OSError, ValueError):
                pass
            result.describe = result.plain()
        return result

    def check_closed(self):
        busy = running(self.folder)
        if busy:
            raise RuntimeError("close %s first" % busy)

    def choose_for(self, release, studio):
        if release is None or not studio:
            return Choice(studio, why="the updater couldn't check what's out there")
        build = release.for_studio(studio)
        if build:
            return Choice(studio, build)
        return Choice(studio, why="Replay Plus isn't ready for Animator Studio %s yet" % studio)

    def report(self):
        """--status: what's here, what's out, and what each button would do now."""
        installed = self.inspect()
        self.log("You have: " + installed.describe + (" (SHA-256 %s...)" % installed.sha256[:12] if installed.has_dll else ""))
        release = reskate = None
        try:
            release = self.latest()
        except Exception as e:  # noqa: BLE001
            self.log("Replay Plus: couldn't check (%s)" % e)
        studio = self.latest_studio()
        try:
            reskate = self.latest_reskate()
        except Exception:  # noqa: BLE001
            pass
        if release:
            self.log("Replay Plus %s (%s): ready-made for Animator Studio %s" % (release.replay_plus, release.tag, release.studios))
        self.log("Newest Animator Studio: %s, newest ReSkate: %s" % (studio or "?", reskate.version if reskate else "?"))

        def says(choice, fallback=None):
            if choice.build:
                return "ready-made %s (%s)" % (choice.build.title, choice.build.dll.name)
            if fallback and fallback.newest:
                return "%s, so %s" % (choice.why, fallback.newest.title)
            return "can't: " + choice.why

        latest = self.choose_for(release, studio)
        self.log("1 Install Replay Plus + Animator Studio: " + says(latest, release))
        if installed.has_studio:
            self.log("3 %s Replay Plus for Studio %s: %s" % ("Update" if installed.ours else "Add", installed.studio,
                                                             says(self.choose_for(release, installed.studio))))
        if installed.ours:
            self.log("4 Update Animator Studio (keeping Replay Plus): " +
                     (says(latest) if studio and compare(studio, installed.studio) > 0 else "already the newest Studio"))
        else:
            self.log("4 %s Animator Studio: %s" % ("Update" if installed.has_studio else "Get", "?" if not studio else
                     "already the newest Studio" if installed.has_studio and compare(studio, installed.studio) <= 0 else
                     "plain Animator Studio " + studio))
        vanilla_now = bool(reskate and installed.vanilla and reskate.runtime and installed.sha256 == reskate.runtime.sha256)
        self.log("5 Vanilla ReSkate: " + ("?" if not reskate else "already vanilla ReSkate " + reskate.version if vanilla_now
                                          else "ReSkate " + reskate.version))
        self.log("Put back what I had before: " + ("yes" if self.can_restore() else "nothing kept"))

    # ---- The five things it does -----------------------------------------------------------------------------
    def install_with_studio(self, release):
        """1. The newest Animator Studio with Replay Plus on it, and the ReSkate launcher it needs."""
        if release is None:
            raise RuntimeError("couldn't check GitHub for Replay Plus")
        studio = self.latest_studio() or (release.newest.studio if release.newest else None)
        choice = self.choose_for(release, studio)
        if not choice.possible:
            if not release.newest:
                raise RuntimeError(choice.why)
            self.log("(%s So this is Replay Plus on Animator Studio %s, the newest one it's ready for.)" % (sentence(choice.why), release.newest.studio))
            choice = Choice(release.newest.studio, release.newest)
        self.install(self.get(choice))

    def update_replay_plus(self, release):
        """3. The newest Replay Plus for the Animator Studio version that's in now; the Studio stays as it is."""
        installed = self.inspect()
        if not installed.has_studio:
            raise RuntimeError("there's no Animator Studio here to put Replay Plus on")
        choice = self.choose_for(release, installed.studio)
        if not choice.possible:
            raise RuntimeError(choice.why + ", so nothing was changed")
        self.install(self.get(choice))

    def update_studio(self, release):
        """4. The newest Animator Studio, with Replay Plus on it when Replay Plus is in now."""
        installed = self.inspect()
        studio = self.latest_studio()
        if not studio:
            raise RuntimeError("couldn't check GitHub for the newest Animator Studio")
        if installed.has_studio and compare(studio, installed.studio) <= 0:
            self.log("You already have the newest Animator Studio (%s), so there's nothing to do." % installed.studio)
            return
        if not installed.ours:
            return self.update_plain_studio()
        choice = self.choose_for(release, studio)
        if not choice.possible:
            raise RuntimeError("%s, so you stay on %s for now. Nothing was changed" % (choice.why, installed.studio))
        self.install(self.get(choice))

    def get(self, choice):
        build = choice.build
        self.log("Getting " + build.title)
        build.file = self.fetch(build.dll, cache(build.dll.repo, build.dll.tag), ours=True)
        return build

    def install(self, build):
        """A build's DLL into the game folder, with the ReSkate launcher it's built for; "updates" off, so the launcher
        doesn't put its own ReSkate.dll back."""
        check = Installed(build.file)
        if not check.ours or compare(check.studio, build.studio) != 0:
            raise RuntimeError("that file isn't Replay Plus on Animator Studio %s, so nothing was changed" % build.studio)
        launcher_path, json_path = self.folder / "ReSkateLauncher.exe", self.folder / "launcher.json"
        new_launcher = new_json = None
        if build.launcher:
            if not self.matches(launcher_path, build.launcher):
                self.log("Getting the ReSkate %s launcher from ReSkate's own release" % build.launcher.tag)
                new_launcher = self.fetch(build.launcher, cache(build.launcher.repo, build.launcher.tag))
            if build.launcher_json and not self.matches(json_path, build.launcher_json):
                new_json = self.fetch(build.launcher_json, cache(build.launcher_json.repo, build.launcher_json.tag))
        else:
            new_launcher, new_json = self.launcher_for(build.reskate)
        self.check_closed()
        record = self.begin()
        self.put(build.file, self.folder / "ReSkate.dll")
        if new_launcher:
            self.put(new_launcher, launcher_path)
        if new_json:
            self.put(new_json, json_path)
        self.updates(record, False)
        self.stamp(record, build.replay_plus, build.studio, build.reskate, build.dll.tag, build.title)
        self.log("Done: %s is in. Start ReSkate like you normally do." % build.title)

    def launcher_for(self, reskate):
        """The launcher (and its launcher.json) of ReSkate's release of that version, when the one here isn't it."""
        release = self.reskate_at(reskate) if reskate else None
        if not release or not release.launcher:
            self.log("(couldn't find the ReSkate %s launcher, so the launcher stays as it is)" % reskate)
            return None, None
        new_launcher = new_json = None
        if not self.matches(self.folder / "ReSkateLauncher.exe", release.launcher):
            self.log("Getting the ReSkate %s launcher from ReSkate's own release" % reskate)
            new_launcher = self.fetch(release.launcher, cache(RESKATE_REPO, release.tag))
        here = self.folder / "launcher.json"
        if not here.exists() or sha256_of(here) != sha256_of(release.json_file):
            new_json = release.json_file
        return new_launcher, new_json

    def remove_replay_plus(self):
        """2. Replay Plus out, the Animator Studio kept: the Studio's own ReSkate.dll of the version this Replay Plus was
        built on (so it fits the launcher), from its own release and checked against its SHA-256. The launcher and
        what was kept from before Replay Plus stay as they are."""
        self.check_closed()
        installed = self.inspect()
        if not installed.ours:
            raise RuntimeError("Replay Plus isn't installed here")
        release = None
        for tag in (installed.studio, "v" + installed.studio):
            try:
                release = get_json(API + STUDIO_REPO + "/releases/tags/" + urllib.parse.quote(tag))
                break
            except urllib.error.HTTPError as e:
                if not missing(e):
                    raise
        studio_dll = self.fetch_studio(release or get_json(API + STUDIO_REPO + "/releases/latest"))
        check = Installed(studio_dll)
        self.check_closed()
        record = self.begin()
        self.put(studio_dll, self.folder / "ReSkate.dll")
        self.stamp(record, None, check.studio, check.reskate, check.studio, "Animator Studio " + check.studio)
        self.log("Done: Replay Plus is out and Animator Studio %s is in. Your presets and settings are still there if you want it back." % check.studio)

    def fetch_studio(self, release):
        """An Animator Studio release's ReSkate.dll, downloaded and checked (GitHub's SHA-256 of it, else its .sha256)."""
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
        pin = Pin({"repo": STUDIO_REPO, "tag": tag, "name": "ReSkate.dll", "sha256": sha, "size": dll_asset.get("size") or 0})
        self.log("Getting Animator Studio %s from its own release" % tag)
        path = self.fetch(pin, cache(STUDIO_REPO, tag))
        check = Installed(path)
        if check.ours or not check.studio:
            raise RuntimeError("that download isn't the Animator Studio's ReSkate.dll, so nothing was changed")
        return path

    def update_plain_studio(self):
        """The newest Animator Studio on its own (no Replay Plus here): its ReSkate.dll from its own release, checked,
        and the ReSkate launcher it is built on (its version string names it) from ReSkate's release of that version,
        checked against that release's launcher.json. "updates" stays off, as the Studio needs."""
        self.check_closed()
        studio_dll = self.fetch_studio(get_json(API + STUDIO_REPO + "/releases/latest"))
        check = Installed(studio_dll)
        new_launcher, new_json = self.launcher_for(check.reskate)
        self.check_closed()
        record = self.begin()
        self.put(studio_dll, self.folder / "ReSkate.dll")
        if new_launcher:
            self.put(new_launcher, self.folder / "ReSkateLauncher.exe")
        if new_json:
            self.put(new_json, self.folder / "launcher.json")
        self.updates(record, False)
        self.stamp(record, None, check.studio, check.reskate, check.studio, "Animator Studio " + check.studio)
        self.log("Done: Animator Studio %s is in%s. Start ReSkate like you normally do." % (
            check.studio, ", with the ReSkate %s launcher" % check.reskate if new_launcher else ""))

    def vanilla(self):
        """5. Vanilla: ReSkate's own newest ReSkate.dll and launcher (checked against its launcher.json), no Studio and
        no Replay Plus, and the launcher's own updates back on, since keeping vanilla ReSkate up to date is what they do."""
        self.check_closed()
        release = self.latest_reskate()
        if not release or not release.runtime or not release.launcher:
            raise RuntimeError("couldn't read ReSkate's newest release, so nothing was changed")
        folder = cache(RESKATE_REPO, release.tag)
        self.log("Getting ReSkate %s from ReSkate's own release" % release.version)
        dll_path, launcher_path, json_path = (self.folder / "ReSkate.dll", self.folder / "ReSkateLauncher.exe",
                                              self.folder / "launcher.json")
        dll = None if self.matches(dll_path, release.runtime) else self.fetch(release.runtime, folder)
        launcher = None if self.matches(launcher_path, release.launcher) else self.fetch(release.launcher, folder)
        if dll:
            check = Installed(dll)
            if check.ours or check.has_studio:
                raise RuntimeError("that download isn't vanilla ReSkate, so nothing was changed")
        self.check_closed()
        record = self.begin()
        if dll:
            self.put(dll, dll_path)
        if launcher:
            self.put(launcher, launcher_path)
        if not json_path.exists() or sha256_of(json_path) != sha256_of(release.json_file):
            self.put(release.json_file, json_path)
        self.updates(record, True)
        self.stamp(record, None, None, release.version, release.tag, "ReSkate " + release.version)
        self.log("Done: vanilla ReSkate %s is in, and its launcher keeps it up to date again. Your Replay Plus presets and "
                 "settings are still there if you want it back." % release.version)

    def can_restore(self):
        record = self.read_record() or {}
        return bool(record.get("originals")) or (self.folder / "ReSkate.dll.studio").exists()

    def restore(self):
        """What there was before the updater's first change: the kept files back, "updates" as it was then."""
        self.check_closed()
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
        for name in record.get("created") or []:
            path = self.folder / name
            if name in originals or not path.is_file() or "/" in name or "\\" in name:
                continue
            shutil.copyfile(path, path.with_name(name + ".previous"))
            path.unlink()
            self.log("Took out %s (it wasn't there before; it's kept as %s.previous)" % (name, name))
        # As it was. When it wasn't there at all (the launcher counts that as on): on for vanilla ReSkate, off for
        # anything else, which the launcher would swap for its own release.
        before = record.get("updates_before")
        self.set_updates(self.folder / "ReSkateLauncher.settings.json", before if isinstance(before, bool) else self.inspect().vanilla)
        (self.folder / RECORD).unlink(missing_ok=True)
        self.log("Put back what you had before Replay Plus.")

    # ---- Files ----------------------------------------------------------------------------------------------------
    def fetch(self, pin, folder, ours=False):
        path = folder / pin.name
        if ours and self.from_folder:
            path = self.from_folder / pin.name
            self.log("Taking %s from %s" % (pin.name, self.from_folder))
        elif not self.matches(path, pin):
            download(pin.url, path, pin.size, lambda done, total: self.progress(done, total, pin.name))
        if not self.matches(path, pin):
            if folder in path.parents:
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

    def begin(self):
        """The record of what the updater did here; the first time, what was there before is kept first."""
        return self.read_record() or self.keep_originals()

    def updates(self, record, value):
        before = self.set_updates(self.folder / "ReSkateLauncher.settings.json", value)
        record.setdefault("updates_before", before)

    def keep_originals(self):
        dll = self.folder / "ReSkate.dll"
        originals = {}
        if dll.exists():
            if Installed(dll).ours:
                for name in ("ReSkate.dll.studio", "ReSkate.dll.original"):
                    kept = self.folder / name
                    if kept.exists() and not Installed(kept).ours:
                        originals["ReSkate.dll"] = name
                        break
            if "ReSkate.dll" not in originals:  # (else, or a Replay Plus with nothing from before it: the one that's here)
                shutil.copyfile(dll, dll.with_name(dll.name + ORIGINAL))
                originals["ReSkate.dll"] = dll.name + ORIGINAL
        for name in ("ReSkateLauncher.exe", "launcher.json"):
            path = self.folder / name
            if path.exists():
                shutil.copyfile(path, path.with_name(name + ORIGINAL))
                originals[name] = name + ORIGINAL
        self.log("Kept what you had before: " + (", ".join(originals.values()) or "nothing to keep"))
        record = {"originals": originals}
        created = [name for name in ("ReSkate.dll", "launcher.json") if not (self.folder / name).exists()]
        if created:  # what wasn't there at all comes out again on Put back
            record["created"] = created
        return record

    def stamp(self, record, replay_plus, studio, reskate, tag, now):
        """What's in now, written down with when."""
        record.update(installed=replay_plus, studio=studio, reskate=reskate, tag=tag, now=now,
                      when=datetime.now().strftime("%Y-%m-%d %H:%M"))
        self.write_record(record)

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
        self.log('Set "updates" to %s in ReSkateLauncher.settings.json%s' % (
            word, " (so the launcher keeps ReSkate up to date)" if value else " (so the launcher doesn't swap the dll back)"))
        return before


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
    root.minsize(640, 420)
    root.geometry("760x470")
    events = queue.Queue()

    frame = ttk.Frame(root, padding=14)
    frame.pack(fill="both", expand=True)
    frame.columnconfigure(1, weight=1)
    frame.rowconfigure(7, weight=1)
    bold = tkfont.nametofont("TkDefaultFont").copy()
    bold.configure(weight="bold")
    labels = {}
    for row, name in enumerate(("ReSkate folder", "You have", "Newest")):
        ttk.Label(frame, text=name, foreground="#777").grid(row=row, column=0, sticky="w", padx=(0, 12), pady=4)
        labels[name] = ttk.Label(frame, text="", font=bold)
        labels[name].grid(row=row, column=1, sticky="w", pady=4)
    change = ttk.Button(frame, text="Change...")
    change.grid(row=0, column=2, sticky="e")
    notice = ttk.Label(frame, text="", foreground="#a05a00", wraplength=700, justify="left")
    notice.grid(row=3, column=0, columnspan=3, sticky="w", pady=(4, 4))
    rows = (ttk.Frame(frame), ttk.Frame(frame))
    rows[0].grid(row=4, column=0, columnspan=3, sticky="w", pady=(4, 4))
    rows[1].grid(row=5, column=0, columnspan=3, sticky="w", pady=(0, 8))
    install = ttk.Button(rows[0], text="Install Replay Plus + Animator Studio")
    plus = ttk.Button(rows[0], text="Update Replay Plus")
    studio_button = ttk.Button(rows[0], text="Update Animator Studio")
    remove = ttk.Button(rows[1], text="Remove Replay Plus")
    vanilla = ttk.Button(rows[1], text="Vanilla ReSkate")
    restore = ttk.Button(rows[1], text="Put back what I had before")
    open_folder = ttk.Button(rows[1], text="Open the folder")
    every = (install, plus, studio_button, remove, vanilla, restore, open_folder)

    def lay_out(shown):
        for b in every:
            b.pack_forget()
        for b in every:
            if b in shown:
                b.pack(side="left", padx=(0, 8))

    lay_out((install, restore, open_folder))
    progress_row = ttk.Frame(frame)
    progress_row.grid(row=6, column=0, columnspan=3, sticky="ew", pady=(0, 8))
    progress_row.columnconfigure(0, weight=1)
    bar = ttk.Progressbar(progress_row, maximum=1000)
    bar.grid(row=0, column=0, sticky="ew")
    status = ttk.Label(progress_row, text="", foreground="#777", width=34)
    status.grid(row=0, column=1, sticky="w", padx=(8, 0))
    log = tk.Text(frame, height=10, wrap="word", state="disabled", relief="solid", borderwidth=1,
                  font=tkfont.nametofont("TkDefaultFont"), padx=6, pady=4)
    log.grid(row=7, column=0, columnspan=3, sticky="nsew")
    state = {"busy": False, "said": False, "installed": None, "release": None, "studio": None, "reskate": None}

    def short(path, most=70):
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
                    status.configure(text="%s %.1f / %.1f MB" % (name, done / 1048576, total / 1048576))
                elif kind == "state":
                    show(value)
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
        for b in every + (change,):
            if b is not open_folder:
                b.state(["!disabled"] if on else ["disabled"])

    def show(s):
        installed, release, studio, reskate = s["installed"], s["release"], s["studio"], s["reskate"]
        state.update(busy=False, installed=installed, release=release, studio=studio, reskate=reskate)
        ours, has_studio = bool(installed and installed.ours), bool(installed and installed.has_studio)
        labels["You have"].configure(text=installed.describe if installed else "?")
        newest = []
        if release:
            newest.append("Replay Plus " + release.replay_plus + (" (pre-release)" if release.prerelease else ""))
        if studio:
            newest.append("Animator Studio " + studio)
        if reskate:
            newest.append("ReSkate " + reskate.version)
        labels["Newest"].configure(text="  /  ".join(newest) or "?")

        for_latest = updater.choose_for(release, studio)
        for_mine = updater.choose_for(release, installed.studio) if has_studio else None
        studio_newer = bool(studio and has_studio and compare(studio, installed.studio) > 0)
        vanilla_now = bool(installed and installed.vanilla and reskate and reskate.runtime and installed.sha256 == reskate.runtime.sha256)
        notes = [s["problem"]] if s["problem"] else []
        if ours and studio_newer:
            notes.append("Animator Studio %s is out. Update Animator Studio moves you to it and keeps Replay Plus." % studio
                         if for_latest.possible else
                         "Animator Studio %s is out, but %s, so you stay on %s for now." % (studio, for_latest.why, installed.studio))
        if not ours and has_studio and studio_newer:
            notes.append("Animator Studio %s is out (you have %s)." % (studio, installed.studio))
        if not ours and release and studio and not for_latest.possible and release.newest:
            notes.append("%s Install puts it on Animator Studio %s, the newest one it's ready for." % (sentence(for_latest.why), release.newest.studio))
        if has_studio and for_mine and not for_mine.possible and (ours or not studio or compare(studio, installed.studio) != 0):
            notes.append(sentence(for_mine.why) + ("" if ours else " (Install gets you the newest Studio with Replay Plus instead.)"))
        if release and release.updater and compare(release.updater, VERSION) > 0:
            notes.append("There's a newer updater (%s) on the releases page." % release.updater)
        notice.configure(text="\n".join(notes))

        offered = for_mine.build.replay_plus if for_mine and for_mine.build else None
        plus_newer = bool(ours and offered and compare(offered, installed.replay_plus) > 0)
        plus_older = bool(ours and offered and compare(offered, installed.replay_plus) < 0)
        plus.configure(text=("Update Replay Plus" if plus_newer else "Reinstall Replay Plus") if ours
                       else "Add Replay Plus to Studio %s" % (installed.studio if installed else ""))
        studio_button.configure(text="Update Animator Studio" if has_studio else "Get Animator Studio")
        shown = []
        if not ours:
            shown.append(install)
        if ours or (has_studio and (not studio or compare(studio, installed.studio) != 0)):
            shown.append(plus)
        if installed:
            shown.append(studio_button)
        if ours:
            shown.append(remove)
        if installed:
            shown.append(vanilla)
        lay_out(tuple(shown) + (restore, open_folder))
        set_buttons(True)
        if not (release and (for_latest.possible or release.newest)):
            install.state(["disabled"])
        if not (for_mine and for_mine.possible and not plus_older):
            plus.state(["disabled"])
        if not (studio and (not has_studio or studio_newer) and (not ours or for_latest.possible)):
            studio_button.state(["disabled"])
        if not ours:
            remove.state(["disabled"])
        if not installed or vanilla_now:
            vanilla.state(["disabled"])
        if not updater.can_restore():
            restore.state(["disabled"])
        status.configure(text="")
        if s["error"] is not None or not release:
            bar["value"] = 0
        if s["error"] is None and release and installed and not state["said"]:
            state["said"] = True
            say("There's an update." if plus_newer or (ours and studio_newer and for_latest.possible) else "You're up to date." if ours else "Ready.")

    def work(action=None):
        if state["busy"]:
            return
        state["busy"] = True
        set_buttons(False)
        bar["value"] = 0
        status.configure(text="Checking" if action is None else "")

        def run():
            s = {"error": None, "problem": None, "installed": None, "release": None, "studio": None, "reskate": None}
            try:
                if action:
                    action()
            except Exception as e:  # noqa: BLE001 (every failure is shown to the player)
                s["error"] = explain(e)
                say("Didn't work: " + s["error"])
            try:
                s["installed"] = updater.inspect()
            except Exception as e:  # noqa: BLE001
                s["problem"] = "Couldn't read ReSkate.dll: %s" % e
            try:
                s["release"] = updater.latest()
            except Exception as e:  # noqa: BLE001
                s["problem"] = "Couldn't check GitHub for the newest Replay Plus: %s" % e
            s["studio"] = updater.latest_studio()
            try:
                s["reskate"] = updater.latest_reskate()
            except Exception:  # noqa: BLE001
                pass
            events.put(("state", s))

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

    def ask(question):
        return messagebox.askyesno("Replay Plus Updater", question)

    def ask_install():
        release, studio = state["release"], state["studio"]
        choice = updater.choose_for(release, studio)
        target = choice.studio if choice.possible else (release.newest.studio if release and release.newest else studio)
        if ask("Get Animator Studio %s with Replay Plus on it, and the ReSkate launcher it goes with?" % target):
            work(lambda: updater.install_with_studio(release))

    def ask_remove():
        installed = state["installed"]
        if ask("Take Replay Plus out and put the plain Animator Studio %s back in? Your presets and settings stay." % installed.studio):
            work(updater.remove_replay_plus)

    def ask_plus():
        installed, release = state["installed"], state["release"]
        question = ("Get the newest Replay Plus for your Animator Studio %s?" % installed.studio if installed.ours else
                    "Put Replay Plus on your Animator Studio %s? The Studio stays as it is." % installed.studio)
        if ask(question):
            work(lambda: updater.update_replay_plus(release))

    def ask_studio():
        installed, release, studio = state["installed"], state["release"], state["studio"]
        question = ("Move to Animator Studio %s and keep Replay Plus on it?" % studio if installed.ours else
                    "Get Animator Studio %s and the ReSkate launcher it goes with?%s" % (studio or "", "" if installed.has_studio else " (without Replay Plus)"))
        if ask(question):
            work(lambda: updater.update_studio(release))

    def ask_vanilla():
        reskate = state["reskate"]
        if ask("Go back to vanilla ReSkate%s? Replay Plus and the Animator Studio come out, ReSkate's own ReSkate.dll and "
               "launcher go in, and the launcher's own updates go back on. Your presets and settings stay, and Put back what "
               "I had before still works." % (" " + reskate.version if reskate else "")):
            work(updater.vanilla)

    def ask_restore():
        if ask("Put back the ReSkate.dll and launcher you had before the updater changed anything?"):
            work(updater.restore)

    change.configure(command=pick_folder)
    install.configure(command=ask_install)
    plus.configure(command=ask_plus)
    studio_button.configure(command=ask_studio)
    remove.configure(command=ask_remove)
    vanilla.configure(command=ask_vanilla)
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


ACTIONS = ("install", "update_plus", "update_studio", "uninstall", "vanilla", "restore", "status")


def main():
    global DATA
    parser = argparse.ArgumentParser(description="Installs and updates Replay Plus for ReSkate.")
    parser.add_argument("--folder")
    parser.add_argument("--data")
    parser.add_argument("--from", dest="from_folder")
    parser.add_argument("--prerelease", action="store_true")
    for name in ACTIONS:
        parser.add_argument("--" + name.replace("_", "-"), action="store_true")
    parser.add_argument("--version", action="version", version="Replay Plus Updater " + VERSION)
    parser.add_argument("--check-ui", action="store_true", help=argparse.SUPPRESS)  # (the builds' own test: Tk works)
    args, _ = parser.parse_known_args()  # (macOS may add its own arguments to an app)
    DATA = args.data

    if args.check_ui:
        import tkinter as tk
        root = tk.Tk()
        root.update()
        print("ui ok, Tk", tk.TkVersion)
        root.destroy()
        return 0

    action = next((name for name in ACTIONS if getattr(args, name)), None)
    saved_path = data_folder() / "folder.txt"
    folder = args.folder
    if not folder:
        try:
            saved = saved_path.read_text(encoding="utf-8").strip()
            folder = saved if is_reskate_folder(saved) else None
        except OSError:
            folder = None
    updater = Updater(folder, args.prerelease, args.from_folder)
    write_log("---- Replay Plus Updater %s (%s) %s, folder %s" % (VERSION, sys.platform, " ".join(sys.argv[1:]), folder or "(not found yet)"))

    if action:
        updater.log = lambda text: (write_log(text), print(text))
        try:
            if not is_reskate_folder(updater.folder):
                raise RuntimeError("no ReSkate folder (pass --folder)")
            if action == "status":
                updater.report()
            elif action == "install":
                updater.install_with_studio(updater.latest())
            elif action == "update_plus":
                updater.update_replay_plus(updater.latest())
            elif action == "update_studio":
                updater.update_studio(updater.latest() if updater.inspect().ours else None)
            elif action == "uninstall":
                updater.remove_replay_plus()
            elif action == "vanilla":
                updater.vanilla()
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
