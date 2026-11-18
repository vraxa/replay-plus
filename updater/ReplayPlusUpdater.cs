// Replay Plus Updater: installs and updates Replay Plus (a ReSkate.dll with the Animator Studio built in) and the
// ReSkate launcher that goes with it. Replay Plus comes from its own GitHub releases, the launcher from ReSkate's own
// releases (the version that Replay Plus build was made for), and every file is checked against the SHA-256 the
// Replay Plus release pins before anything in the game folder is touched.
//
// Command line (for testing): --folder <ReSkate folder>, --prerelease (pre-releases count too), --from <folder>
// (replay-plus.json and ReSkate.dll from a local folder instead of GitHub), --install / --restore (do it without the
// window and exit: 0 done, 1 failed). Everything it does goes to %LOCALAPPDATA%\ReSkate\Replay Plus\updater.log.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Net;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;

[assembly: AssemblyTitle("Replay Plus Updater")]
[assembly: AssemblyProduct("Replay Plus")]
[assembly: AssemblyDescription("Installs and updates Replay Plus for ReSkate")]
[assembly: AssemblyVersion("1.0.0.0")]
[assembly: AssemblyFileVersion("1.0.0.0")]

namespace ReplayPlusUpdater
{
    // A file one Replay Plus release pins: where it is and what it must hash to.
    class Pin
    {
        public string Repo, Tag, Name, Sha256;
        public long Size;
        public string Url { get { return "https://github.com/" + Repo + "/releases/download/" + Tag + "/" + Uri.EscapeDataString(Name); } }
    }

    // A Replay Plus release, from its replay-plus.json.
    class Build
    {
        public string Tag, ReplayPlus, Studio, ReSkate, Updater;
        public bool Prerelease;
        public Pin Dll, Launcher, LauncherJson;
        public string Title { get { return "Replay Plus " + ReplayPlus + " (Animator Studio " + Studio + ", ReSkate " + ReSkate + ")"; } }
    }

    // What the game folder has now.
    class Installed
    {
        public bool HasDll, Ours;
        public string Sha256, ReplayPlus, Studio, ReSkate, Describe;
    }

    static class Json
    {
        static readonly JavaScriptSerializer serializer = new JavaScriptSerializer { MaxJsonLength = 16 * 1024 * 1024 };
        public static object Parse(string text) { return serializer.DeserializeObject(text); }
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
    }

    static class Web
    {
        public const string Agent = "ReplayPlusUpdater";

        static HttpWebRequest Request(string url)
        {
            var request = (HttpWebRequest)WebRequest.Create(url);
            request.UserAgent = Agent;
            request.Accept = url.Contains("api.github.com") ? "application/vnd.github+json" : "*/*";
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

    class Updater
    {
        public const string Repo = "vraxa/replay-plus";
        public const string StudioRepo = "FourthStudio/ReSkate-Animator-Studio";
        public const string Api = "https://api.github.com/repos/";
        const string RecordName = "replay-plus-updater.json";
        const string OriginalSuffix = ".before-replay-plus";

        public string Folder;
        public bool Prerelease;
        public string FromFolder;
        public Action<string> Log = delegate { };
        public Action<long, long, string> Progress = delegate { };

        public static string DataFolder
        {
            get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ReSkate", "Replay Plus"); }
        }
        public static string Version
        {
            get { var v = Assembly.GetExecutingAssembly().GetName().Version; return v.Major + "." + v.Minor + (v.Build > 0 ? "." + v.Build : ""); }
        }

        public static bool IsReSkateFolder(string folder)
        {
            return !string.IsNullOrEmpty(folder) && File.Exists(Path.Combine(folder, "ReSkateLauncher.exe"));
        }

        // ---- What is out there --------------------------------------------------------------------------------------

        public Build Latest()
        {
            if (!string.IsNullOrEmpty(FromFolder))
            {
                var local = ParseBuild(File.ReadAllText(Path.Combine(FromFolder, "replay-plus.json")));
                local.Tag = "local";
                return local;
            }
            Dictionary<string, object> release = null;
            if (!Prerelease)
            {
                release = Json.Obj(Json.Parse(Web.Text(Api + Repo + "/releases/latest")));
                if (!HasAsset(release, "replay-plus.json"))
                    throw new Exception("the newest Replay Plus release (" + Json.Str(release, "tag_name") + ") can't be installed by this updater");
            }
            else
            {
                var list = Json.Parse(Web.Text(Api + Repo + "/releases?per_page=10")) as object[];
                if (list != null)
                    foreach (var item in list)
                    {
                        var r = Json.Obj(item);
                        if (r == null || Json.Bool(r, "draft") || !HasAsset(r, "replay-plus.json")) continue;
                        release = r;
                        break;
                    }
                if (release == null) throw new Exception("no Replay Plus release the updater can install");
            }
            var tag = Json.Str(release, "tag_name");
            var build = ParseBuild(Web.Text("https://github.com/" + Repo + "/releases/download/" + tag + "/replay-plus.json"));
            build.Tag = tag;
            build.Prerelease = Json.Bool(release, "prerelease");
            return build;
        }

        static bool HasAsset(Dictionary<string, object> release, string name)
        {
            var assets = Json.Get(release, "assets") as object[];
            if (assets == null) return false;
            foreach (var a in assets) if (Json.Str(Json.Obj(a), "name") == name) return true;
            return false;
        }

        static Build ParseBuild(string text)
        {
            var d = Json.Obj(Json.Parse(text));
            if (d == null || Json.Num(d, "schema") != 1) throw new Exception("replay-plus.json is not one this updater knows (a newer updater may be needed)");
            var b = new Build
            {
                ReplayPlus = Json.Str(d, "replay_plus"),
                Studio = Json.Str(d, "studio"),
                ReSkate = Json.Str(d, "reskate"),
                Updater = Json.Str(d, "updater"),
                Dll = ParsePin(Json.Obj(Json.Get(d, "dll"))),
                Launcher = ParsePin(Json.Obj(Json.Get(d, "launcher"))),
                LauncherJson = ParsePin(Json.Obj(Json.Get(d, "launcher_json"))),
            };
            if (b.Dll == null || string.IsNullOrEmpty(b.ReplayPlus)) throw new Exception("replay-plus.json has no ReSkate.dll in it");
            return b;
        }

        static Pin ParsePin(Dictionary<string, object> d)
        {
            if (d == null) return null;
            var pin = new Pin
            {
                Repo = Json.Str(d, "repo") ?? Repo,
                Tag = Json.Str(d, "tag"),
                Name = Json.Str(d, "name"),
                Sha256 = (Json.Str(d, "sha256") ?? "").ToLowerInvariant(),
                Size = Json.Num(d, "size"),
            };
            if (string.IsNullOrEmpty(pin.Name) || pin.Sha256.Length != 64) throw new Exception("replay-plus.json names a file without its SHA-256");
            return pin;
        }

        public string LatestStudio()
        {
            try { return Json.Str(Json.Obj(Json.Parse(Web.Text(Api + StudioRepo + "/releases/latest"))), "tag_name"); }
            catch (Exception) { return null; }
        }

        // ---- What is installed ------------------------------------------------------------------------------------

        public Installed Inspect() { return InspectFile(Path.Combine(Folder, "ReSkate.dll")); }

        public static Installed InspectFile(string dll)
        {
            var result = new Installed();
            if (!File.Exists(dll)) { result.Describe = "no ReSkate.dll here yet"; return result; }
            result.HasDll = true;
            var bytes = File.ReadAllBytes(dll);
            result.Sha256 = Hex(SHA256.Create().ComputeHash(bytes));
            // The version the DLL was built as, a C string in it: "1.1+studio-2.1.5.reskate-20261116" for Replay Plus,
            // "2.1.5+reskate.20261116" for the Animator Studio.
            var text = Encoding.GetEncoding(28591).GetString(bytes);
            var ours = Regex.Match(text, @"(\d+(?:\.\d+)+)\+studio-(\d+(?:\.\d+)+)\.reskate-([0-9A-Za-z.\-]+?)\x00");
            if (ours.Success)
            {
                result.Ours = true;
                result.ReplayPlus = ours.Groups[1].Value;
                result.Studio = ours.Groups[2].Value;
                result.ReSkate = ours.Groups[3].Value;
                result.Describe = "Replay Plus " + result.ReplayPlus + " (Animator Studio " + result.Studio + ", ReSkate " + result.ReSkate + ")";
                return result;
            }
            var studio = Regex.Match(text, @"(\d+\.\d+\.\d+)\+reskate\.([0-9A-Za-z.\-]+?)\x00");
            if (studio.Success)
            {
                result.Studio = studio.Groups[1].Value;
                result.ReSkate = studio.Groups[2].Value;
                result.Describe = "Animator Studio " + result.Studio + " (ReSkate " + result.ReSkate + "), no Replay Plus";
                return result;
            }
            result.Describe = "ReSkate without the Animator Studio or Replay Plus";
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

        public static string Running()
        {
            foreach (var name in new[] { "Skate", "ReSkateLauncher" })
                if (Process.GetProcessesByName(name).Length > 0) return name == "Skate" ? "skate." : "the ReSkate launcher";
            return null;
        }

        // ---- Installing -------------------------------------------------------------------------------------------

        public void Install(Build build)
        {
            var running = Running();
            if (running != null) throw new Exception("close " + running + " first");
            var cache = Path.Combine(DataFolder, "Updater downloads", build.Tag);
            Directory.CreateDirectory(cache);

            var dllPath = Path.Combine(Folder, "ReSkate.dll");
            var launcherPath = Path.Combine(Folder, "ReSkateLauncher.exe");
            var jsonPath = Path.Combine(Folder, "launcher.json");

            Log("Getting " + build.Title);
            var newDll = Fetch(build.Dll, cache, true);
            string newLauncher = null, newJson = null;
            if (build.Launcher != null && !Matches(launcherPath, build.Launcher))
            {
                Log("Getting the ReSkate " + build.Launcher.Tag + " launcher from ReSkate's own release");
                newLauncher = Fetch(build.Launcher, cache, false);
            }
            if (build.LauncherJson != null && !Matches(jsonPath, build.LauncherJson)) newJson = Fetch(build.LauncherJson, cache, false);

            running = Running();
            if (running != null) throw new Exception("close " + running + " first");
            var record = ReadRecord();
            if (record == null) record = KeepOriginals(dllPath, launcherPath, jsonPath);

            Put(newDll, dllPath);
            if (newLauncher != null) Put(newLauncher, launcherPath);
            if (newJson != null) Put(newJson, jsonPath);

            bool? before = SetUpdates(Path.Combine(Folder, "ReSkateLauncher.settings.json"), false);
            if (!record.ContainsKey("updates_before")) record["updates_before"] = before;
            record["installed"] = build.ReplayPlus;
            record["studio"] = build.Studio;
            record["reskate"] = build.ReSkate;
            record["tag"] = build.Tag;
            record["when"] = DateTime.Now.ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture);
            WriteRecord(record);
            Log("Done: " + build.Title + " is installed. Start ReSkate like you normally do.");
        }

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
                if (path.StartsWith(cache, StringComparison.OrdinalIgnoreCase)) File.Delete(path);
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

        // The first time: what was there before Replay Plus, kept as <name>.before-replay-plus. A Replay Plus put in by
        // hand earlier points at the DLL kept from before it instead (ReSkate.dll.studio, as its README said, or
        // ReSkate.dll.original), when that one is not a Replay Plus too.
        Dictionary<string, object> KeepOriginals(string dllPath, string launcherPath, string jsonPath)
        {
            var record = new Dictionary<string, object>();
            var originals = new Dictionary<string, object>();
            if (File.Exists(dllPath))
            {
                if (!Inspect().Ours) { File.Copy(dllPath, dllPath + OriginalSuffix, true); originals["ReSkate.dll"] = "ReSkate.dll" + OriginalSuffix; }
                else
                    foreach (var name in new[] { "ReSkate.dll.studio", "ReSkate.dll.original" })
                    {
                        var kept = Path.Combine(Folder, name);
                        if (File.Exists(kept) && !InspectFile(kept).Ours) { originals["ReSkate.dll"] = name; break; }
                    }
            }
            foreach (var path in new[] { launcherPath, jsonPath })
                if (File.Exists(path))
                {
                    File.Copy(path, path + OriginalSuffix, true);
                    originals[Path.GetFileName(path)] = Path.GetFileName(path) + OriginalSuffix;
                }
            record["originals"] = originals;
            Log("Kept what you had before: " + (originals.Count > 0 ? string.Join(", ", new List<object>(originals.Values).ConvertAll(o => o.ToString()).ToArray()) : "nothing to keep"));
            return record;
        }

        public bool CanRestore()
        {
            var record = ReadRecord();
            var originals = record == null ? null : Json.Obj(Json.Get(record, "originals"));
            if (originals != null && originals.Count > 0) return true;
            return File.Exists(Path.Combine(Folder, "ReSkate.dll.studio"));
        }

        public void Restore()
        {
            var running = Running();
            if (running != null) throw new Exception("close " + running + " first");
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
            var before = record == null ? null : Json.Get(record, "updates_before");
            if (before is bool && (bool)before) SetUpdates(Path.Combine(Folder, "ReSkateLauncher.settings.json"), true);
            var recordPath = Path.Combine(Folder, RecordName);
            if (File.Exists(recordPath)) File.Delete(recordPath);
            Log("Put back what you had before Replay Plus.");
        }

        Dictionary<string, object> ReadRecord()
        {
            var path = Path.Combine(Folder, RecordName);
            try { return File.Exists(path) ? Json.Obj(Json.Parse(File.ReadAllText(path))) : null; }
            catch (Exception) { return null; }
        }

        void WriteRecord(Dictionary<string, object> record)
        {
            var text = new JavaScriptSerializer().Serialize(record);
            File.WriteAllText(Path.Combine(Folder, RecordName), text, new UTF8Encoding(false));
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
            Log("Set \"updates\" to " + word + " in ReSkateLauncher.settings.json" + (value ? "" : " (so the launcher doesn't swap the dll back)"));
            return before;
        }
    }

    class MainForm : Form
    {
        readonly Updater updater;
        readonly Label folderValue = new Label(), installedValue = new Label(), latestValue = new Label(), notice = new Label();
        readonly Button installButton = new Button(), restoreButton = new Button(), folderButton = new Button(), openButton = new Button();
        readonly ProgressBar progress = new ProgressBar();
        readonly TextBox log = new TextBox();
        Build latest;
        bool busy;

        public MainForm(Updater updater)
        {
            this.updater = updater;
            Text = "Replay Plus Updater " + Updater.Version;
            Font = new Font("Segoe UI", 9f);
            AutoScaleMode = AutoScaleMode.Dpi;
            ClientSize = new Size(620, 400);
            MinimumSize = new Size(520, 360);
            StartPosition = FormStartPosition.CenterScreen;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch (Exception) { }

            var table = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(12), ColumnCount = 3, RowCount = 7 };
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            for (int i = 0; i < 6; ++i) table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            table.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

            AddRow(table, 0, "ReSkate folder", folderValue, folderButton, "Change...");
            AddRow(table, 1, "You have", installedValue, null, null);
            AddRow(table, 2, "Newest", latestValue, null, null);
            notice.AutoSize = true;
            notice.ForeColor = Color.FromArgb(150, 90, 0);
            notice.Margin = new Padding(3, 6, 3, 6);
            table.Controls.Add(notice, 0, 3);
            table.SetColumnSpan(notice, 3);

            var buttons = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, Margin = new Padding(0, 4, 0, 4) };
            foreach (var b in new[] { installButton, restoreButton, openButton })
            {
                b.AutoSize = true;
                b.Padding = new Padding(8, 2, 8, 2);
                buttons.Controls.Add(b);
            }
            installButton.Text = "Install";
            restoreButton.Text = "Put back what I had before";
            openButton.Text = "Open the folder";
            table.Controls.Add(buttons, 0, 4);
            table.SetColumnSpan(buttons, 3);

            progress.Dock = DockStyle.Fill;
            progress.Height = 16;
            table.Controls.Add(progress, 0, 5);
            table.SetColumnSpan(progress, 3);

            log.Multiline = true;
            log.ReadOnly = true;
            log.ScrollBars = ScrollBars.Vertical;
            log.Dock = DockStyle.Fill;
            log.BackColor = SystemColors.Window;
            table.Controls.Add(log, 0, 6);
            table.SetColumnSpan(log, 3);
            Controls.Add(table);

            updater.Log = text => Ui(() => { log.AppendText(text + Environment.NewLine); });
            updater.Progress = (done, total, name) => Ui(() =>
            {
                progress.Maximum = 1000;
                progress.Value = total > 0 ? (int)Math.Min(1000, done * 1000 / total) : 0;
                installButton.Text = string.Format(CultureInfo.InvariantCulture, "{0} {1:0.0} / {2:0.0} MB", name, done / 1048576.0, total / 1048576.0);
            });

            folderButton.Click += (s, e) => PickFolder();
            openButton.Click += (s, e) => { if (Updater.IsReSkateFolder(updater.Folder)) Process.Start("explorer.exe", "\"" + updater.Folder + "\""); };
            installButton.Click += (s, e) => Work(() => { updater.Install(latest); });
            restoreButton.Click += (s, e) =>
            {
                if (MessageBox.Show(this, "Put back the ReSkate.dll and launcher you had before Replay Plus?", "Replay Plus Updater",
                        MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes)
                    Work(() => { updater.Restore(); });
            };
            Shown += (s, e) =>
            {
                if (!Updater.IsReSkateFolder(updater.Folder)) PickFolder();
                else Check();
            };
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
            if (InvokeRequired) BeginInvoke(action);
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

        // Runs `action` (an install or a restore) on its own thread, then looks again at what is there and what is out.
        void Work(Action action)
        {
            if (busy) return;
            busy = true;
            SetButtons(false);
            progress.Value = 0;
            new Thread(() =>
            {
                string error = null;
                try { if (action != null) action(); }
                catch (UnauthorizedAccessException) { error = "Windows won't let the updater write to that folder. Try running it as administrator."; }
                catch (IOException e) { error = "A file is busy or can't be written (" + e.Message + "). Make sure skate. and the launcher are closed."; }
                catch (WebException e) { error = "Couldn't reach GitHub (" + e.Message + ")."; }
                catch (Exception e) { error = Program.Sentence(e.Message); }
                if (error != null) updater.Log("Didn't work: " + error);

                Installed installed = null;
                Build newest = null;
                string studio = null, problem = null;
                try { installed = updater.Inspect(); } catch (Exception e) { problem = "Couldn't read ReSkate.dll: " + e.Message; }
                try { newest = updater.Latest(); studio = updater.LatestStudio(); }
                catch (Exception e) { problem = "Couldn't check GitHub for the newest Replay Plus: " + e.Message; }
                Ui(() => ShowState(installed, newest, studio, problem, error));
            }) { IsBackground = true }.Start();
        }

        void ShowState(Installed installed, Build newest, string studio, string problem, string error)
        {
            busy = false;
            latest = newest;
            installedValue.Text = installed == null ? "?" : installed.Describe;
            latestValue.Text = newest == null ? "?" : newest.Title + (newest.Prerelease ? " (pre-release)" : "");
            var notes = new List<string>();
            if (problem != null) notes.Add(problem);
            if (newest != null && studio != null && Updater.Compare(studio, newest.Studio) > 0)
                notes.Add("Animator Studio " + studio + " is out. Replay Plus for it isn't ready yet, so this one stays on " + newest.Studio + ".");
            if (newest != null && !string.IsNullOrEmpty(newest.Updater) && Updater.Compare(newest.Updater, Updater.Version) > 0)
                notes.Add("There's a newer updater (" + newest.Updater + ") on the releases page.");
            notice.Text = string.Join(Environment.NewLine, notes.ToArray());
            bool upToDate = installed != null && newest != null && installed.Ours && installed.Sha256 == newest.Dll.Sha256;
            installButton.Text = newest == null ? "Install" : upToDate ? "Reinstall" : installed != null && installed.Ours ? "Update" : "Install";
            SetButtons(true);
            installButton.Enabled = newest != null;
            restoreButton.Enabled = updater.CanRestore();
            progress.Value = error == null && newest != null ? progress.Value : 0;
            if (installed != null && newest != null && error == null && log.TextLength == 0)
                updater.Log(upToDate ? "You're up to date." : installed.Ours ? "There's an update." : "Ready to install.");
        }

        void SetButtons(bool on)
        {
            installButton.Enabled = on;
            restoreButton.Enabled = on;
            folderButton.Enabled = on;
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
            if (string.IsNullOrEmpty(text)) return "something went wrong.";
            text = char.ToUpperInvariant(text[0]) + text.Substring(1);
            return text.EndsWith(".") ? text : text + ".";
        }

        static void WriteLog(string line)
        {
            try
            {
                Directory.CreateDirectory(Updater.DataFolder);
                File.AppendAllText(LogPath, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture) + "  " + line + Environment.NewLine);
            }
            catch (Exception) { }
        }

        [STAThread]
        static int Main(string[] args)
        {
            ServicePointManager.SecurityProtocol = (SecurityProtocolType)3072; // TLS 1.2
            var updater = new Updater();
            bool install = false, restore = false;
            for (int i = 0; i < args.Length; ++i)
            {
                var a = args[i].ToLowerInvariant();
                if (a == "--folder" && i + 1 < args.Length) updater.Folder = args[++i];
                else if (a == "--from" && i + 1 < args.Length) updater.FromFolder = args[++i];
                else if (a == "--prerelease") updater.Prerelease = true;
                else if (a == "--install") install = true;
                else if (a == "--restore") restore = true;
            }
            if (string.IsNullOrEmpty(updater.Folder))
            {
                var saved = SavedFolder();
                var here = Path.GetDirectoryName(Application.ExecutablePath);
                updater.Folder = Updater.IsReSkateFolder(saved) ? saved : Updater.IsReSkateFolder(here) ? here : null;
            }
            WriteLog("---- Replay Plus Updater " + Updater.Version + (install ? " --install" : restore ? " --restore" : "") +
                     (updater.Prerelease ? " --prerelease" : "") + (updater.FromFolder != null ? " --from " + updater.FromFolder : "") +
                     ", folder " + (updater.Folder ?? "(not found yet)"));

            if (install || restore)
            {
                updater.Log = WriteLog;
                updater.Progress = delegate { };
                try
                {
                    if (!Updater.IsReSkateFolder(updater.Folder)) throw new Exception("no ReSkate folder (pass --folder)");
                    if (install) updater.Install(updater.Latest());
                    else updater.Restore();
                    return 0;
                }
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
