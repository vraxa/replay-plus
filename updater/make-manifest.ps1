# Makes a release's two manifests out of its builds:
#   replay-plus-builds.json  (schema 2) every Replay Plus build, one per Animator Studio version, and the patches the
#                            updater builds Replay Plus with for any other Studio. Updaters from 1.2 on read this one.
#   replay-plus.json         (schema 1) just the build on the newest Studio, for updaters from before 1.2.
# The builds come from ReplayPlusUpdater.exe --build-only (build-<studio>.json beside each DLL). The workflow in
# .github/workflows/builds.yml runs this; by hand it's e.g.
#   powershell -File updater\make-manifest.ps1 -Tag v1.2 -Patches patches -Add out\build-2.1.5.json -Out out
# A release made before 1.2 has only replay-plus.json: -Old with it keeps its build in the list.
param(
    [Parameter(Mandatory = $true)][string]$Tag,  # the release the DLLs and patches are (or go) in
    [string]$Current = '',                       # that release's replay-plus-builds.json as it is now: its builds stay
    [string]$Old = '',                           # or, for a release from before 1.2, its replay-plus.json: its build stays
    [string[]]$Add = @(),                        # build-<studio>.json files to add (a newer one for a Studio replaces it)
    [string]$Patches = '',                       # the patches folder (patches.json and the .patch files) of this release
    [string]$Updater = '1.2',                    # the newest updater, so older ones say there's a newer one
    [string]$Out = '.'
)
$ErrorActionPreference = 'Stop'
$repo = 'vraxa/replay-plus'

function Read-Json([string]$path) { Get-Content -Raw -LiteralPath $path | ConvertFrom-Json }
function Get-Numbers([string]$v) { @([regex]::Matches(($v -replace '^[vV]', ''), '\d+') | ForEach-Object { [int]$_.Value }) }
function Compare-Version([string]$a, [string]$b) {
    $x = Get-Numbers $a
    $y = Get-Numbers $b
    for ($i = 0; $i -lt [Math]::Max($x.Count, $y.Count); $i++) {
        $p = 0; $q = 0
        if ($i -lt $x.Count) { $p = $x[$i] }
        if ($i -lt $y.Count) { $q = $y[$i] }
        if ($p -ne $q) { return [Math]::Sign($p - $q) }
    }
    return 0
}
function Get-Pin([string]$path) {
    $item = Get-Item -LiteralPath $path
    [ordered]@{ repo = $repo; tag = $Tag; name = $item.Name; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower(); size = $item.Length }
}

$builds = @()
$patchPins = @()
$plus = $null
if ($Current) {
    $now = Read-Json $Current
    if ($now.schema -ne 2) { throw "$Current isn't a schema 2 replay-plus-builds.json" }
    $builds = @($now.builds)
    $patchPins = @($now.patches)
    $plus = $now.replay_plus
}
elseif ($Old) {
    $one = Read-Json $Old
    if ($one.schema -ne 1) { throw "$Old isn't a schema 1 replay-plus.json" }
    $builds = @([pscustomobject][ordered]@{
        replay_plus = $one.replay_plus; studio = $one.studio; reskate = $one.reskate; game_build = $one.game_build
        dll = $one.dll; launcher = $one.launcher; launcher_json = $one.launcher_json
    })
    $plus = $one.replay_plus
    Write-Host "kept Replay Plus $($one.replay_plus) on Animator Studio $($one.studio) ($($one.dll.name)) from $Old"
}
foreach ($file in $Add) {
    $b = Read-Json $file
    $b.dll | Add-Member -NotePropertyName repo -NotePropertyValue $repo -Force
    $b.dll | Add-Member -NotePropertyName tag -NotePropertyValue $Tag -Force
    $newer = @($builds | Where-Object { (Compare-Version $_.studio $b.studio) -eq 0 -and (Compare-Version $_.replay_plus $b.replay_plus) -gt 0 })
    if ($newer.Count -gt 0) { Write-Host "kept the newer Replay Plus $($newer[0].replay_plus) for Animator Studio $($b.studio)"; continue }
    $builds = @($builds | Where-Object { (Compare-Version $_.studio $b.studio) -ne 0 }) + @($b)
    Write-Host "added Replay Plus $($b.replay_plus) on Animator Studio $($b.studio) ($($b.dll.name))"
}
if ($Patches) {
    $info = Read-Json (Join-Path $Patches 'patches.json')
    $plus = $info.replay_plus
    $patchPins = @($info.patches | ForEach-Object { Get-Pin (Join-Path $Patches $_) })
}
if (-not $plus) { throw 'which Replay Plus is this? Pass -Patches (with its patches.json) or -Current' }
if ($builds.Count -eq 0) { throw 'there are no builds to list' }
$builds = @($builds | Sort-Object { (@(Get-Numbers $_.studio) | ForEach-Object { '{0:D6}' -f $_ }) -join '.' } -Descending)
$newest = $builds[0]

$manifest = [ordered]@{ schema = 2; replay_plus = $plus; updater = $Updater; patches = $patchPins; builds = $builds }
$first = [ordered]@{
    schema = 1; replay_plus = $newest.replay_plus; studio = $newest.studio; reskate = $newest.reskate; game_build = $newest.game_build
    updater = $Updater; dll = $newest.dll; launcher = $newest.launcher; launcher_json = $newest.launcher_json
}
$Out = (New-Item -ItemType Directory -Force -Path $Out).FullName
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $Out 'replay-plus-builds.json'), ($manifest | ConvertTo-Json -Depth 10) + "`n", $utf8)
[IO.File]::WriteAllText((Join-Path $Out 'replay-plus.json'), ($first | ConvertTo-Json -Depth 10) + "`n", $utf8)
Write-Host "replay-plus-builds.json: Replay Plus $plus, ready-made for Animator Studio $(($builds | ForEach-Object { $_.studio }) -join ', ')"
Write-Host "replay-plus.json: Replay Plus $($newest.replay_plus) on Animator Studio $($newest.studio)"
