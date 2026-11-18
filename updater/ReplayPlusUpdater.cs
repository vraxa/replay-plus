// Replay Plus Updater: puts Replay Plus (a ReSkate.dll with the Animator Studio and Replay Plus built in) on the newest
// Animator Studio or on the one you have, keeps either up to date, takes Replay Plus out again, or goes back to vanilla
// ReSkate. Replay Plus builds come from its own GitHub releases, one for each Animator Studio version; when there isn't
// one for the Studio you need, it can build it here from the Studio's source and the Replay Plus patches (with git,
// CMake and Visual Studio Build Tools 2022). The Studio comes from Fourth Studio's releases and ReSkate and its launcher
// from ReSkate's, and every download is checked against its SHA-256 before anything in the game folder is touched.
//
// Command line (for testing, and for the release workflow's builds):
//   --folder <dir>        the ReSkate folder (the one with ReSkateLauncher.exe)
//   --data <dir>          where the log, downloads and builds go (default %LOCALAPPDATA%\ReSkate\Replay Plus)
//   --from <dir>          the release's files (replay-plus-builds.json or replay-plus.json, the DLLs, the patches) from
//                         a folder instead of GitHub
//   --prerelease          pre-releases count too
//   --install | --update-plus | --update-studio | --uninstall | --vanilla | --restore
//                         do that without the window and exit: 0 done, 1 failed, 2 the patches don't go onto that Studio,
//                         3 the build or its tests failed (--status: just say what's here and what each would do)
//   --build-here          build Replay Plus here even when there's a ready-made one; --no-build: never build here
//   --jobs <n>            how many files a build compiles at once (default: half the processors)
//   --build-only <studio tag> --out <dir> [--patches <dir>]
//                         only build Replay Plus for that Studio (with the release's patches, or the ones in <dir>, which
//                         has patches.json) and put the DLL, its .sha256 and its build entry in <out>
// Everything it does goes to updater.log in the data folder.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Net;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;

[assembly: AssemblyTitle("Replay Plus Updater")]
[assembly: AssemblyProduct("Replay Plus")]
[assembly: AssemblyDescription("Installs and updates Replay Plus for ReSkate")]
[assembly: AssemblyVersion("1.2.0.0")]
[assembly: AssemblyFileVersion("1.2.0.0")]

namespace ReplayPlusUpdater
{
    // A file a release pins: where it is and what it must hash to.
    class Pin
    {
        public string Repo, Tag, Name, Sha256;
        public long Size;
        public string Url { get { return "https://github.com/" + Repo + "/releases/download/" + Uri.EscapeDataString(Tag) + "/" + Uri.EscapeDataString(Name); } }
    }

    // One Replay Plus build: a ReSkate.dll with Replay Plus on one Animator Studio version, and the ReSkate launcher it
    // goes with. A build made here has no Dll pin, only File.
    class Build
    {
        public string ReplayPlus, Studio, StudioTag, StudioCommit, ReSkate, File;
        public Pin Dll, Launcher, LauncherJson;
        public string Title { get { return "Replay Plus " + ReplayPlus + " (Animator Studio " + Studio + ", ReSkate " + ReSkate + ")"; } }
    }

    // A Replay Plus release, from its replay-plus-builds.json (schema 2: a build for each Animator Studio version and the
    // patches to build one for any other) or, in releases from before that, its replay-plus.json (schema 1: one build).
    class Release
    {
        public string Tag, ReplayPlus, UpdaterVersion;
        public bool Prerelease;
        public List<Build> Builds = new List<Build>();
        public List<Pin> Patches = new List<Pin>();

        // The newest Replay Plus made for that Studio version, or null.
        public Build For(string studio)
        {
            Build best = null;
            foreach (var b in Builds)
                if (studio != null && Updater.Compare(b.Studio, studio) == 0 && (best == null || Updater.Compare(b.ReplayPlus, best.ReplayPlus) > 0)) best = b;
            return best;
        }

        // The build on the newest Studio.
        public Build Newest
        {
            get
            {
                Build best = null;
                foreach (var b in Builds)
                {
                    int c = best == null ? 1 : Updater.Compare(b.Studio, best.Studio);
                    if (c > 0 || (c == 0 && Updater.Compare(b.ReplayPlus, best.ReplayPlus) > 0)) best = b;
                }
                return best;
            }
        }

        public string Studios
        {
            get
            {
                var list = new List<string>();
                foreach (var b in Builds) if (!list.Contains(b.Studio)) list.Add(b.Studio);
                list.Sort((a, b) => Updater.Compare(b, a));
                return string.Join(", ", list.ToArray());
            }
        }
    }

    // What the game folder has now.
    class Installed
    {
        public bool HasDll, Ours, Vanilla;
        public string Sha256, ReplayPlus, Studio, ReSkate, Describe;
        public bool HasStudio { get { return Studio != null; } }
    }

    // ReSkate's newest release: its launcher.json pins the ReSkate.dll and the launcher in it.
    class ReSkateRelease
    {
        public string Tag, Version, JsonFile;
        public Pin Runtime, Launcher;
    }

    // What to do for a Studio version: a ready-made build, a build here, or neither (and why).
    class Choice
    {
        public Build Build;
        public bool BuildHere;
        public string Studio, Why;
        public bool Possible { get { return Build != null || BuildHere; } }
    }

    class PatchesDontFit : Exception { public PatchesDontFit(string message) : base(message) { } }
    class BuildFailed : Exception { public BuildFailed(string message) : base(message) { } }

    static class Json
    {
        static readonly JavaScriptSerializer serializer = new JavaScriptSerializer { MaxJsonLength = 16 * 1024 * 1024 };
        public static object Parse(string text) { return serializer.DeserializeObject(text); }
        public static string Write(object o) { return serializer.Serialize(o); }
        public static Dictionary<string, object> Obj(object o) { return o as Dictionary<string, object>; }
        public static object Get(Dictionary<string, object> d, string key)
        {
            object v;
            return d != null && d.TryGetValue(key, out v) ? v : null;
        }
        public static string Str(Dictionary<string, object> d, string key)
        {
            var v = Get(d, key);
            return v == null ? null : Convert.ToString(v, CultureInfo.InvariantCulture);
        }
        public static long Num(Dictionary<string, object> d, string key)
        {
            var v = Get(d, key);
            return v == null ? 0 : Convert.ToInt64(v, CultureInfo.InvariantCulture);
        }
        public static bool Bool(Dictionary<string, object> d, string key)
        {
            var v = Get(d, key);
            return v is bool && (bool)v;
        }
        public static object[] List(Dictionary<string, object> d, string key)
        {
            return Get(d, key) as object[] ?? new object[0];
        }
    }

    static class Web
    {
        public const string Agent = "ReplayPlusUpdater";

        static HttpWebRequest Request(string url)
        {
            var request = (HttpWebRequest)WebRequest.Create(url);
            request.UserAgent = Agent;
            bool api = url.Contains("api.github.com");
            request.Accept = api ? "application/vnd.github+json" : "*/*";
            var token = Environment.GetEnvironmentVariable("REPLAY_PLUS_GITHUB_TOKEN");
            if (api && !string.IsNullOrEmpty(token)) request.Headers["Authorization"] = "Bearer " + token;
            request.Timeout = 30000;
            request.ReadWriteTimeout = 60000;
            request.AllowAutoRedirect = true;
            return request;
        }

        public static string Text(string url)
        {
            using (var response = (HttpWebResponse)Request(url).GetResponse())
            using (var reader = new StreamReader(response.GetResponseStream(), Encoding.UTF8))
                return reader.ReadToEnd();
        }

        // A repository's newest release (not a pre-release), without GitHub's API, which allows only 60 calls an hour
        // from one address: github.com's /releases/latest sends you on to that release's page.
        public static string LatestTag(string repo)
        {
            var request = Request("https://github.com/" + repo + "/releases/latest");
            request.AllowAutoRedirect = false;
            request.Method = "HEAD";
            using (var response = (HttpWebResponse)request.GetResponse())
            {
                var m = Regex.Match(response.Headers["Location"] ?? "", @"/releases/tag/([^/?#]+)$");
                if (!m.Success) throw new Exception(repo + " has no releases");
                return Uri.UnescapeDataString(m.Groups[1].Value);
            }
        }

        public static bool Missing(WebException e)
        {
            var response = e.Response as HttpWebResponse;
            return response != null && response.StatusCode == HttpStatusCode.NotFound;
        }

        // Into `path` (a .part file first), reporting (bytes so far, bytes in all).
        public static void Download(string url, string path, long size, Action<long, long> progress)
        {
            var part = path + ".part";
            using (var response = (HttpWebResponse)Request(url).GetResponse())
            using (var input = response.GetResponseStream())
            using (var output = File.Create(part))
            {
                long total = response.ContentLength > 0 ? response.ContentLength : size, done = 0;
                var buffer = new byte[1 << 16];
                int read;
                while ((read = input.Read(buffer, 0, buffer.Length)) > 0)
                {
                    output.Write(buffer, 0, read);
                    done += read;
                    if (progress != null) progress(done, total);
                }
            }
            if (File.Exists(path)) File.Delete(path);
            File.Move(part, path);
        }
    }

    static class Native
    {
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenProcess(uint access, bool inherit, int id);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern bool QueryFullProcessImageName(IntPtr process, int flags, StringBuilder name, ref int size);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern bool GetDiskFreeSpaceEx(string folder, out ulong available, out ulong total, out ulong free);

        // Where a running process's .exe is, or null when Windows won't say.
        public static string ExePath(int id)
        {
            var handle = OpenProcess(0x1000, false, id); // PROCESS_QUERY_LIMITED_INFORMATION
            if (handle == IntPtr.Zero) return null;
            try
            {
                var name = new StringBuilder(1024);
                int size = name.Capacity;
                return QueryFullProcessImageName(handle, 0, name, ref size) ? name.ToString(0, size) : null;
            }
            finally { CloseHandle(handle); }
        }

        public static long FreeBytes(string folder)
        {
            ulong available, total, free;
            return GetDiskFreeSpaceEx(folder, out available, out total, out free) ? (long)available : -1;
        }
    }

    // What a build here needs: git, CMake (with ctest) and Visual Studio 2022's C++ build tools (v143, the toolset
    // ReSkate's CMake preset asks for, with its "Visual Studio 17 2022" generator).
    class BuildTools
    {
        public string Git, CMake, CTest, VisualStudio;
        public List<string> Missing = new List<string>();
        public bool Ready { get { return Missing.Count == 0; } }
        public string MissingText
        {
            get
            {
                if (Missing.Count <= 1) return string.Join("", Missing.ToArray());
                return string.Join(", ", Missing.GetRange(0, Missing.Count - 1).ToArray()) + " and " + Missing[Missing.Count - 1];
            }
        }

        const string CMakeInVs = @"Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe";

        public static BuildTools Find()
        {
            var t = new BuildTools();
            t.Git = OnPath("git.exe") ?? FirstFile(
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), @"Git\cmd\git.exe"),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), @"Git\cmd\git.exe"),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), @"Programs\Git\cmd\git.exe"));
            var vswhere = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), @"Microsoft Visual Studio\Installer\vswhere.exe");
            var cmakes = new List<string>();
            if (File.Exists(vswhere))
            {
                var found = Lines(Capture(vswhere, "-products * -version [17.0,18.0) -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -property installationPath"));
                if (found.Count > 0 && Directory.Exists(found[0])) t.VisualStudio = found[0];
                if (t.VisualStudio != null) cmakes.Add(Path.Combine(t.VisualStudio, CMakeInVs));
                foreach (var vs in Lines(Capture(vswhere, "-all -products * -property installationPath"))) cmakes.Add(Path.Combine(vs, CMakeInVs));
            }
            foreach (var c in cmakes) if (File.Exists(c)) { t.CMake = c; break; }
            if (t.CMake == null) t.CMake = OnPath("cmake.exe");
            if (t.CMake != null)
            {
                t.CTest = Path.Combine(Path.GetDirectoryName(t.CMake), "ctest.exe");
                if (!File.Exists(t.CTest)) t.CTest = null;
            }
            if (t.Git == null) t.Missing.Add("git");
            if (t.VisualStudio == null) t.Missing.Add("Visual Studio Build Tools 2022 with its C++ tools");
            if (t.CMake == null || t.CTest == null) t.Missing.Add("CMake");
            return t;
        }

        static string OnPath(string name)
        {
            foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(';'))
            {
                try
                {
                    var path = Path.Combine(dir.Trim().Trim('"'), name);
                    if (dir.Trim().Length > 0 && File.Exists(path)) return path;
                }
                catch (ArgumentException) { }
            }
            return null;
        }

        static string FirstFile(params string[] paths)
        {
            foreach (var p in paths) if (File.Exists(p)) return p;
            return null;
        }

        static List<string> Lines(string text)
        {
            var list = new List<string>();
            foreach (var line in (text ?? "").Split('\n')) if (line.Trim().Length > 0) list.Add(line.Trim());
            return list;
        }

        static string Capture(string exe, string args)
        {
            try
            {
                var info = new ProcessStartInfo(exe, args) { UseShellExecute = false, RedirectStandardOutput = true, CreateNoWindow = true, StandardOutputEncoding = Encoding.UTF8 };
                using (var p = Process.Start(info))
                {
                    var text = p.StandardOutput.ReadToEnd();
                    p.WaitForExit();
                    return text;
                }
            }
            catch (Exception) { return ""; }
        }
    }

    class Updater
    {
        public const string Repo = "vraxa/replay-plus";
        public const string StudioRepo = "FourthStudio/ReSkate-Animator-Studio";
        public const string ReSkateRepo = "Dingo-Shenanigans/ReSkate";
        public const string Api = "https://api.github.com/repos/";
        public const string BuildsManifest = "replay-plus-builds.json";
        public const string OldManifest = "replay-plus.json";
        const string RecordName = "replay-plus-updater.json";
        const string OriginalSuffix = ".before-replay-plus";
        const string ModId = "AnimatorStudio_ReplayPlus";
        const long BuildSpace = 4L << 30;

        public string Folder;
        public bool Prerelease, NoBuild, ForceBuild;
        public string FromFolder, PatchesFolder;
        public int Jobs;
        public Action<string> Log = delegate { };
        public Action<long, long, string> Progress = delegate { };
        public Action<string> Status = delegate { };
        public Action<int, int, string> Steps = delegate { };

        static string dataFolder;
        public static string DataFolder
        {
            get { return dataFolder ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ReSkate", "Replay Plus"); }
            set { dataFolder = string.IsNullOrEmpty(value) ? null : Path.GetFullPath(value); }
        }
        public static string Version
        {
            get { var v = Assembly.GetExecutingAssembly().GetName().Version; return v.Major + "." + v.Minor + (v.Build > 0 ? "." + v.Build : ""); }
        }

        public static bool IsReSkateFolder(string folder)
        {
            return !string.IsNullOrEmpty(folder) && File.Exists(Path.Combine(folder, "ReSkateLauncher.exe"));
        }

        // Looked up once (vswhere takes a moment), and again on each check.
        BuildTools tools;
        public BuildTools Tools { get { return tools ?? (tools = BuildTools.Find()); } }
        public void RefreshTools() { tools = BuildTools.Find(); }

        // ---- What is out there --------------------------------------------------------------------------------------

        public Release Latest()
        {
            if (!string.IsNullOrEmpty(FromFolder))
            {
                var builds = Path.Combine(FromFolder, BuildsManifest);
                var local = ParseRelease(File.ReadAllText(File.Exists(builds) ? builds : Path.Combine(FromFolder, OldManifest)), "local");
                local.Tag = "local";
                return local;
            }
            if (!Prerelease)
            {
                // The newest release without GitHub's API: its manifest, the new kind first.
                var newest = Web.LatestTag(Repo);
                foreach (var name in new[] { BuildsManifest, OldManifest })
                {
                    try
                    {
                        var found = ParseRelease(Web.Text("https://github.com/" + Repo + "/releases/download/" + Uri.EscapeDataString(newest) + "/" + name), newest);
                        found.Tag = newest;
                        return found;
                    }
                    catch (WebException e) { if (!Web.Missing(e)) throw; }
                }
                throw new Exception("the newest Replay Plus release (" + newest + ") can't be installed by this updater");
            }
            Dictionary<string, object> release = null;
            foreach (var item in Json.Parse(Web.Text(Api + Repo + "/releases?per_page=10")) as object[] ?? new object[0])
            {
                var r = Json.Obj(item);
                if (r == null || Json.Bool(r, "draft") || ManifestOf(r) == null) continue;
                release = r;
                break;
            }
            if (release == null) throw new Exception("no Replay Plus release the updater can install");
            var tag = Json.Str(release, "tag_name");
            var parsed = ParseRelease(Web.Text("https://github.com/" + Repo + "/releases/download/" + Uri.EscapeDataString(tag) + "/" + ManifestOf(release)), tag);
            parsed.Tag = tag;
            parsed.Prerelease = Json.Bool(release, "prerelease");
            return parsed;
        }

        static string ManifestOf(Dictionary<string, object> release)
        {
            bool builds = false, old = false;
            foreach (var a in Json.List(release, "assets"))
            {
                var name = Json.Str(Json.Obj(a), "name");
                if (name == BuildsManifest) builds = true;
                else if (name == OldManifest) old = true;
            }
            return builds ? BuildsManifest : old ? OldManifest : null;
        }

        public static Release ParseRelease(string text, string tag)
        {
            var d = Json.Obj(Json.Parse(text));
            var schema = d == null ? 0 : Json.Num(d, "schema");
            if (schema != 1 && schema != 2) throw new Exception("that Replay Plus release is newer than this updater, so get the newest updater from the releases page");
            var r = new Release { ReplayPlus = Json.Str(d, "replay_plus"), UpdaterVersion = Json.Str(d, "updater") };
            if (schema == 1)
            {
                r.Builds.Add(new Build
                {
                    ReplayPlus = r.ReplayPlus,
                    Studio = Json.Str(d, "studio"),
                    ReSkate = Json.Str(d, "reskate"),
                    Dll = ParsePin(Json.Obj(Json.Get(d, "dll")), tag),
                    Launcher = ParsePin(Json.Obj(Json.Get(d, "launcher")), tag),
                    LauncherJson = ParsePin(Json.Obj(Json.Get(d, "launcher_json")), tag),
                });
            }
            else
            {
                foreach (var item in Json.List(d, "builds"))
                {
                    var b = Json.Obj(item);
                    r.Builds.Add(new Build
                    {
                        ReplayPlus = Json.Str(b, "replay_plus") ?? r.ReplayPlus,
                        Studio = Json.Str(b, "studio"),
                        StudioTag = Json.Str(b, "studio_tag"),
                        StudioCommit = Json.Str(b, "studio_commit"),
                        ReSkate = Json.Str(b, "reskate"),
                        Dll = ParsePin(Json.Obj(Json.Get(b, "dll")), tag),
                        Launcher = ParsePin(Json.Obj(Json.Get(b, "launcher")), tag),
                        LauncherJson = ParsePin(Json.Obj(Json.Get(b, "launcher_json")), tag),
                    });
                }
                foreach (var item in Json.List(d, "patches")) r.Patches.Add(ParsePin(Json.Obj(item), tag));
            }
            r.Builds.RemoveAll(b => b.Dll == null || string.IsNullOrEmpty(b.Studio) || string.IsNullOrEmpty(b.ReplayPlus));
            if (r.Builds.Count == 0 && r.Patches.Count == 0) throw new Exception("that Replay Plus release has no ReSkate.dll in it");
            if (string.IsNullOrEmpty(r.ReplayPlus) && r.Newest != null) r.ReplayPlus = r.Newest.ReplayPlus;
            return r;
        }

        static Pin ParsePin(Dictionary<string, object> d, string tag)
        {
            if (d == null) return null;
            var pin = new Pin
            {
                Repo = Json.Str(d, "repo") ?? Repo,
                Tag = Json.Str(d, "tag") ?? tag,
                Name = Json.Str(d, "name"),
                Sha256 = (Json.Str(d, "sha256") ?? "").ToLowerInvariant(),
                Size = Json.Num(d, "size"),
            };
            if (string.IsNullOrEmpty(pin.Name) || pin.Sha256.Length != 64) throw new Exception("that Replay Plus release names a file without its SHA-256");
            return pin;
        }

        public string LatestStudio()
        {
            try { return Web.LatestTag(StudioRepo); }
            catch (Exception) { return null; }
        }

        // ReSkate's newest release and the files its launcher.json pins (launcher.json kept in the download folder).
        public ReSkateRelease LatestReSkate()
        {
            return ReSkateAt(Web.LatestTag(ReSkateRepo), true);
        }

        // ReSkate's release of a version: its tag is the version itself or "v" + it ("20261116", "v2.1.4-4").
        ReSkateRelease ReSkateAt(string version, bool exact)
        {
            var tags = exact ? new[] { version } : new[] { version, "v" + version };
            foreach (var tag in tags)
            {
                var cache = Cache(ReSkateRepo, tag);
                var json = Path.Combine(cache, "launcher.json");
                try { Web.Download("https://github.com/" + ReSkateRepo + "/releases/download/" + Uri.EscapeDataString(tag) + "/launcher.json", json, 0, null); }
                catch (WebException e)
                {
                    if (Web.Missing(e)) continue;
                    throw;
                }
                var d = Json.Obj(Json.Parse(File.ReadAllText(json)));
                var r = new ReSkateRelease { Tag = tag, JsonFile = json };
                r.Runtime = FilePin(Json.Obj(Json.Get(d, "runtime")), tag, "ReSkate.dll");
                r.Launcher = FilePin(Json.Obj(Json.Get(d, "launcher")), tag, "ReSkateLauncher.exe");
                r.Version = Json.Str(Json.Obj(Json.Get(d, "runtime")), "version") ?? version;
                return r;
            }
            return null;
        }

        static Pin FilePin(Dictionary<string, object> d, string tag, string name)
        {
            var sha = (Json.Str(d, "sha256") ?? "").ToLowerInvariant();
            if (sha.Length != 64) return null;
            var url = Json.Str(d, "url") ?? "";
            if (url.StartsWith("asset:")) name = url.Substring(6);
            return new Pin { Repo = ReSkateRepo, Tag = tag, Name = name, Sha256 = sha, Size = Json.Num(d, "size") };
        }

        static string Cache(string repo, string tag)
        {
            var dir = Path.Combine(DataFolder, "Updater downloads", repo.Replace('/', '_'), Regex.Replace(tag ?? "local", @"[^0-9A-Za-z._+\-]", "_"));
            Directory.CreateDirectory(dir);
            return dir;
        }

        // ---- What is installed ------------------------------------------------------------------------------------

        public Installed Inspect()
        {
            var result = InspectFile(Path.Combine(Folder, "ReSkate.dll"));
            if (result.HasDll && !result.Ours && !result.HasStudio)
            {
                // Vanilla ReSkate: the launcher.json beside it pins this very file.
                try
                {
                    var json = Path.Combine(Folder, "launcher.json");
                    var runtime = File.Exists(json) ? Json.Obj(Json.Get(Json.Obj(Json.Parse(File.ReadAllText(json))), "runtime")) : null;
                    if (runtime != null && (Json.Str(runtime, "sha256") ?? "").ToLowerInvariant() == result.Sha256)
                    {
                        result.Vanilla = true;
                        result.ReSkate = Json.Str(runtime, "version") ?? result.ReSkate;
                    }
                }
                catch (Exception) { }
                result.Describe = result.Vanilla ? "ReSkate " + result.ReSkate + " (vanilla)"
                    : result.ReSkate != null ? "ReSkate " + result.ReSkate + ", without the Animator Studio or Replay Plus"
                    : "a ReSkate.dll without the Animator Studio or Replay Plus";
            }
            return result;
        }

        public static Installed InspectFile(string dll)
        {
            var result = new Installed();
            if (!File.Exists(dll)) { result.Describe = "no ReSkate.dll here yet"; return result; }
            result.HasDll = true;
            var bytes = File.ReadAllBytes(dll);
            result.Sha256 = Hex(SHA256.Create().ComputeHash(bytes));
            // The version the DLL was built as, a C string in it: "1.1+studio-2.1.5.reskate-20261116" for Replay Plus,
            // "2.1.5+reskate.20261116" for the Animator Studio; vanilla ReSkate has no such string, only its own
            // version ("20261116").
            var text = Encoding.GetEncoding(28591).GetString(bytes);
            var ours = Regex.Match(text, @"(?<![0-9.])(\d+(?:\.\d+)+)\+studio-(\d+(?:\.\d+)+)\.reskate-([0-9A-Za-z.\-]+?)\x00");
            if (ours.Success)
            {
                result.Ours = true;
                result.ReplayPlus = ours.Groups[1].Value;
                result.Studio = ours.Groups[2].Value;
                result.ReSkate = ours.Groups[3].Value;
                result.Describe = "Replay Plus " + result.ReplayPlus + " (Animator Studio " + result.Studio + ", ReSkate " + result.ReSkate + ")";
                return result;
            }
            var studio = Regex.Match(text, @"(?<![0-9.])(\d+\.\d+\.\d+)\+reskate\.([0-9A-Za-z.\-]+?)\x00");
            if (studio.Success)
            {
                result.Studio = studio.Groups[1].Value;
                result.ReSkate = studio.Groups[2].Value;
                result.Describe = "Animator Studio " + result.Studio + " (ReSkate " + result.ReSkate + "), no Replay Plus";
                return result;
            }
            var versions = new List<string>();
            foreach (Match m in Regex.Matches(text, @"\x00(20\d{6})\x00"))
                if (!versions.Contains(m.Groups[1].Value)) versions.Add(m.Groups[1].Value);
            if (versions.Count == 1) result.ReSkate = versions[0];
            result.Describe = result.ReSkate != null ? "ReSkate " + result.ReSkate + ", without the Animator Studio or Replay Plus"
                : "a ReSkate.dll without the Animator Studio or Replay Plus";
            return result;
        }

        public static int Compare(string a, string b)
        {
            var x = Numbers(a);
            var y = Numbers(b);
            for (int i = 0; i < Math.Max(x.Count, y.Count); ++i)
            {
                int p = i < x.Count ? x[i] : 0, q = i < y.Count ? y[i] : 0;
                if (p != q) return p.CompareTo(q);
            }
            return 0;
        }

        static List<int> Numbers(string v)
        {
            var list = new List<int>();
            if (v == null) return list;
            foreach (Match m in Regex.Matches(v.TrimStart('v', 'V'), @"\d+"))
            {
                int n;
                list.Add(int.TryParse(m.Value, out n) ? n : 0);
            }
            return list;
        }

        // skate. or the ReSkate launcher, when one runs from this folder (or from somewhere Windows won't say).
        public string Running()
        {
            foreach (var name in new[] { "Skate", "ReSkateLauncher" })
                foreach (var p in Process.GetProcessesByName(name))
                {
                    string exe = null;
                    try { exe = Native.ExePath(p.Id); } catch (Exception) { }
                    bool here = exe == null || string.IsNullOrEmpty(Folder) ||
                        string.Equals(Path.GetFullPath(Path.GetDirectoryName(exe)).TrimEnd('\\'), Path.GetFullPath(Folder).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase);
                    if (here) return name == "Skate" ? "skate." : "the ReSkate launcher";
                }
            return null;
        }

        void CheckClosed()
        {
            var running = Running();
            if (running != null) throw new Exception("close " + running + " first");
        }

        // ---- Choosing -----------------------------------------------------------------------------------------------

        // What there is for a Studio version: the newest ready-made Replay Plus for it, else a build here.
        public Choice ChooseFor(Release release, string studio)
        {
            var choice = new Choice { Studio = studio };
            if (release == null || studio == null) { choice.Why = "the updater couldn't check what's out there"; return choice; }
            choice.Build = ForceBuild ? null : release.For(studio);
            // A ready-made one, unless it's an older Replay Plus than the release's and the newest can be built here.
            bool canBuild = !NoBuild && (release.Patches.Count > 0 || !string.IsNullOrEmpty(PatchesFolder)) && Tools.Ready;
            if (choice.Build != null && Compare(choice.Build.ReplayPlus, release.ReplayPlus) < 0 && canBuild) choice.Build = null;
            if (choice.Build != null) return choice;
            if (release.Patches.Count == 0 && string.IsNullOrEmpty(PatchesFolder))
                choice.Why = "Replay Plus isn't ready for Animator Studio " + studio + " yet";
            else if (NoBuild)
                choice.Why = "there's no ready-made Replay Plus for Animator Studio " + studio;
            else if (Tools.Ready) choice.BuildHere = true;
            else choice.Why = "there's no ready-made Replay Plus for Animator Studio " + studio + ", and building it here needs " + Tools.MissingText;
            return choice;
        }

        // --status: what's here, what's out, and what each button would do now.
        public void Report()
        {
            var installed = Inspect();
            Log("You have: " + installed.Describe + (installed.HasDll ? " (SHA-256 " + installed.Sha256.Substring(0, 12) + "...)" : ""));
            Release release = null;
            try { release = Latest(); } catch (Exception e) { Log("Replay Plus: couldn't check (" + e.Message + ")"); }
            var studio = LatestStudio();
            ReSkateRelease reskate = null;
            try { reskate = LatestReSkate(); } catch (Exception) { }
            if (release != null) Log("Replay Plus " + release.ReplayPlus + " (" + release.Tag + "): ready-made for Animator Studio " + release.Studios +
                (release.Patches.Count > 0 ? "; patches to build it for others" : "; no patches"));
            Log("Newest Animator Studio: " + (studio ?? "?") + ", newest ReSkate: " + (reskate != null ? reskate.Version : "?"));
            Log("Building here: " + (NoBuild ? "off (--no-build)" : Tools.Ready ? "possible (git, CMake and Visual Studio 2022's C++ tools are here)" : "needs " + Tools.MissingText));
            var latest = ChooseFor(release, studio);
            Log("1 Install Replay Plus + Animator Studio: " + Says(latest, release));
            if (installed.HasStudio) Log("3 " + (installed.Ours ? "Update" : "Add") + " Replay Plus for Studio " + installed.Studio + ": " + Says(ChooseFor(release, installed.Studio), null));
            if (installed.Ours) Log("4 Update Animator Studio (keeping Replay Plus): " + (studio != null && Compare(studio, installed.Studio) > 0 ? Says(latest, null) : "already the newest Studio"));
            else Log("4 " + (installed.HasStudio ? "Update" : "Get") + " Animator Studio: " + (studio == null ? "?" : installed.HasStudio && Compare(studio, installed.Studio) <= 0 ? "already the newest Studio" : "plain Animator Studio " + studio));
            Log("5 Vanilla ReSkate: " + (reskate == null ? "?" : installed.Vanilla && reskate.Runtime != null && installed.Sha256 == reskate.Runtime.Sha256 ? "already vanilla ReSkate " + reskate.Version : "ReSkate " + reskate.Version));
            Log("Put back what I had before: " + (CanRestore() ? "yes" : "nothing kept"));
        }

        static string Says(Choice c, Release fallback)
        {
            if (c.Build != null) return "ready-made " + c.Build.Title + " (" + c.Build.Dll.Name + ")";
            if (c.BuildHere) return "builds Replay Plus for Animator Studio " + c.Studio + " here";
            if (fallback != null && fallback.Newest != null) return c.Why + ", so " + fallback.Newest.Title;
            return "can't: " + c.Why;
        }

        // ---- The five things it does -----------------------------------------------------------------------------

        // 1. The newest Animator Studio with Replay Plus on it, and the ReSkate launcher it needs.
        public void InstallWithStudio(Release release)
        {
            if (release == null) throw new Exception("couldn't check GitHub for Replay Plus");
            var studio = LatestStudio();
            if (studio == null && release.Newest != null)
            {
                Log("(couldn't check GitHub for the newest Animator Studio, so this is the newest one Replay Plus is ready for)");
                studio = release.Newest.Studio;
            }
            var choice = ChooseFor(release, studio);
            if (!choice.Possible)
            {
                var newest = release.Newest;
                if (newest == null) throw new Exception(Program.Sentence(choice.Why));
                Log("(" + Program.Sentence(choice.Why) + " So this is Replay Plus on Animator Studio " + newest.Studio + ", the newest one it's ready for.)");
                choice = new Choice { Build = newest, Studio = newest.Studio };
            }
            Build build;
            try { build = Get(choice, release); }
            catch (Exception e)
            {
                // A build here that didn't work: the newest ready-made one instead.
                if (!choice.BuildHere || !(e is PatchesDontFit || e is BuildFailed) || release.Newest == null) throw;
                Log("(" + Program.Sentence(e.Message) + " So this is Replay Plus on Animator Studio " + release.Newest.Studio + ", the newest one it's ready for.)");
                build = Get(new Choice { Build = release.Newest, Studio = release.Newest.Studio }, release);
            }
            Install(build, true);
        }

        // 3. The newest Replay Plus for the Animator Studio version that's in now; the Studio stays as it is.
        public void UpdateReplayPlus(Release release)
        {
            var installed = Inspect();
            if (!installed.HasStudio) throw new Exception("there's no Animator Studio here to put Replay Plus on");
            var choice = ChooseFor(release, installed.Studio);
            if (!choice.Possible) throw new Exception(choice.Why + ", so nothing was changed");
            Install(Get(choice, release), true);
        }

        // 4. The newest Animator Studio, with Replay Plus on it when Replay Plus is in now.
        public void UpdateStudio(Release release)
        {
            var installed = Inspect();
            var studio = LatestStudio();
            if (studio == null) throw new Exception("couldn't check GitHub for the newest Animator Studio");
            if (installed.HasStudio && Compare(studio, installed.Studio) <= 0)
            {
                Log("You already have the newest Animator Studio (" + installed.Studio + "), so there's nothing to do.");
                return;
            }
            if (!installed.Ours) { UpdatePlainStudio(); return; }
            var choice = ChooseFor(release, studio);
            if (!choice.Possible) throw new Exception(choice.Why + ", so you stay on " + installed.Studio + " for now. Nothing was changed");
            Install(Get(choice, release), true);
        }

        // The build a choice names: downloaded and checked, or made here.
        Build Get(Choice choice, Release release)
        {
            if (choice.Build != null)
            {
                Log("Getting " + choice.Build.Title);
                var b = choice.Build;
                return new Build
                {
                    ReplayPlus = b.ReplayPlus, Studio = b.Studio, StudioTag = b.StudioTag, ReSkate = b.ReSkate, Launcher = b.Launcher, LauncherJson = b.LauncherJson,
                    Dll = b.Dll, File = Fetch(b.Dll, Cache(b.Dll.Repo, b.Dll.Tag), true),
                };
            }
            return BuildHere(choice.Studio, release);
        }

        // A build's DLL into the game folder, with the ReSkate launcher it's built for; "updates" off, so the launcher
        // doesn't put its own ReSkate.dll back.
        void Install(Build build, bool checkDll)
        {
            if (checkDll)
            {
                var check = InspectFile(build.File);
                if (!check.Ours || Compare(check.Studio, build.Studio) != 0)
                    throw new Exception("that file isn't Replay Plus on Animator Studio " + build.Studio + ", so nothing was changed");
            }
            string newLauncher = null, newJson = null;
            var launcherPath = Path.Combine(Folder, "ReSkateLauncher.exe");
            var jsonPath = Path.Combine(Folder, "launcher.json");
            if (build.Launcher != null)
            {
                if (!Matches(launcherPath, build.Launcher))
                {
                    Log("Getting the ReSkate " + build.Launcher.Tag + " launcher from ReSkate's own release");
                    newLauncher = Fetch(build.Launcher, Cache(build.Launcher.Repo, build.Launcher.Tag), false);
                }
                if (build.LauncherJson != null && !Matches(jsonPath, build.LauncherJson))
                    newJson = Fetch(build.LauncherJson, Cache(build.LauncherJson.Repo, build.LauncherJson.Tag), false);
            }
            else LauncherFor(build.ReSkate, out newLauncher, out newJson);

            CheckClosed();
            var record = Begin();
            Put(build.File, Path.Combine(Folder, "ReSkate.dll"));
            if (newLauncher != null) Put(newLauncher, launcherPath);
            if (newJson != null) Put(newJson, jsonPath);
            Updates(record, false);
            Stamp(record, build.ReplayPlus, build.Studio, build.ReSkate, build.Dll != null ? build.Dll.Tag : "built here", build.Title);
            Log("Done: " + build.Title + " is in. Start ReSkate like you normally do.");
        }

        // The launcher (and its launcher.json) of ReSkate's release of that version, when the one here isn't it.
        void LauncherFor(string reskate, out string newLauncher, out string newJson)
        {
            newLauncher = newJson = null;
            var release = string.IsNullOrEmpty(reskate) ? null : ReSkateAt(reskate, false);
            if (release == null || release.Launcher == null)
            {
                Log("(couldn't find the ReSkate " + reskate + " launcher, so the launcher stays as it is)");
                return;
            }
            var launcherPath = Path.Combine(Folder, "ReSkateLauncher.exe");
            if (!Matches(launcherPath, release.Launcher))
            {
                Log("Getting the ReSkate " + reskate + " launcher from ReSkate's own release");
                newLauncher = Fetch(release.Launcher, Cache(ReSkateRepo, release.Tag), false);
            }
            var jsonPath = Path.Combine(Folder, "launcher.json");
            if (!File.Exists(jsonPath) || Sha256Of(jsonPath) != Sha256Of(release.JsonFile)) newJson = release.JsonFile;
        }

        // 2. Replay Plus out, the Animator Studio kept: the Studio's own ReSkate.dll of the version this Replay Plus was
        // built on (so it fits the launcher), from its own GitHub release and checked against its SHA-256. The launcher
        // and what was kept from before Replay Plus stay as they are.
        public void RemoveReplayPlus()
        {
            CheckClosed();
            var installed = Inspect();
            if (!installed.Ours) throw new Exception("Replay Plus isn't installed here");
            Dictionary<string, object> release = null;
            foreach (var tag in new[] { installed.Studio, "v" + installed.Studio })
            {
                try { release = Json.Obj(Json.Parse(Web.Text(Api + StudioRepo + "/releases/tags/" + Uri.EscapeDataString(tag)))); break; }
                catch (WebException e) { if (!Web.Missing(e)) throw; }
            }
            if (release == null) release = Json.Obj(Json.Parse(Web.Text(Api + StudioRepo + "/releases/latest")));
            var studioDll = FetchStudio(release);
            var check = InspectFile(studioDll);
            CheckClosed();
            var record = Begin();
            Put(studioDll, Path.Combine(Folder, "ReSkate.dll"));
            Stamp(record, null, check.Studio, check.ReSkate, check.Studio, "Animator Studio " + check.Studio);
            Log("Done: Replay Plus is out and Animator Studio " + check.Studio + " is in. Your presets and settings are still there if you want it back.");
        }

        // An Animator Studio release's ReSkate.dll, downloaded and checked (GitHub's SHA-256 of it, else its .sha256).
        string FetchStudio(Dictionary<string, object> release)
        {
            var tag = Json.Str(release, "tag_name");
            string sumUrl = null, digest = null;
            long size = 0;
            bool hasDll = false;
            foreach (var item in Json.List(release, "assets"))
            {
                var a = Json.Obj(item);
                var name = Json.Str(a, "name");
                if (name == "ReSkate.dll") { hasDll = true; size = Json.Num(a, "size"); digest = Json.Str(a, "digest"); }
                else if (name == "ReSkate.dll.sha256") sumUrl = Json.Str(a, "browser_download_url");
            }
            if (!hasDll) throw new Exception("the Animator Studio's release " + tag + " has no ReSkate.dll");
            string sha = digest != null && digest.StartsWith("sha256:") ? digest.Substring(7) : null;
            if (sha == null && sumUrl != null) sha = Regex.Match(Web.Text(sumUrl), "[0-9a-fA-F]{64}").Value;
            if (string.IsNullOrEmpty(sha) || sha.Length != 64) throw new Exception("couldn't get the Animator Studio's checksum, so nothing was changed");
            var pin = new Pin { Repo = StudioRepo, Tag = tag, Name = "ReSkate.dll", Sha256 = sha.ToLowerInvariant(), Size = size };
            Log("Getting Animator Studio " + tag + " from its own release");
            var path = Fetch(pin, Cache(StudioRepo, tag), false);
            var check = InspectFile(path);
            if (check.Ours || check.Studio == null) throw new Exception("that download isn't the Animator Studio's ReSkate.dll, so nothing was changed");
            return path;
        }

        // The newest Animator Studio on its own (no Replay Plus here): its ReSkate.dll from its own release, checked,
        // and the ReSkate launcher it is built on (its version string names it: "2.1.5+reskate.20261116") from ReSkate's
        // release of that version, checked against that release's launcher.json. "updates" stays off, as the Studio
        // needs (with it on, the launcher puts vanilla ReSkate's DLL back).
        void UpdatePlainStudio()
        {
            CheckClosed();
            var studioDll = FetchStudio(Json.Obj(Json.Parse(Web.Text(Api + StudioRepo + "/releases/latest"))));
            var check = InspectFile(studioDll);
            string newLauncher, newJson;
            LauncherFor(check.ReSkate, out newLauncher, out newJson);
            CheckClosed();
            var record = Begin();
            Put(studioDll, Path.Combine(Folder, "ReSkate.dll"));
            if (newLauncher != null) Put(newLauncher, Path.Combine(Folder, "ReSkateLauncher.exe"));
            if (newJson != null) Put(newJson, Path.Combine(Folder, "launcher.json"));
            Updates(record, false);
            Stamp(record, null, check.Studio, check.ReSkate, check.Studio, "Animator Studio " + check.Studio);
            Log("Done: Animator Studio " + check.Studio + " is in" + (newLauncher != null ? ", with the ReSkate " + check.ReSkate + " launcher" : "") +
                ". Start ReSkate like you normally do.");
        }

        // 5. Vanilla: ReSkate's own newest ReSkate.dll and launcher (checked against its launcher.json), no Studio and no
        // Replay Plus, and the launcher's own updates back on, since keeping vanilla ReSkate up to date is what they do.
        public void Vanilla()
        {
            CheckClosed();
            var release = LatestReSkate();
            if (release == null || release.Runtime == null || release.Launcher == null) throw new Exception("couldn't read ReSkate's newest release, so nothing was changed");
            var cache = Cache(ReSkateRepo, release.Tag);
            Log("Getting ReSkate " + release.Version + " from ReSkate's own release");
            var dllPath = Path.Combine(Folder, "ReSkate.dll");
            var launcherPath = Path.Combine(Folder, "ReSkateLauncher.exe");
            var jsonPath = Path.Combine(Folder, "launcher.json");
            var dll = Matches(dllPath, release.Runtime) ? null : Fetch(release.Runtime, cache, false);
            var launcher = Matches(launcherPath, release.Launcher) ? null : Fetch(release.Launcher, cache, false);
            if (dll != null)
            {
                var check = InspectFile(dll);
                if (check.Ours || check.HasStudio) throw new Exception("that download isn't vanilla ReSkate, so nothing was changed");
            }
            CheckClosed();
            var record = Begin();
            if (dll != null) Put(dll, dllPath);
            if (launcher != null) Put(launcher, launcherPath);
            if (!File.Exists(jsonPath) || Sha256Of(jsonPath) != Sha256Of(release.JsonFile)) Put(release.JsonFile, jsonPath);
            Updates(record, true);
            Stamp(record, null, null, release.Version, release.Tag, "ReSkate " + release.Version);
            Log("Done: vanilla ReSkate " + release.Version + " is in, and its launcher keeps it up to date again. Your Replay Plus presets and settings " +
                "are still there if you want it back.");
        }

        public bool CanRestore()
        {
            var record = ReadRecord();
            var originals = record == null ? null : Json.Obj(Json.Get(record, "originals"));
            if (originals != null && originals.Count > 0) return true;
            return File.Exists(Path.Combine(Folder, "ReSkate.dll.studio"));
        }

        // What there was before the updater's first change: the kept files back, "updates" as it was then.
        public void Restore()
        {
            CheckClosed();
            var record = ReadRecord();
            var originals = record == null ? null : Json.Obj(Json.Get(record, "originals"));
            if (originals == null || originals.Count == 0)
            {
                var studioCopy = Path.Combine(Folder, "ReSkate.dll.studio");
                if (!File.Exists(studioCopy)) throw new Exception("there's nothing from before Replay Plus to put back");
                originals = new Dictionary<string, object> { { "ReSkate.dll", "ReSkate.dll.studio" } };
            }
            foreach (var pair in originals)
            {
                var kept = Path.Combine(Folder, Convert.ToString(pair.Value, CultureInfo.InvariantCulture));
                if (!File.Exists(kept)) { Log("(" + pair.Value + " isn't there any more, skipped)"); continue; }
                Put(kept, Path.Combine(Folder, pair.Key));
            }
            foreach (var item in Json.List(record, "created"))
            {
                var name = Convert.ToString(item, CultureInfo.InvariantCulture);
                var path = Path.Combine(Folder, name);
                if (originals.ContainsKey(name) || !File.Exists(path) || name.IndexOfAny(new[] { '\\', '/' }) >= 0) continue;
                File.Copy(path, path + ".previous", true);
                File.Delete(path);
                Log("Took out " + name + " (it wasn't there before; it's kept as " + name + ".previous)");
            }
            // As it was. When it wasn't there at all (the launcher counts that as on): on for vanilla ReSkate, off for
            // anything else, which the launcher would swap for its own release.
            var before = record == null ? null : Json.Get(record, "updates_before");
            SetUpdates(Path.Combine(Folder, "ReSkateLauncher.settings.json"), before is bool ? (bool)before : Inspect().Vanilla);
            var recordPath = Path.Combine(Folder, RecordName);
            if (File.Exists(recordPath)) File.Delete(recordPath);
            Log("Put back what you had before Replay Plus.");
        }

        // ---- Files ---------------------------------------------------------------------------------------------------

        // The file a pin names, downloaded (or from --from) and checked; one downloaded before and whole is used again.
        string Fetch(Pin pin, string cache, bool ours)
        {
            var path = Path.Combine(cache, pin.Name);
            if (ours && !string.IsNullOrEmpty(FromFolder))
            {
                path = Path.Combine(FromFolder, pin.Name);
                Log("Taking " + pin.Name + " from " + FromFolder);
            }
            else if (!Matches(path, pin))
            {
                var label = pin.Name;
                Web.Download(pin.Url, path, pin.Size, (done, total) => Progress(done, total, label));
            }
            if (!Matches(path, pin))
            {
                if (path.StartsWith(cache, StringComparison.OrdinalIgnoreCase) && File.Exists(path)) File.Delete(path);
                throw new Exception(pin.Name + " didn't download right (its SHA-256 doesn't match), so nothing was changed. Try again");
            }
            Log(pin.Name + ": checked (SHA-256 " + pin.Sha256.Substring(0, 12) + "...)");
            return path;
        }

        static bool Matches(string path, Pin pin)
        {
            if (!File.Exists(path)) return false;
            if (pin.Size > 0 && new FileInfo(path).Length != pin.Size) return false;
            return Sha256Of(path) == pin.Sha256;
        }

        public static string Sha256Of(string path)
        {
            using (var stream = File.OpenRead(path)) return Hex(SHA256.Create().ComputeHash(stream));
        }

        static string Hex(byte[] bytes)
        {
            var sb = new StringBuilder(bytes.Length * 2);
            foreach (var b in bytes) sb.Append(b.ToString("x2"));
            return sb.ToString();
        }

        // The current file kept as <name>.previous, then the new one in its place.
        void Put(string source, string target)
        {
            if (File.Exists(target)) File.Copy(target, target + ".previous", true);
            File.Copy(source, target, true);
            Log("Installed " + Path.GetFileName(target));
        }

        // The record of what the updater did here; the first time, what was there before is kept first.
        Dictionary<string, object> Begin()
        {
            return ReadRecord() ?? KeepOriginals();
        }

        void Updates(Dictionary<string, object> record, bool value)
        {
            bool? before = SetUpdates(Path.Combine(Folder, "ReSkateLauncher.settings.json"), value);
            if (!record.ContainsKey("updates_before")) record["updates_before"] = before;
        }

        // The first time: what was there before, kept as <name>.before-replay-plus. A Replay Plus put in by hand earlier
        // points at the DLL kept from before it instead (ReSkate.dll.studio, as its README said, or ReSkate.dll.original),
        // when that one is not a Replay Plus too.
        Dictionary<string, object> KeepOriginals()
        {
            var dllPath = Path.Combine(Folder, "ReSkate.dll");
            var record = new Dictionary<string, object>();
            var originals = new Dictionary<string, object>();
            if (File.Exists(dllPath))
            {
                if (InspectFile(dllPath).Ours)
                    foreach (var name in new[] { "ReSkate.dll.studio", "ReSkate.dll.original" })
                    {
                        var kept = Path.Combine(Folder, name);
                        if (File.Exists(kept) && !InspectFile(kept).Ours) { originals["ReSkate.dll"] = name; break; }
                    }
                // (Else, or a Replay Plus with nothing from before it: the one that's here.)
                if (!originals.ContainsKey("ReSkate.dll")) { File.Copy(dllPath, dllPath + OriginalSuffix, true); originals["ReSkate.dll"] = "ReSkate.dll" + OriginalSuffix; }
            }
            foreach (var name in new[] { "ReSkateLauncher.exe", "launcher.json" })
            {
                var path = Path.Combine(Folder, name);
                if (!File.Exists(path)) continue;
                File.Copy(path, path + OriginalSuffix, true);
                originals[name] = name + OriginalSuffix;
            }
            record["originals"] = originals;
            // What wasn't there at all comes out again on Put back.
            var created = new List<object>();
            foreach (var name in new[] { "ReSkate.dll", "launcher.json" }) if (!File.Exists(Path.Combine(Folder, name))) created.Add(name);
            if (created.Count > 0) record["created"] = created.ToArray();
            var kept2 = new List<string>();
            foreach (var v in originals.Values) kept2.Add(Convert.ToString(v, CultureInfo.InvariantCulture));
            Log("Kept what you had before: " + (kept2.Count > 0 ? string.Join(", ", kept2.ToArray()) : "nothing to keep"));
            return record;
        }

        // What's in now, written down with when.
        void Stamp(Dictionary<string, object> record, string replayPlus, string studio, string reskate, string tag, string now)
        {
            record["installed"] = replayPlus;
            record["studio"] = studio;
            record["reskate"] = reskate;
            record["tag"] = tag;
            record["now"] = now;
            record["when"] = DateTime.Now.ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture);
            WriteRecord(record);
        }

        Dictionary<string, object> ReadRecord()
        {
            var path = Path.Combine(Folder, RecordName);
            try { return File.Exists(path) ? Json.Obj(Json.Parse(File.ReadAllText(path))) : null; }
            catch (Exception) { return null; }
        }

        void WriteRecord(Dictionary<string, object> record)
        {
            File.WriteAllText(Path.Combine(Folder, RecordName), Json.Write(record), new UTF8Encoding(false));
        }

        // "updates" in ReSkateLauncher.settings.json, changed in place (the rest of the file as it was). Without it the
        // launcher counts updates as on, and would put its own ReSkate.dll back. Returns what it was (null: not there).
        bool? SetUpdates(string path, bool value)
        {
            var word = value ? "true" : "false";
            if (!File.Exists(path))
            {
                File.WriteAllText(path, "{\r\n    \"updates\": " + word + "\r\n}\r\n", new UTF8Encoding(false));
                Log("Made ReSkateLauncher.settings.json with \"updates\": " + word);
                return null;
            }
            var raw = File.ReadAllBytes(path);
            bool bom = raw.Length >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF;
            var text = new UTF8Encoding(false).GetString(raw, bom ? 3 : 0, raw.Length - (bom ? 3 : 0));
            bool? before = null;
            var m = Regex.Match(text, "\"updates\"\\s*:\\s*(true|false)");
            if (m.Success)
            {
                before = m.Groups[1].Value == "true";
                if (before == value) return before;
                text = text.Substring(0, m.Groups[1].Index) + word + text.Substring(m.Groups[1].Index + m.Groups[1].Length);
            }
            else
            {
                int brace = text.IndexOf('{');
                if (brace < 0) throw new Exception("ReSkateLauncher.settings.json isn't readable");
                bool empty = text.Substring(brace + 1).Trim().StartsWith("}");
                string newline = text.Contains("\r\n") ? "\r\n" : "\n";
                text = text.Substring(0, brace + 1) + newline + "    \"updates\": " + word + (empty ? "" : ",") + text.Substring(brace + 1);
            }
            var bytes = new UTF8Encoding(false).GetBytes(text);
            using (var output = File.Create(path))
            {
                if (bom) output.Write(new byte[] { 0xEF, 0xBB, 0xBF }, 0, 3);
                output.Write(bytes, 0, bytes.Length);
            }
            Log("Set \"updates\" to " + word + " in ReSkateLauncher.settings.json" + (value ? " (so the launcher keeps ReSkate up to date)" : " (so the launcher doesn't swap the dll back)"));
            return before;
        }

        // ---- Building Replay Plus here -----------------------------------------------------------------------------
        //
        // The Animator Studio's source at that version's tag (a shallow clone, kept in the data folder's Build folder for
        // next time), the Replay Plus patches put on with git am --3way (first on the Studio they were made on, so every
        // file they change is there to merge from), then CMake's vs2022-x64 preset with the Studio's own release version
        // and Replay Plus's label, a Release build, and the Replay Shake and Replay Plus tests. The DLL is kept in Built,
        // so the same build is never made twice.

        public string Work { get { return Path.Combine(DataFolder, "Build"); } }
        Process child;
        readonly object childLock = new object();

        // Stops a build that's running (the window closing).
        public void Cancel()
        {
            lock (childLock)
            {
                if (child == null) return;
                try
                {
                    var kill = new ProcessStartInfo("taskkill.exe", "/T /F /PID " + child.Id) { UseShellExecute = false, CreateNoWindow = true };
                    using (var p = Process.Start(kill)) p.WaitForExit(10000);
                }
                catch (Exception) { }
            }
        }

        public Build BuildHere(string studio, Release release)
        {
            var tools = BuildTools.Find();
            if (!tools.Ready) throw new Exception("building Replay Plus here needs " + tools.MissingText);
            string plus;
            var patches = GetPatches(release, out plus);
            Directory.CreateDirectory(Work);
            var src = Path.Combine(Work, "studio");
            var logPath = Path.Combine(Work, "build.log");
            File.WriteAllText(logPath, "Replay Plus Updater " + Version + ": Replay Plus " + plus + " on Animator Studio " + studio + ", " +
                DateTime.Now.ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture) + Environment.NewLine);
            Log("Building Replay Plus " + plus + " for Animator Studio " + studio + " here (the whole log: " + logPath + ")");

            // The Studio's source at the tag, and the commit the patches were made on.
            Status("Getting Animator Studio " + studio + "'s source");
            Log("Getting Animator Studio " + studio + "'s source from GitHub");
            var git = tools.Git;
            if (!Directory.Exists(Path.Combine(src, ".git")))
            {
                if (Directory.Exists(src)) Directory.Delete(src, true);
                Directory.CreateDirectory(src);
                MustRun(git, "init -q", src, logPath, "git init");
                MustRun(git, "remote add origin https://github.com/" + StudioRepo, src, logPath, "git remote");
            }
            MustRun(git, "config user.name \"replay plus updater\"", src, logPath, "git config");
            MustRun(git, "config user.email updater@localhost", src, logPath, "git config");
            MustRun(git, "config core.longpaths true", src, logPath, "git config");
            Run(git, "am --abort", src, logPath, null);
            string tag = null;
            foreach (var t in new[] { studio, "v" + studio })
                if (Run(git, "fetch -q --depth 1 --no-tags origin +refs/tags/" + t + ":refs/tags/" + t, src, logPath, null) == 0) { tag = t; break; }
            if (tag == null) throw new BuildFailed("couldn't get Animator Studio " + studio + "'s source from GitHub (is " + studio + " one of its releases?)");
            var commit = Capture(git, "rev-parse refs/tags/" + tag + "^{commit}", src);
            string baseCommit = null;
            var baseLine = Regex.Match(File.ReadAllText(patches[0]), @"^base-commit: ([0-9a-f]{40})", RegexOptions.Multiline);
            if (baseLine.Success && Run(git, "fetch -q --depth 1 --no-tags origin " + baseLine.Groups[1].Value, src, logPath, null) == 0) baseCommit = baseLine.Groups[1].Value;

            Status("Putting Replay Plus on it");
            var am = "am -q --3way";
            foreach (var p in patches) am += " " + Quote(p);
            if (baseCommit != null && baseCommit != commit)
            {
                MustRun(git, "checkout -q -f --detach " + baseCommit, src, logPath, "git checkout");
                if (Run(git, am, src, logPath, null) != 0) { Run(git, "am --abort", src, logPath, null); Log("(the patches don't go onto the Studio they were made on, so they're tried as they are)"); }
            }
            MustRun(git, "checkout -q -f --detach " + commit, src, logPath, "git checkout");
            if (Run(git, am, src, logPath, null) != 0)
            {
                var clashes = Capture(git, "diff --name-only --diff-filter=U", src).Replace("\r", "").Trim().Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries);
                Run(git, "am --abort", src, logPath, null);
                var names = new List<string>();
                foreach (var c in clashes) names.Add(Path.GetFileName(c));
                throw new PatchesDontFit("Replay Plus " + plus + " doesn't go onto Animator Studio " + studio + " by itself" +
                    (names.Count > 0 ? " (it clashes in " + string.Join(", ", names.ToArray()) + ")" : "") + ". It needs porting to that Studio by hand first");
            }

            // The Studio's own release version, from its CMake preset: "2.1.5+reskate.20261116" and "20261116".
            string reskate, version;
            ReadIdentity(Path.Combine(src, "CMakePresets.json"), studio, out reskate, out version);
            var modVersion = plus + "+studio-" + studio + ".reskate-" + reskate;
            var key = commit + " " + modVersion;
            foreach (var p in patches) key += " " + Sha256Of(p);
            var built = Path.Combine(DataFolder, "Built");
            Directory.CreateDirectory(built);
            var keep = Path.Combine(built, "ReSkate-studio-" + studio + "-replay-plus-" + plus + ".dll");
            if (File.Exists(keep) && File.Exists(keep + ".key") && File.ReadAllText(keep + ".key").Trim() == key.Trim() && InspectFile(keep).Ours)
            {
                Log("Using the one built here before (" + keep + ")");
                return new Build { ReplayPlus = plus, Studio = studio, StudioTag = tag, StudioCommit = commit, ReSkate = reskate, File = keep };
            }

            var buildDir = Path.Combine(src, "build", "vs2022-x64");
            var free = Native.FreeBytes(Work);
            if (free >= 0 && free < BuildSpace && !Directory.Exists(buildDir))
                throw new BuildFailed(string.Format(CultureInfo.InvariantCulture, "building needs about 4 GB of free space on {0} (there's {1:0.0} GB)", Path.GetPathRoot(Work), free / 1073741824.0));
            var stamp = Path.Combine(Work, "configured.txt");
            if (Directory.Exists(buildDir) && (!File.Exists(stamp) || File.ReadAllText(stamp).Trim() != key.Trim()))
            {
                Status("Clearing out the last build");
                Directory.Delete(buildDir, true);
            }
            if (File.Exists(stamp)) File.Delete(stamp);

            Status("Setting up the build");
            Log("Setting up the build (CMake, Visual Studio 2022's C++ tools)");
            var configure = "--preset vs2022-x64 -DDINGOSDK_BUILD_REPLAY_SHAKE_TESTS=ON -DDINGOSDK_REPLAY_PLUS_TEST_INBOX=OFF" +
                " " + Quote("-DCMAKE_GENERATOR_INSTANCE=" + tools.VisualStudio) +
                " " + Quote("-DDINGOSDK_VERSION=" + version) + " -DDINGOSDK_MOD_ID=" + ModId +
                " " + Quote("-DDINGOSDK_MOD_NAME=Animator Studio " + studio + " + Replay Plus") + " " + Quote("-DDINGOSDK_MOD_VERSION=" + modVersion) +
                " -DDINGOSDK_CRASH_REPORT_URL= -DDINGOSDK_CARGO=OFF";
            if (Run(tools.CMake, configure, src, logPath, null) != 0) throw new BuildFailed("setting up the build didn't work" + LastLines(logPath));
            File.WriteAllText(stamp, key);

            int jobs = Jobs > 0 ? Jobs : Math.Max(1, Environment.ProcessorCount / 2);
            // How many files there are to compile: every project's ClCompile items (MSBuild names each as it goes).
            int files = 0;
            foreach (var project in Directory.GetFiles(buildDir, "*.vcxproj", SearchOption.AllDirectories))
                files += Regex.Matches(File.ReadAllText(project), "<ClCompile Include=").Count;
            Log("Building " + (files > 0 ? files + " files, " : "") + jobs + " at a time (this takes a while, 10 to 30 minutes)");
            var started = DateTime.Now;
            int compiled = 0;
            Action<string> line = text =>
            {
                if (!Regex.IsMatch(text, @"^\s+[\w.\-]+\.(cpp|cc|c|cxx)$")) return;
                ++compiled;
                var minutes = (DateTime.Now - started).TotalMinutes;
                if (files > 0) Steps(Math.Min(compiled, files), files, string.Format(CultureInfo.InvariantCulture, "Building: {0} of {1} files, {2:0} min", Math.Min(compiled, files), files, minutes));
                else Status(string.Format(CultureInfo.InvariantCulture, "Building: {0} files, {1:0} min", compiled, minutes));
            };
            Status("Building");
            if (Run(tools.CMake, "--build --preset release --parallel " + jobs, src, logPath, line) != 0) throw new BuildFailed("the build didn't work" + LastLines(logPath));
            Log(string.Format(CultureInfo.InvariantCulture, "Built in {0:0} minutes", (DateTime.Now - started).TotalMinutes));

            Status("Testing");
            Log("Running the Replay Shake and Replay Plus tests");
            var testOut = new StringBuilder();
            int tested = Run(tools.CTest, "--test-dir " + Quote(buildDir) + " -C Release -R \"replay_(shake|plus)\" --output-on-failure", src, logPath, t => testOut.AppendLine(t));
            if (tested != 0 || !Regex.IsMatch(testOut.ToString(), @"100% tests passed, 0 tests failed out of [1-9]"))
                throw new BuildFailed("the tests didn't pass" + LastLines(logPath));
            Log("Tests passed");

            var dll = Path.Combine(buildDir, "Release", "ReSkate.dll");
            var made = File.Exists(dll) ? InspectFile(dll) : null;
            if (made == null || !made.Ours || made.ReplayPlus != plus || Compare(made.Studio, studio) != 0)
                throw new BuildFailed("the build didn't make the ReSkate.dll it should have");
            var notes = Path.Combine(buildDir, "replay_shake_notes.txt");
            if (File.Exists(notes) && File.ReadAllText(notes).Trim().Length > 0)
                Log("(the build left notes: " + File.ReadAllText(notes).Trim().Replace("\r", "").Replace("\n", " / ") + ")");
            File.Copy(dll, keep, true);
            File.WriteAllText(keep + ".key", key);
            Log("Built " + made.Describe + " (SHA-256 " + made.Sha256.Substring(0, 12) + "...)");
            return new Build { ReplayPlus = plus, Studio = studio, StudioTag = tag, StudioCommit = commit, ReSkate = reskate, File = keep };
        }

        // The build folder's gigabytes back, once the DLL is kept.
        public void CleanBuild()
        {
            var buildDir = Path.Combine(Work, "studio", "build");
            try
            {
                if (Directory.Exists(buildDir)) { Directory.Delete(buildDir, true); Log("Cleared the build folder (the source stays for next time)"); }
                var stamp = Path.Combine(Work, "configured.txt");
                if (File.Exists(stamp)) File.Delete(stamp);
            }
            catch (Exception e) { Log("(couldn't clear the build folder: " + e.Message + ")"); }
        }

        // The patches, in the order they go on: from --patches (with its patches.json), else the release's, checked.
        List<string> GetPatches(Release release, out string plus)
        {
            var files = new List<string>();
            if (!string.IsNullOrEmpty(PatchesFolder))
            {
                var info = Json.Obj(Json.Parse(File.ReadAllText(Path.Combine(PatchesFolder, "patches.json"))));
                plus = Json.Str(info, "replay_plus");
                foreach (var name in Json.List(info, "patches")) files.Add(Path.Combine(PatchesFolder, Convert.ToString(name, CultureInfo.InvariantCulture)));
            }
            else
            {
                if (release == null || release.Patches.Count == 0) throw new Exception("this Replay Plus release has no patches to build with");
                plus = release.ReplayPlus;
                foreach (var pin in release.Patches) files.Add(Fetch(pin, Cache(pin.Repo, pin.Tag), true));
            }
            if (string.IsNullOrEmpty(plus) || files.Count == 0) throw new Exception("the patches don't say which Replay Plus they are");
            foreach (var f in files) if (!File.Exists(f)) throw new Exception(Path.GetFileName(f) + " isn't there");
            return files;
        }

        static void ReadIdentity(string presets, string studio, out string reskate, out string version)
        {
            reskate = version = null;
            var d = Json.Obj(Json.Parse(File.ReadAllText(presets)));
            foreach (var item in Json.List(d, "configurePresets"))
            {
                var cache = Json.Obj(Json.Get(Json.Obj(item), "cacheVariables"));
                if (Json.Str(cache, "DINGOSDK_MOD_ID") != "AnimatorStudio") continue;
                var m = Regex.Match(Json.Str(cache, "DINGOSDK_MOD_VERSION") ?? "", @"^(\d+(?:\.\d+)+)\+reskate\.([0-9A-Za-z.\-]+)$");
                if (!m.Success) continue;
                if (Compare(m.Groups[1].Value, studio) != 0)
                    throw new BuildFailed("Animator Studio " + studio + "'s source says it's " + m.Groups[1].Value + ", so nothing was built");
                reskate = m.Groups[2].Value;
                version = Json.Str(cache, "DINGOSDK_VERSION") ?? reskate;
                return;
            }
            throw new BuildFailed("couldn't read Animator Studio " + studio + "'s version from its source");
        }

        // A program run to the end, its output into the log file (and each line to `line`); its exit code.
        int Run(string exe, string args, string dir, string logPath, Action<string> line)
        {
            var info = new ProcessStartInfo(exe, args)
            {
                WorkingDirectory = dir, UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
            };
            // (MSBuild otherwise leaves its worker processes running for a while after the build.)
            info.EnvironmentVariables["MSBUILDDISABLENODEREUSE"] = "1";
            using (var log = new StreamWriter(logPath, true, new UTF8Encoding(false)))
            {
                log.WriteLine("> " + Path.GetFileName(exe) + " " + args);
                log.Flush();
                var gate = new object();
                DataReceivedEventHandler take = (s, e) =>
                {
                    if (e.Data == null) return;
                    lock (gate)
                    {
                        log.WriteLine(e.Data);
                        if (line != null) line(e.Data);
                    }
                };
                using (var p = new Process { StartInfo = info })
                {
                    p.OutputDataReceived += take;
                    p.ErrorDataReceived += take;
                    p.Start();
                    lock (childLock) child = p;
                    try
                    {
                        p.BeginOutputReadLine();
                        p.BeginErrorReadLine();
                        p.WaitForExit();
                    }
                    finally { lock (childLock) child = null; }
                    lock (gate) log.WriteLine("(exit " + p.ExitCode + ")");
                    return p.ExitCode;
                }
            }
        }

        void MustRun(string exe, string args, string dir, string logPath, string what)
        {
            if (Run(exe, args, dir, logPath, null) != 0) throw new BuildFailed(what + " didn't work" + LastLines(logPath));
        }

        static string Capture(string exe, string args, string dir)
        {
            var info = new ProcessStartInfo(exe, args) { WorkingDirectory = dir, UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, StandardOutputEncoding = Encoding.UTF8 };
            using (var p = Process.Start(info))
            {
                var text = p.StandardOutput.ReadToEnd();
                p.WaitForExit();
                return text.Trim();
            }
        }

        static string LastLines(string logPath)
        {
            try
            {
                var lines = File.ReadAllLines(logPath);
                var useful = new List<string>();
                for (int i = lines.Length - 1; i >= 0 && useful.Count < 4; --i)
                    if (Regex.IsMatch(lines[i], @"error|fatal|failed|conflict", RegexOptions.IgnoreCase) && !lines[i].StartsWith(">")) useful.Insert(0, lines[i].Trim());
                return useful.Count > 0 ? ": " + string.Join(" / ", useful.ToArray()) : "";
            }
            catch (Exception) { return ""; }
        }

        static string Quote(string arg)
        {
            if (arg.Length > 0 && arg.IndexOfAny(new[] { ' ', '\t', '"' }) < 0) return arg;
            var sb = new StringBuilder("\"");
            int slashes = 0;
            foreach (var c in arg)
            {
                if (c == '\\') { ++slashes; continue; }
                if (c == '"') sb.Append('\\', slashes * 2 + 1).Append('"');
                else sb.Append('\\', slashes).Append(c);
                slashes = 0;
            }
            return sb.Append('\\', slashes * 2).Append('"').ToString();
        }

        // --build-only: a build for the release workflow; the DLL, its .sha256 and its entry for replay-plus-builds.json.
        public void BuildOnly(string studio, string outDir)
        {
            Release release = string.IsNullOrEmpty(PatchesFolder) ? Latest() : null;
            var build = BuildHere(studio, release);
            Directory.CreateDirectory(outDir);
            var name = "ReSkate-studio-" + studio + ".dll";
            var dll = Path.Combine(outDir, name);
            File.Copy(build.File, dll, true);
            var sha = Sha256Of(dll);
            File.WriteAllText(dll + ".sha256", sha + "  " + name + "\n", new UTF8Encoding(false));
            var entry = new Dictionary<string, object>
            {
                { "replay_plus", build.ReplayPlus }, { "studio", build.Studio }, { "studio_tag", build.StudioTag },
                { "studio_commit", build.StudioCommit }, { "reskate", build.ReSkate },
                { "dll", new Dictionary<string, object> { { "name", name }, { "sha256", sha }, { "size", new FileInfo(dll).Length } } },
            };
            var reskate = ReSkateAt(build.ReSkate, false);
            if (reskate != null && reskate.Launcher != null)
            {
                entry["launcher"] = PinJson(reskate.Launcher);
                entry["launcher_json"] = PinJson(new Pin { Repo = ReSkateRepo, Tag = reskate.Tag, Name = "launcher.json", Sha256 = Sha256Of(reskate.JsonFile), Size = new FileInfo(reskate.JsonFile).Length });
                entry["game_build"] = Json.Str(Json.Obj(Json.Get(Json.Obj(Json.Parse(File.ReadAllText(reskate.JsonFile))), "game")), "build_id");
            }
            else Log("(couldn't find ReSkate " + build.ReSkate + "'s release, so the entry names no launcher)");
            File.WriteAllText(Path.Combine(outDir, "build-" + studio + ".json"), Json.Write(entry), new UTF8Encoding(false));
            Log("Done: " + dll + " (" + sha + ")");
        }

        static Dictionary<string, object> PinJson(Pin pin)
        {
            return new Dictionary<string, object> { { "repo", pin.Repo }, { "tag", pin.Tag }, { "name", pin.Name }, { "sha256", pin.Sha256 }, { "size", pin.Size } };
        }
    }

    // What the window shows: looked up on a worker thread, shown on the window's.
    class State
    {
        public Installed Installed;
        public Release Release;
        public ReSkateRelease ReSkate;
        public string Studio, Problem, Error;
    }

    class MainForm : Form
    {
        readonly Updater updater;
        readonly Label folderValue = new Label(), installedValue = new Label(), latestValue = new Label(), notice = new Label(), status = new Label();
        readonly Button installButton = new Button(), plusButton = new Button(), studioButton = new Button(), removeButton = new Button(),
            vanillaButton = new Button(), restoreButton = new Button(), folderButton = new Button(), openButton = new Button();
        readonly ProgressBar progress = new ProgressBar();
        readonly TextBox log = new TextBox();
        State state;
        bool busy;

        public MainForm(Updater updater)
        {
            this.updater = updater;
            Text = "Replay Plus Updater " + Updater.Version;
            Font = new Font("Segoe UI", 9f);
            AutoScaleMode = AutoScaleMode.Dpi;
            ClientSize = new Size(760, 460);
            MinimumSize = new Size(680, 400);
            StartPosition = FormStartPosition.CenterScreen;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch (Exception) { }

            var table = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(12), ColumnCount = 3, RowCount = 8 };
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            for (int i = 0; i < 7; ++i) table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            table.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

            AddRow(table, 0, "ReSkate folder", folderValue, folderButton, "Change...");
            AddRow(table, 1, "You have", installedValue, null, null);
            AddRow(table, 2, "Newest", latestValue, null, null);
            notice.AutoSize = true;
            notice.MaximumSize = new Size(720, 0);
            notice.ForeColor = Color.FromArgb(150, 90, 0);
            notice.Margin = new Padding(3, 6, 3, 6);
            table.Controls.Add(notice, 0, 3);
            table.SetColumnSpan(notice, 3);

            var buttons = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, Margin = new Padding(0, 4, 0, 0) };
            foreach (var b in new[] { installButton, plusButton, studioButton })
            {
                b.AutoSize = true;
                b.Padding = new Padding(8, 2, 8, 2);
                buttons.Controls.Add(b);
            }
            table.Controls.Add(buttons, 0, 4);
            table.SetColumnSpan(buttons, 3);
            var more = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, Margin = new Padding(0, 0, 0, 4) };
            foreach (var b in new[] { removeButton, vanillaButton, restoreButton, openButton })
            {
                b.AutoSize = true;
                b.Padding = new Padding(8, 2, 8, 2);
                more.Controls.Add(b);
            }
            installButton.Text = "Install Replay Plus + Animator Studio";
            plusButton.Text = "Update Replay Plus";
            studioButton.Text = "Update Animator Studio";
            removeButton.Text = "Remove Replay Plus";
            vanillaButton.Text = "Vanilla ReSkate";
            restoreButton.Text = "Put back what I had before";
            openButton.Text = "Open the folder";
            table.Controls.Add(more, 0, 5);
            table.SetColumnSpan(more, 3);

            var bar = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1, AutoSize = true, Margin = new Padding(0, 0, 0, 6) };
            bar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 45));
            bar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 55));
            progress.Dock = DockStyle.Fill;
            progress.Height = 16;
            status.AutoSize = true;
            status.ForeColor = SystemColors.GrayText;
            status.Margin = new Padding(8, 1, 3, 0);
            bar.Controls.Add(progress, 0, 0);
            bar.Controls.Add(status, 1, 0);
            table.Controls.Add(bar, 0, 6);
            table.SetColumnSpan(bar, 3);

            log.Multiline = true;
            log.ReadOnly = true;
            log.ScrollBars = ScrollBars.Vertical;
            log.Dock = DockStyle.Fill;
            log.BackColor = SystemColors.Window;
            table.Controls.Add(log, 0, 7);
            table.SetColumnSpan(log, 3);
            Controls.Add(table);

            updater.Log = text => Ui(() => { log.AppendText(text + Environment.NewLine); });
            updater.Progress = (done, total, name) => Ui(() =>
            {
                progress.Style = ProgressBarStyle.Continuous;
                progress.Maximum = 1000;
                progress.Value = total > 0 ? (int)Math.Min(1000, done * 1000 / total) : 0;
                status.Text = string.Format(CultureInfo.InvariantCulture, "{0} {1:0.0} / {2:0.0} MB", name, done / 1048576.0, total / 1048576.0);
            });
            updater.Status = text => Ui(() =>
            {
                if (progress.Style != ProgressBarStyle.Marquee) { progress.Style = ProgressBarStyle.Marquee; progress.MarqueeAnimationSpeed = 30; }
                status.Text = text;
            });
            updater.Steps = (done, total, text) => Ui(() =>
            {
                progress.Style = ProgressBarStyle.Continuous;
                progress.Maximum = 1000;
                progress.Value = total > 0 ? Math.Min(1000, done * 1000 / total) : 0;
                status.Text = text;
            });

            folderButton.Click += (s, e) => PickFolder();
            openButton.Click += (s, e) => { if (Updater.IsReSkateFolder(updater.Folder)) Process.Start("explorer.exe", "\"" + updater.Folder + "\""); };
            installButton.Click += (s, e) => Act(1);
            removeButton.Click += (s, e) => Act(2);
            plusButton.Click += (s, e) => Act(3);
            studioButton.Click += (s, e) => Act(4);
            vanillaButton.Click += (s, e) => Act(5);
            restoreButton.Click += (s, e) =>
            {
                if (Ask("Put back the ReSkate.dll and launcher you had before the updater changed anything?"))
                    Work(() => { updater.Restore(); });
            };
            Shown += (s, e) =>
            {
                if (!Updater.IsReSkateFolder(updater.Folder)) PickFolder();
                else Check();
            };
            FormClosing += (s, e) =>
            {
                if (!busy) return;
                if (MessageBox.Show(this, "The updater is still working. Stop it and close?", "Replay Plus Updater", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) e.Cancel = true;
                else updater.Cancel();
            };
        }

        bool Ask(string question)
        {
            return MessageBox.Show(this, question, "Replay Plus Updater", MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes;
        }

        const string BuildNote = "\n\nThere's no ready-made Replay Plus for that Studio, so the updater builds it here from the Studio's source. That takes " +
            "10 to 30 minutes and about 4 GB of free space while it builds.";

        // The five buttons: asks, then does it.
        void Act(int what)
        {
            var s = state;
            if (s == null) return;
            var installed = s.Installed;
            var release = s.Release;
            if (what == 1)
            {
                var choice = updater.ChooseFor(release, s.Studio ?? (release != null && release.Newest != null ? release.Newest.Studio : null));
                var studio = choice.Possible ? choice.Studio : release != null && release.Newest != null ? release.Newest.Studio : s.Studio;
                if (Ask("Get Animator Studio " + studio + " with Replay Plus on it, and the ReSkate launcher it goes with?" + (choice.BuildHere ? BuildNote : "")))
                    Work(() => { updater.InstallWithStudio(release); updater.CleanBuild(); });
            }
            else if (what == 2)
            {
                if (Ask("Take Replay Plus out and put the plain Animator Studio " + installed.Studio + " back in? Your presets and settings stay."))
                    Work(() => { updater.RemoveReplayPlus(); });
            }
            else if (what == 3)
            {
                var choice = updater.ChooseFor(release, installed.Studio);
                var question = installed.Ours ? "Get the newest Replay Plus for your Animator Studio " + installed.Studio + "?"
                    : "Put Replay Plus on your Animator Studio " + installed.Studio + "? The Studio stays as it is.";
                if (Ask(question + (choice.BuildHere ? BuildNote : "")))
                    Work(() => { updater.UpdateReplayPlus(release); updater.CleanBuild(); });
            }
            else if (what == 4)
            {
                if (installed.Ours)
                {
                    var choice = updater.ChooseFor(release, s.Studio);
                    if (Ask("Move to Animator Studio " + s.Studio + " and keep Replay Plus on it?" + (choice.BuildHere ? BuildNote : "")))
                        Work(() => { updater.UpdateStudio(release); updater.CleanBuild(); });
                }
                else if (Ask("Get Animator Studio " + (s.Studio ?? "") + " and the ReSkate launcher it goes with?" + (installed.HasStudio ? "" : " (without Replay Plus)")))
                    Work(() => { updater.UpdateStudio(release); });
            }
            else if (what == 5)
            {
                var version = s.ReSkate != null ? " " + s.ReSkate.Version : "";
                if (Ask("Go back to vanilla ReSkate" + version + "? Replay Plus and the Animator Studio come out, ReSkate's own ReSkate.dll and launcher go in, " +
                        "and the launcher's own updates go back on. Your presets and settings stay, and Put back what I had before still works."))
                    Work(() => { updater.Vanilla(); });
            }
        }

        static void AddRow(TableLayoutPanel table, int row, string label, Label value, Button button, string buttonText)
        {
            table.Controls.Add(new Label { Text = label, AutoSize = true, Margin = new Padding(3, 7, 12, 3), ForeColor = SystemColors.GrayText }, 0, row);
            value.AutoSize = true;
            value.Margin = new Padding(3, 7, 3, 3);
            value.Font = new Font("Segoe UI", 9f, FontStyle.Bold);
            table.Controls.Add(value, 1, row);
            if (button != null)
            {
                button.Text = buttonText;
                button.AutoSize = true;
                table.Controls.Add(button, 2, row);
            }
        }

        void Ui(Action action)
        {
            if (IsDisposed) return;
            if (InvokeRequired) { try { BeginInvoke(action); } catch (InvalidOperationException) { } }
            else action();
        }

        void PickFolder()
        {
            using (var dialog = new OpenFileDialog
            {
                Title = "Find ReSkateLauncher.exe (in the folder you start ReSkate from)",
                Filter = "ReSkate launcher|ReSkateLauncher.exe",
                CheckFileExists = true,
            })
            {
                if (dialog.ShowDialog(this) != DialogResult.OK) { if (!Updater.IsReSkateFolder(updater.Folder)) Close(); return; }
                updater.Folder = Path.GetDirectoryName(dialog.FileName);
                Program.SaveFolder(updater.Folder);
                Check();
            }
        }

        void Check()
        {
            folderValue.Text = updater.Folder;
            Work(null);
        }

        // Runs `action` on its own thread, then looks again at what is there and what is out.
        void Work(Action action)
        {
            if (busy) return;
            busy = true;
            SetButtons(false);
            progress.Style = ProgressBarStyle.Continuous;
            progress.Value = 0;
            status.Text = action == null ? "Checking" : "";
            new Thread(() =>
            {
                string error = null;
                try { if (action != null) action(); }
                catch (UnauthorizedAccessException) { error = "Windows won't let the updater write to that folder. Try running it as administrator."; }
                catch (IOException e) { error = "A file is busy or can't be written (" + e.Message + "). Make sure skate. and the launcher are closed."; }
                catch (WebException e) { error = "Couldn't reach GitHub (" + e.Message + ")."; }
                catch (PatchesDontFit e) { error = Program.Sentence(e.Message) + " Nothing was changed."; }
                catch (BuildFailed e) { error = Program.Sentence(e.Message) + " Nothing was changed."; }
                catch (Exception e) { error = Program.Sentence(e.Message); }
                if (error != null) updater.Log("Didn't work: " + error);

                var s = new State { Error = error };
                try { s.Installed = updater.Inspect(); } catch (Exception e) { s.Problem = "Couldn't read ReSkate.dll: " + e.Message; }
                try { s.Release = updater.Latest(); }
                catch (Exception e) { s.Problem = "Couldn't check GitHub for the newest Replay Plus: " + e.Message; }
                s.Studio = updater.LatestStudio();
                try { s.ReSkate = updater.LatestReSkate(); } catch (Exception) { }
                updater.RefreshTools();
                Ui(() => ShowState(s));
            }) { IsBackground = true }.Start();
        }

        void ShowState(State s)
        {
            busy = false;
            state = s;
            var installed = s.Installed;
            var release = s.Release;
            bool ours = installed != null && installed.Ours, hasStudio = installed != null && installed.HasStudio;
            installedValue.Text = installed == null ? "?" : installed.Describe;
            var newest = new List<string>();
            if (release != null) newest.Add("Replay Plus " + release.ReplayPlus + (release.Prerelease ? " (pre-release)" : ""));
            if (s.Studio != null) newest.Add("Animator Studio " + s.Studio);
            if (s.ReSkate != null) newest.Add("ReSkate " + s.ReSkate.Version);
            latestValue.Text = newest.Count > 0 ? string.Join("  /  ", newest.ToArray()) : "?";

            // What each button would do now.
            var forLatest = updater.ChooseFor(release, s.Studio);
            var forMine = hasStudio ? updater.ChooseFor(release, installed.Studio) : null;
            bool studioNewer = s.Studio != null && hasStudio && Updater.Compare(s.Studio, installed.Studio) > 0;
            bool vanillaNow = installed != null && installed.Vanilla && s.ReSkate != null && s.ReSkate.Runtime != null && installed.Sha256 == s.ReSkate.Runtime.Sha256;

            var notes = new List<string>();
            if (s.Problem != null) notes.Add(s.Problem);
            if (ours && s.Studio != null && studioNewer)
                notes.Add(forLatest.Possible ? "Animator Studio " + s.Studio + " is out. Update Animator Studio moves you to it and keeps Replay Plus" + (forLatest.BuildHere ? " (it builds Replay Plus for it here first)." : ".")
                    : "Animator Studio " + s.Studio + " is out, but " + forLatest.Why + ", so you stay on " + installed.Studio + " for now.");
            if (!ours && hasStudio && studioNewer) notes.Add("Animator Studio " + s.Studio + " is out (you have " + installed.Studio + ").");
            if (!ours && release != null && s.Studio != null && !forLatest.Possible && release.Newest != null)
                notes.Add(Program.Sentence(forLatest.Why) + " Install puts it on Animator Studio " + release.Newest.Studio + ", the newest one it's ready for.");
            else if (!ours && forLatest.BuildHere)
                notes.Add("There's no ready-made Replay Plus for Animator Studio " + s.Studio + " yet, so Install builds it here (10 to 30 minutes)" +
                    (release != null && release.Newest != null ? ", or puts it on Studio " + release.Newest.Studio + " if it doesn't fit." : "."));
            if (hasStudio && forMine != null && !forMine.Possible && (ours || s.Studio == null || Updater.Compare(s.Studio, installed.Studio) != 0))
                notes.Add(Program.Sentence(forMine.Why) + (ours ? "" : " (Install gets you the newest Studio with Replay Plus instead.)"));
            if (hasStudio && forMine != null && forMine.BuildHere && (ours || s.Studio == null || Updater.Compare(s.Studio, installed.Studio) != 0))
                notes.Add("There's no ready-made Replay Plus " + (release != null ? release.ReplayPlus + " " : "") + "for Animator Studio " + installed.Studio + ", so " +
                    (ours ? "Update" : "Add") + " Replay Plus builds it here (10 to 30 minutes).");
            if (release != null && !string.IsNullOrEmpty(release.UpdaterVersion) && Updater.Compare(release.UpdaterVersion, Updater.Version) > 0)
                notes.Add("There's a newer updater (" + release.UpdaterVersion + ") on the releases page.");
            notice.Text = string.Join(Environment.NewLine, notes.ToArray());

            // Which buttons, and what they say.
            installButton.Visible = !ours;
            removeButton.Visible = ours;
            plusButton.Visible = ours || (hasStudio && (s.Studio == null || Updater.Compare(s.Studio, installed.Studio) != 0));
            // The Replay Plus that Update Replay Plus would put in, against the one that's in.
            string offered = forMine == null ? null : forMine.Build != null ? forMine.Build.ReplayPlus : forMine.BuildHere && release != null ? release.ReplayPlus : null;
            bool plusNewer = ours && offered != null && Updater.Compare(offered, installed.ReplayPlus) > 0;
            bool plusOlder = ours && offered != null && Updater.Compare(offered, installed.ReplayPlus) < 0;
            plusButton.Text = !ours ? "Add Replay Plus to Studio " + (installed != null ? installed.Studio : "") : plusNewer ? "Update Replay Plus" : "Reinstall Replay Plus";
            studioButton.Visible = installed != null;
            studioButton.Text = hasStudio ? "Update Animator Studio" : "Get Animator Studio";
            vanillaButton.Visible = installed != null;
            SetButtons(true);
            installButton.Enabled = release != null && (forLatest.Possible || release.Newest != null);
            plusButton.Enabled = forMine != null && forMine.Possible && !plusOlder;
            studioButton.Enabled = s.Studio != null && (!hasStudio || studioNewer) && (!ours || forLatest.Possible);
            removeButton.Enabled = ours;
            vanillaButton.Enabled = installed != null && !vanillaNow;
            restoreButton.Enabled = updater.CanRestore();
            progress.Style = ProgressBarStyle.Continuous;
            if (s.Error != null || release == null) progress.Value = 0;
            status.Text = "";
            if (installed != null && release != null && s.Error == null && log.TextLength == 0)
                updater.Log(plusNewer || (ours && studioNewer && forLatest.Possible) ? "There's an update." : ours ? "You're up to date." : "Ready.");
        }

        void SetButtons(bool on)
        {
            foreach (var b in new[] { installButton, plusButton, studioButton, removeButton, vanillaButton, restoreButton, folderButton })
                b.Enabled = on;
        }
    }

    static class Program
    {
        static string LogPath { get { return Path.Combine(Updater.DataFolder, "updater.log"); } }
        static string SavedPath { get { return Path.Combine(Updater.DataFolder, "updater-folder.txt"); } }

        public static void SaveFolder(string folder)
        {
            try { Directory.CreateDirectory(Updater.DataFolder); File.WriteAllText(SavedPath, folder); } catch (Exception) { }
        }

        static string SavedFolder()
        {
            try { return File.Exists(SavedPath) ? File.ReadAllText(SavedPath).Trim() : null; } catch (Exception) { return null; }
        }

        public static string Sentence(string text)
        {
            if (string.IsNullOrEmpty(text)) return "Something went wrong.";
            text = char.ToUpperInvariant(text[0]) + text.Substring(1);
            return text.EndsWith(".") ? text : text + ".";
        }

        static readonly object logLock = new object();
        static void WriteLog(string line)
        {
            try
            {
                lock (logLock)
                {
                    Directory.CreateDirectory(Updater.DataFolder);
                    File.AppendAllText(LogPath, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture) + "  " + line + Environment.NewLine);
                }
            }
            catch (Exception) { }
        }

        [STAThread]
        static int Main(string[] args)
        {
            ServicePointManager.SecurityProtocol = (SecurityProtocolType)3072; // TLS 1.2
            var updater = new Updater();
            string action = null, buildOnly = null, outDir = null;
            for (int i = 0; i < args.Length; ++i)
            {
                var a = args[i].ToLowerInvariant();
                bool more = i + 1 < args.Length;
                if (a == "--folder" && more) updater.Folder = args[++i];
                else if (a == "--from" && more) updater.FromFolder = args[++i];
                else if (a == "--data" && more) Updater.DataFolder = args[++i];
                else if (a == "--patches" && more) updater.PatchesFolder = args[++i];
                else if (a == "--jobs" && more) int.TryParse(args[++i], out updater.Jobs);
                else if (a == "--build-only" && more) buildOnly = args[++i];
                else if (a == "--out" && more) outDir = args[++i];
                else if (a == "--prerelease") updater.Prerelease = true;
                else if (a == "--build-here") updater.ForceBuild = true;
                else if (a == "--no-build") updater.NoBuild = true;
                else if (a == "--install" || a == "--update-plus" || a == "--update-studio" || a == "--uninstall" || a == "--vanilla" || a == "--restore" || a == "--status") action = a;
            }
            if (string.IsNullOrEmpty(updater.Folder))
            {
                var saved = SavedFolder();
                var here = Path.GetDirectoryName(Application.ExecutablePath);
                updater.Folder = Updater.IsReSkateFolder(saved) ? saved : Updater.IsReSkateFolder(here) ? here : null;
            }
            WriteLog("---- Replay Plus Updater " + Updater.Version + " " + string.Join(" ", args) + ", folder " + (updater.Folder ?? "(not found yet)"));

            if (action != null || buildOnly != null)
            {
                updater.Log = WriteLog;
                updater.Progress = delegate { };
                string last = null;
                updater.Status = text => { var head = Regex.Replace(text, @"[:,].*$", ""); if (head != last) { last = head; WriteLog("(" + text + ")"); } };
                int tenth = -1;
                updater.Steps = (done, total, text) => { int t = total > 0 ? done * 10 / total : 0; if (t != tenth) { tenth = t; WriteLog("(" + text + ")"); } };
                try
                {
                    if (buildOnly != null)
                    {
                        if (string.IsNullOrEmpty(outDir)) throw new Exception("--build-only needs --out <folder>");
                        updater.BuildOnly(buildOnly, outDir);
                        return 0;
                    }
                    if (!Updater.IsReSkateFolder(updater.Folder)) throw new Exception("no ReSkate folder (pass --folder)");
                    if (action == "--status") { updater.Report(); return 0; }
                    if (action == "--install") updater.InstallWithStudio(updater.Latest());
                    else if (action == "--update-plus") updater.UpdateReplayPlus(updater.Latest());
                    else if (action == "--update-studio") updater.UpdateStudio(updater.Inspect().Ours ? updater.Latest() : null);
                    else if (action == "--uninstall") updater.RemoveReplayPlus();
                    else if (action == "--vanilla") updater.Vanilla();
                    else updater.Restore();
                    if (action != "--restore" && action != "--uninstall" && action != "--vanilla") updater.CleanBuild();
                    return 0;
                }
                catch (PatchesDontFit e) { WriteLog("Didn't work: " + Sentence(e.Message) + " Nothing was changed."); return 2; }
                catch (BuildFailed e) { WriteLog("Didn't work: " + Sentence(e.Message) + " Nothing was changed."); return 3; }
                catch (Exception e)
                {
                    WriteLog("Didn't work: " + Sentence(e.Message));
                    return 1;
                }
            }

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            var form = new MainForm(updater);
            var show = updater.Log;
            updater.Log = text => { WriteLog(text); show(text); };
            Application.Run(form);
            return 0;
        }
    }
}
