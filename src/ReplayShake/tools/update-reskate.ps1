<#
.SYNOPSIS
Builds ReSkate with Replay Shake (and the extra patches beside it: stick-pops.patch) and puts it in the
folder ReSkate runs from: the first time from the patch, and again whenever ReSkate has a new version.

.DESCRIPTION
The first time, from the folder holding replay-shake.patch and this script:

    powershell -ExecutionPolicy Bypass -File update-reskate.ps1 -Patch replay-shake.patch -Skate "C:\Games\ReSkate"

It clones ReSkate into a ReSkate folder beside this script, puts Replay Shake on a branch of its own,
builds it with Visual Studio 2022, runs Replay Shake's tests, and copies ReSkate.dll and
ReSkateLauncher.exe into the folder you start ReSkate from, beside the copy of the game its launcher
keeps there. The ones already there are kept as *.original (the first time) and *.previous (every
time). Since 2.0.8, ReSkate's launcher does not start from Steam's skate. folder
(...\steamapps\common\...): it has a folder of its own, so that is the one to give.

It builds on the ReSkate its launcher installs (-On), so it plays online with everyone on that release
(ReSkate's main branch moves on, and its multiplayer may not match the servers'). After that, whenever
ReSkate puts out a new release, run it again with nothing more (or with that release's -On):

    powershell -ExecutionPolicy Bypass -File update-reskate.ps1

It fetches the newest release, moves Replay Shake onto it, builds, tests and installs. Where ReSkate
changed the lines beside Replay Shake's few lines in its files, it puts them back beside ReSkate's new
ones. Should one of those lines of ReSkate's be gone, or the build fail, nothing is installed, the
ReSkate you have keeps working, and it says what to fit by hand (README.md, Staying up to date); run
it again after, and it finishes the move, builds and installs.

    -Patch       the first time, the patch; later, a newer one replaces Replay Shake (the branch as it
                 was is kept as replay-shake-previous)
    -Extra       more patches put on top of Replay Shake, each one commit: stick-pops.patch (L3 / R3 pop
                 out of grinds, Skater XL style). When not given, stick-pops.patch beside this script is
                 used if it is there. A newer one replaces the one on the branch, and one no longer given
                 is taken off (the branch as it was is kept as replay-shake-previous).
    -NoExtra     build Replay Shake without the extra patches (takes them off the branch)
    -Rollback    put back the ReSkate.dll and ReSkateLauncher.exe from before the last install
    -Original    put back the ones from before Replay Shake was first installed
    -Force       build and install even when ReSkate has not changed (it is also built and installed
                 when what is in the folder you start ReSkate from is not this build, as after the
                 ReSkate launcher put a release there)
    -On          the ReSkate to build on: release (the newest release tag; the default), a commit or tag
                 (ReSkate sometimes rebuilds a release from newer code without moving its tag: 2.1.2's
                 download is main at 55a7342d), or main (its newest code, unreleased)
    -Fetch       more of ReSkate to fetch before that, by the name GitHub gives it: a pull request's
                 commits are on none of ReSkate's branches (pull/383/head, the Animator Studio's: with
                 -On its commit, Replay Shake goes onto that). A commit -On names in full is also
                 fetched by itself, when nothing fetched holds it.
    -FetchFrom   the repository -Fetch's names, and a commit -On names that ReSkate's own does not
                 have, are fetched from (its URL; ReSkate's when not given): the Animator Studio's
                 releases for each ReSkate are made in https://github.com/FourthStudio/ReSkate-Animator-Studio
    -NoFetch     build what is checked out now (your own changes too), without fetching ReSkate
    -SkipTests   skip Replay Shake's tests
    -ReSkate     where the ReSkate checkout is (remembered); -Skate the folder you start ReSkate from
                 (remembered)

Runs in Windows PowerShell 5.1 and PowerShell 7. Needs Git and Visual Studio 2022's C++ tools: Build
Tools for Visual Studio 2022 (https://aka.ms/vs/17/release/vs_buildtools.exe) with the "Desktop
development with C++" workload, or Visual Studio 2022 itself (ReSkate's README). A newer Visual Studio
alone does not have them.
#>
[CmdletBinding()]
param(
    [string]$Skate,
    [string]$ReSkate,
    [string]$Patch,
    [string[]]$Extra,
    [switch]$NoExtra,
    [switch]$Rollback,
    [switch]$Original,
    [switch]$Force,
    [switch]$NoFetch,
    [switch]$SkipTests,
    [string]$On = 'release',
    [string[]]$Fetch,
    [string]$FetchFrom,
    [string]$Upstream = 'https://github.com/Dingo-Shenanigans/ReSkate'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

$branch = 'replay-shake'
# Where -Fetch's names (and a missing -On or patch commit) are fetched from: a URL, or ReSkate's own.
$wantedFrom = if ($FetchFrom) { $FetchFrom } else { 'origin' }
$files = @('ReSkate.dll', 'ReSkateLauncher.exe')

function Say([string]$Text) { Write-Host $Text }
function Fail([string]$Text, [int]$Code = 1) {
    Write-Host ''
    Write-Host $Text -ForegroundColor Red
    exit $Code
}
# A program, its output on the console. Native tools write progress to stderr: that must not become
# a PowerShell error. Returns the exit code.
function Run([string]$Program, [string[]]$Arguments) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Program @Arguments | Out-Host
        return $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $saved
    }
}
# A program's output as text, or $null when it fails.
function Capture([string]$Program, [string[]]$Arguments) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $text = & $Program @Arguments 2>$null
        if ($LASTEXITCODE -ne 0) { return $null }
        return (@($text) -join "`n").Trim()
    } finally {
        $ErrorActionPreference = $saved
    }
}
# Path parts joined. In an array, a part made with + needs brackets: the comma binds tighter than +.
function Join([string[]]$Parts) { return [System.IO.Path]::Combine($Parts) }

# What it remembers between runs: where things are, and what it installed.
$stateFolder = if ($env:LOCALAPPDATA) { Join @($env:LOCALAPPDATA, 'ReSkate') } else { Join @($HOME, '.reskate') }
$stateFile = Join @($stateFolder, 'replay_shake_update.json')
$state = @{}
if (Test-Path -LiteralPath $stateFile) {
    try {
        $saved = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
        foreach ($property in $saved.PSObject.Properties) { $state[$property.Name] = [string]$property.Value }
    } catch {
        Say "($stateFile could not be read: starting afresh.)"
    }
}
function Remember([string]$Name, [string]$Value) { $state[$Name] = $Value }
function Save-State {
    New-Item -ItemType Directory -Force -Path $stateFolder | Out-Null
    ([pscustomobject]$state | ConvertTo-Json) | Set-Content -LiteralPath $stateFile -Encoding UTF8
}
function Recall([string]$Name) { if ($state.ContainsKey($Name)) { return $state[$Name] } return '' }

function Assert-GameClosed {
    $running = @(Get-Process -Name 'Skate', 'ReSkateLauncher' -ErrorAction SilentlyContinue)
    if ($running.Count) { Fail 'Close skate. and the ReSkate launcher first: their files cannot be replaced while they run.' }
}
function Assert-Skate {
    if (-not $script:Skate) {
        Fail 'Say where ReSkate is, the first time: -Skate "<the folder you start ReSkateLauncher.exe from>".'
    }
    if (-not (Test-Path -LiteralPath (Join @($script:Skate, 'Skate.exe')))) { Fail "There is no Skate.exe in $script:Skate." }
}
# ReSkate's launcher (2.0.8 on) closes when started from a game's folder in a Steam library
# (...\steamapps\common\<game>, Launcher/main.cpp in_steam_library): a build installed there would
# not start. Checked before anything is built.
function Assert-NotSteamLibrary {
    for ($folder = New-Object System.IO.DirectoryInfo $script:Skate; $folder -and $folder.Parent; $folder = $folder.Parent) {
        if ($folder.Name -ieq 'common' -and $folder.Parent.Name -ieq 'steamapps') {
            Fail ("$script:Skate is Steam's skate. folder, and ReSkate's launcher no longer starts from there (ReSkate " +
                '2.0.8 on): it runs from a folder of its own, with its own copy of the game. Give that folder: ' +
                '-Skate "<the folder you start ReSkateLauncher.exe from>". Nothing was changed.')
        }
    }
}

if (-not $Skate) { $Skate = Recall 'skate' }
if ($Skate) { $Skate = [System.IO.Path]::GetFullPath($Skate) }

# Going back needs nothing built.
if ($Rollback -or $Original) {
    Assert-Skate
    $suffix = if ($Original) { '.original' } else { '.previous' }
    foreach ($name in $files) {
        if (-not (Test-Path -LiteralPath (Join @($Skate, ($name + $suffix))))) { Fail "There is no $name$suffix in $Skate to go back to." }
    }
    Assert-GameClosed
    foreach ($name in $files) { Copy-Item -LiteralPath (Join @($Skate, ($name + $suffix))) -Destination (Join @($Skate, $name)) -Force }
    Remember 'installed_commit' ''
    Save-State
    Say "Put back ReSkate.dll and ReSkateLauncher.exe from their $suffix copies in $Skate."
    exit 0
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Fail 'Git is not installed: get it from https://git-scm.com/download/win, then run this again.'
}

# The ReSkate checkout: given; the one this script sits in; remembered; or a new one beside it.
if (-not $ReSkate) {
    $inside = Capture git @('-C', $PSScriptRoot, 'rev-parse', '--show-toplevel')
    if ($inside -and (Test-Path -LiteralPath (Join @($inside, 'Extension', 'ReplayShake', 'ReplayShake.cmake')))) { $ReSkate = $inside }
    elseif (Recall 'reskate') { $ReSkate = Recall 'reskate' }
    else { $ReSkate = Join @($PSScriptRoot, 'ReSkate') }
}
$ReSkate = [System.IO.Path]::GetFullPath($ReSkate)

# Visual Studio's own CMake, else one on the PATH (REPLAY_SHAKE_CMAKE names another).
function Find-CMake {
    if ($env:REPLAY_SHAKE_CMAKE) { return $env:REPLAY_SHAKE_CMAKE }
    $programs = ${env:ProgramFiles(x86)}
    if ($programs) {
        $vswhere = Join @($programs, 'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
        if (Test-Path -LiteralPath $vswhere) {
            $vs = Capture $vswhere @('-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                '-property', 'installationPath')
            if ($vs) {
                $bundled = Join @($vs, 'Common7', 'IDE', 'CommonExtensions', 'Microsoft', 'CMake', 'CMake', 'bin', 'cmake.exe')
                if (Test-Path -LiteralPath $bundled) { return $bundled }
            }
        }
    }
    $onPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    Fail ('Visual Studio 2022 with the "Desktop development with C++" workload is needed to build ReSkate ' +
        '(https://visualstudio.microsoft.com/downloads/, Community is free). Install it, then run this again.')
}

# ReSkate builds with Visual Studio 2022's C++ tools (MSVC v143, CMakePresets.json). A newer Visual
# Studio alone does not have them: what to install, when they are missing.
function Get-ToolsHint {
    $programs = ${env:ProgramFiles(x86)}
    if (-not $programs) { return '' }
    $vswhere = Join @($programs, 'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
    if (-not (Test-Path -LiteralPath $vswhere)) { return '' }
    $vs2022 = Capture $vswhere @('-products', '*', '-version', '[17.0,18.0)', '-requires',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath')
    if ($vs2022) { return '' }
    return ("ReSkate builds with Visual Studio 2022's C++ tools (MSVC v143), and they are not installed: a newer " +
        "Visual Studio does not have them. Install Build Tools for Visual Studio 2022 beside it, from " +
        "https://aka.ms/vs/17/release/vs_buildtools.exe, with the 'Desktop development with C++' workload. " +
        "Then run this again.`n`n")
}
# A configure that failed, or was made by another CMake, may hold on to a Visual Studio that cannot
# build ReSkate: the build folder then starts afresh (what was built stays, and is reused).
function Clear-Configure([string]$Build) {
    foreach ($part in 'CMakeCache.txt', 'CMakeFiles') {
        $path = Join @($Build, $part)
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue }
    }
}
# The ReSkate to build on: 'release', its newest release (a tag vX.Y.Z, the one its launcher installs);
# 'main', its newest code; else the tag or commit named. '' when there is none.
function Get-Base([string]$Want) {
    if (-not $Want -or $Want -eq 'release') {
        $tags = @((Capture git @('tag', '--list', 'v*')) -split "`n" | Where-Object { $_ -match '^v\d+\.\d+\.\d+$' })
        if (-not $tags.Count) { return '' }
        $newest = $tags | Sort-Object { [version]($_.Substring(1)) } | Select-Object -Last 1
        return "refs/tags/$newest"
    }
    if ($Want -eq 'main') {
        $main = Capture git @('rev-parse', '--abbrev-ref', 'origin/HEAD')
        if ($main) { return $main }
        return 'origin/main'
    }
    if ($null -ne (Capture git @('rev-parse', '--verify', '--quiet', "$Want^{commit}"))) { return $Want }
    return ''
}
# What -Fetch names, fetched from GitHub (-FetchFrom, else ReSkate's own; kept as refs/fetched/<name>);
# then a commit -On names in full that is still missing, by itself (GitHub gives a commit its branches
# or pull requests hold). What GitHub does not give is said, and Get-Base says what is then missing.
function Get-Wanted {
    foreach ($name in @($Fetch | Where-Object { $_ } | ForEach-Object { $_ -replace '^refs/', '' })) {
        if ((Run git @('-C', $ReSkate, 'fetch', '--no-tags', $wantedFrom, "+refs/${name}:refs/fetched/$name")) -ne 0) {
            Say "(GitHub did not give ${name}, above.)"
        }
    }
    if ($On -match '^[0-9a-f]{40}$' -and $null -eq (Capture git @('-C', $ReSkate, 'cat-file', '-e', "$On^{commit}"))) {
        Run git @('-C', $ReSkate, 'fetch', '--no-tags', $wantedFrom, $On) | Out-Null
    }
}
# Its name, as said: v2.1.2, or main.
function Get-BaseName([string]$Base) { return ($Base -replace '^refs/tags/', '' -replace '^origin/', '') }
# The ReSkate under Replay Shake's commit on the branch (its parent), or where the branch and $Base
# part, should there be no Replay Shake commit.
function Get-ShakeBase([string]$Base) {
    $forked = Capture git @('merge-base', 'HEAD', $Base)
    $range = if ($forked) { "$forked..HEAD" } else { 'HEAD' }
    $shake = @((Capture git @('rev-list', '--reverse', '--grep=^Replay Shake:', $range)) -split "`n" | Where-Object { $_ }) |
        Select-Object -First 1
    if ($shake) { return (Capture git @('rev-parse', "$shake^")) }
    return $forked
}
# A newer replay-shake.patch replaces the Replay Shake commit there; your own commits after it stay on
# top, and the branch as it was is kept as replay-shake-previous. True when it did. It is made on the
# ReSkate the patch was made on (its base-commit line), and moves onto the newest ReSkate after, like
# any update. A patch with the same Replay Shake files, made on the same ReSkate, changes nothing.
function Update-ReplayShake([string]$PatchFile, [string]$Mainline) {
    $tip = Capture git @('rev-parse', 'HEAD')
    $forked = Capture git @('merge-base', 'HEAD', $Mainline)
    $range = if ($forked) { "$forked..HEAD" } else { 'HEAD' }
    $old = @((Capture git @('rev-list', '--reverse', '--grep=^Replay Shake:', $range)) -split "`n" | Where-Object { $_ }) |
        Select-Object -First 1
    if (-not $old) { return $false }
    $onto = Capture git @('rev-parse', "$old^")
    $made = Select-String -LiteralPath $PatchFile -Pattern '^base-commit: ([0-9a-f]{40})' | Select-Object -First 1
    if ($made) {
        $madeOn = $made.Matches[0].Groups[1].Value
        # A patch made on a newer ReSkate than this checkout has: that ReSkate is fetched first (by
        # itself, should it be on none of ReSkate's branches, as a pull request's commit is).
        if ($null -eq (Capture git @('cat-file', '-e', "$madeOn^{commit}")) -and -not $NoFetch) {
            Say 'Getting the ReSkate the new patch was made on ...'
            Run git @('fetch', 'origin') | Out-Null
            if ($null -eq (Capture git @('cat-file', '-e', "$madeOn^{commit}"))) { Run git @('fetch', '--no-tags', $wantedFrom, $madeOn) | Out-Null }
        }
        if ($null -ne (Capture git @('cat-file', '-e', "$madeOn^{commit}"))) { $onto = $madeOn }
    }
    if ((Run git @('checkout', '--quiet', '--detach', $onto)) -ne 0) { Fail "Could not set the new patch up (above)." 2 }
    if ((Run git @('am', '--3way', '--quiet', $PatchFile)) -ne 0) {
        Run git @('am', '--abort') | Out-Null
        Run git @('checkout', '--quiet', $branch) | Out-Null
        Fail 'The new patch does not go onto ReSkate (above). Nothing was changed.' 2
    }
    # The same Replay Shake on the same ReSkate: nothing to change.
    if ((Capture git @('rev-parse', "${old}:Extension/ReplayShake")) -eq (Capture git @('rev-parse', 'HEAD:Extension/ReplayShake')) -and
        (Capture git @('rev-parse', "$old^")) -eq (Capture git @('rev-parse', 'HEAD^'))) {
        Run git @('checkout', '--quiet', $branch) | Out-Null
        return $false
    }
    foreach ($own in @((Capture git @('rev-list', '--reverse', "$old..$tip")) -split "`n" | Where-Object { $_ })) {
        # The extra patches' commits are put back on by Update-Extras, from the patches given.
        if (Get-ExtraKind (Capture git @('log', '-1', '--format=%s', $own))) { continue }
        if ((Run git @('cherry-pick', '--allow-empty', $own)) -ne 0) {
            Run git @('cherry-pick', '--abort') | Out-Null
            Run git @('checkout', '--quiet', $branch) | Out-Null
            Fail "Your commit $own does not go onto the new Replay Shake (above). Nothing was changed." 2
        }
    }
    Run git @('branch', '--force', "$branch-previous", $tip) | Out-Null
    if ((Run git @('checkout', '--quiet', '-B', $branch)) -ne 0) { Fail "Could not move the branch '$branch' (above)." 2 }
    Say "Replay Shake replaced by the new patch (the branch as it was is kept as $branch-previous)."
    return $true
}

# The extra patches that go on top of Replay Shake (-Extra), each one commit with a subject of its
# own, by which its commit is told on the branch (never taken for one of your own).
$extraKinds = @(
    @{ Prefix = 'Stick pops:'; File = 'stick-pops.patch'; Name = 'the stick pops' }
)
function Get-PatchHash([string]$PatchFile) { return (Get-FileHash -LiteralPath $PatchFile -Algorithm SHA256).Hash }
# A patch's subject (its Subject line, without [PATCH]).
function Get-PatchSubject([string]$PatchFile) {
    $line = Select-String -LiteralPath $PatchFile -Pattern '^Subject: (\[PATCH[^\]]*\] )?(.*)$' | Select-Object -First 1
    if ($line) { return $line.Matches[0].Groups[2].Value }
    return ''
}
function Get-ExtraKind([string]$Subject) {
    foreach ($kind in $extraKinds) { if ($Subject -and $Subject.StartsWith($kind.Prefix)) { return $kind } }
    return $null
}
# The extra patches on the branch, after the Replay Shake commit: put on, replaced or taken off when
# the ones given change (their hashes are remembered). The branch is put together again from the
# Replay Shake commit: the extras in the order given, then your own commits. The branch as it was is
# kept as replay-shake-previous. True when the branch changed.
function Update-Extras([string[]]$Patches, [string]$Mainline) {
    $Patches = @($Patches | Where-Object { $_ })
    $hashes = (@($Patches | ForEach-Object { Get-PatchHash $_ })) -join ' '
    $tip = Capture git @('rev-parse', 'HEAD')
    $forked = Capture git @('merge-base', 'HEAD', $Mainline)
    $range = if ($forked) { "$forked..HEAD" } else { 'HEAD' }
    $shake = ''
    $extras = @()
    $own = @()
    foreach ($commit in @((Capture git @('rev-list', '--reverse', '--no-merges', $range)) -split "`n" | Where-Object { $_ })) {
        $subject = Capture git @('log', '-1', '--format=%s', $commit)
        if (-not $shake) {
            if ($subject -and $subject.StartsWith('Replay Shake:')) { $shake = $commit }
            continue
        }
        if (Get-ExtraKind $subject) { $extras += $commit } else { $own += $commit }
    }
    if (-not $shake) { return $false }
    if ($extras.Count -eq $Patches.Count -and (Recall 'replay_shake_extras') -eq $hashes) { return $false }
    if (-not $Patches.Count -and -not $extras.Count) {
        Remember 'replay_shake_extras' ''
        Save-State
        return $false
    }
    if ($Patches.Count) {
        Say ('Putting ' + ((@($Patches | ForEach-Object { (Get-ExtraKind (Get-PatchSubject $_)).Name })) -join ' and ') + ' on Replay Shake ...')
    } else {
        Say 'Taking the extra patches off Replay Shake ...'
    }
    if ((Run git @('checkout', '--quiet', '--detach', $shake)) -ne 0) { Fail 'Could not set the extra patches up (above).' 2 }
    foreach ($file in $Patches) {
        if ((Run git @('am', '--3way', '--quiet', $file)) -ne 0) {
            Run git @('am', '--abort') | Out-Null
            Run git @('checkout', '--quiet', $branch) | Out-Null
            Fail ((Split-Path -Leaf $file) + ' does not go onto Replay Shake on this ReSkate (above). Nothing was changed: ' +
                'make a new one for this ReSkate, or run this with -NoExtra to build Replay Shake without it.') 2
        }
    }
    foreach ($commit in $own) {
        if ((Run git @('cherry-pick', '--allow-empty', $commit)) -ne 0) {
            Run git @('cherry-pick', '--abort') | Out-Null
            Run git @('checkout', '--quiet', $branch) | Out-Null
            Fail "Your commit $commit does not go onto the extra patches (above). Nothing was changed." 2
        }
    }
    Run git @('branch', '--force', "$branch-previous", $tip) | Out-Null
    if ((Run git @('checkout', '--quiet', '-B', $branch)) -ne 0) { Fail "Could not move the branch '$branch' (above)." 2 }
    Remember 'replay_shake_extras' $hashes
    Save-State
    return $true
}
# Whether the files in the folder ReSkate starts from are the ones this checkout built last: the
# ReSkate launcher may have put a release over them since.
function Test-Installed {
    $release = Join @($ReSkate, 'build', 'vs2022-x64', 'Release')
    foreach ($name in $files) {
        $built = Join @($release, $name)
        $there = Join @($Skate, $name)
        if (-not (Test-Path -LiteralPath $built) -or -not (Test-Path -LiteralPath $there)) { return $false }
        if ((Get-FileHash -LiteralPath $built -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $there -Algorithm SHA256).Hash) { return $false }
    }
    return $true
}

# Replay Shake's changes to ReSkate's own files, all of them: lines put beside a line of ReSkate's
# (the first of the anchors found; Key finds them once in) and tokens added after one in a line.
# When ReSkate changes the lines around them, a move onto it stops; they are then put back beside
# ReSkate's new ones (Fit-Rebase). Lines go in with the anchor's indent.
$fits = @(
    @{ File = 'CMakeLists.txt'; Key = 'ReplayShake/ReplayShake\.cmake'; After = @('^include\(cmake/Apps\.cmake\)')
       Lines = @('include(Extension/ReplayShake/ReplayShake.cmake OPTIONAL)') }
    @{ File = 'Extension/Console/commands.h'; Key = 'register_replay_shake_commands'
       After = @('^void register_animation_studio_commands\(', '^void register_trainer_commands\(')
       Lines = @('void register_replay_shake_commands(Commands &);') }
    @{ File = 'Extension/Console/commands.cpp'; Key = 'register_replay_shake_commands'
       After = @('^\s*register_animation_studio_commands\(', '^\s*register_trainer_commands\(')
       Lines = @('register_replay_shake_commands(*result);') }
    @{ File = 'Runtime/client_tick.cpp'; Key = 'Extension/ReplayShake/replay_shake\.h'; After = @('^#include "runtime_internal\.h"')
       Lines = @('#include "Extension/ReplayShake/replay_shake.h"') }
    @{ File = 'Runtime/client_tick.cpp'; Key = 'replay_shake::tick\('
       Before = @('^\s*if \(!has_request && now < r\.next_model', '^\s*dingosdk::hall_of_meat::on_client_tick\(')
       Lines = @('dingosdk::replay_shake::tick(r.base, state);') }
    @{ File = 'Runtime/runtime_init.cpp'; Key = 'Extension/ReplayShake/replay_shake\.h'
       After = @('^#include "Extension/UI/Overlay/animation_studio_images\.h"', '^#include "runtime_internal\.h"')
       Lines = @('#include "Extension/ReplayShake/replay_shake.h"') }
    @{ File = 'Runtime/runtime_init.cpp'; Key = 'replay_shake::start\('
       After = @('^\s*\(void\)dingosdk::start_no_bail\(r\.base\);')
       Lines = @('const bool replay_shake = dingosdk::replay_shake::start(r.base);',
                 'record("{\"event\":\"replay_shake_initialized\",\"active\":" + std::string(replay_shake ? "true" : "false") + "}");') }
    # Its page: a tab of Settings, after REPLAYS (ReSkate 2.1.4 moved MUSIC and REPLAYS into Settings,
    # and ADDONS went). The lines are the commit's own, as for every fit.
    @{ File = 'Extension/UI/Overlay/skate_menu_internal.h'; Key = 'ReplayShake/replay_shake_page\.cpp'
       After = @('^void replays_page\(', '^void music_page\(')
       Lines = @('// Settings > REPLAY SHAKE (Extension/ReplayShake/replay_shake_page.cpp): the replay editor''s camera shake.',
                 'void replay_page(SkateMenu&, const Model&, const CallbacksV3&);') }
    @{ File = 'Extension/UI/Overlay/skate_menu_internal.h'; Key = 'settings_replay_shake_tab'
       After = @('settings_replays_tab = \d+;')
       Lines = @('inline constexpr int settings_replay_shake_tab = settings_replays_tab + 1; // Extension/ReplayShake''s tab, after REPLAYS') }
    @{ File = 'Extension/UI/Overlay/skate_menu_settings.cpp'; In = 'category_tabs\(menu, menu\.settings_tab'
       Token = '"REPLAYS"'; Add = ', "REPLAY SHAKE"' }
    @{ File = 'Extension/UI/Overlay/skate_menu_settings.cpp'; Key = '^\s*case settings_replay_shake_tab:'
       After = @('^\s*case settings_replays_tab:')
       Lines = @('case settings_replay_shake_tab: replay_page(menu, model, callbacks); break;') }
)
function Find-Line($Lines, [string[]]$Patterns) {
    foreach ($pattern in $Patterns) {
        for ($i = 0; $i -lt $Lines.Count; ++$i) { if ($Lines[$i] -cmatch $pattern) { return $i } }
    }
    return -1
}
function New-LineSet($Lines) {
    $set = New-Object 'System.Collections.Generic.HashSet[string]' -ArgumentList ([System.StringComparer]::Ordinal)
    foreach ($line in $Lines) { $null = $set.Add($line.Trim()) }
    return ,$set
}
# A token fit: Add after Token in the line In finds. Should that line or token be gone, after the
# token Add follows in the commit being moved ($Moving), where that token is in just one line.
function Fit-Token($Lines, $Fit, [string[]]$Moving) {
    $add = $Fit.Add
    $token = $Fit.Token
    $at = Find-Line $Lines @($Fit.In)
    if ($at -lt 0 -or -not $Lines[$at].Contains($token)) {
        $at = -1
        $mine = Find-Line $Moving @($Fit.In)
        if ($mine -lt 0 -or -not $Moving[$mine].Contains($add.Trim())) { $mine = Find-Line $Moving @([regex]::Escape($add.Trim())) }
        $follows = if ($mine -ge 0) { [regex]::Match($Moving[$mine], '("[^"]*",|\w+,)\s*' + [regex]::Escape($add.Trim())) } else { $null }
        if ($follows -and $follows.Success) {
            $token = $follows.Groups[1].Value
            $places = @(for ($i = 0; $i -lt $Lines.Count; ++$i) { if ($Lines[$i].Contains($token)) { $i } })
            if ($places.Count -eq 1) { $at = $places[0] }
        }
        if ($at -lt 0) { return $false }
    }
    if (-not $Lines[$at].Contains($token + $add)) {
        $Lines[$at] = $Lines[$at].Insert($Lines[$at].IndexOf($token) + $token.Length, $add)
    }
    return $true
}
# Replay Shake's changes put into the file as it is now (any already there are left be). False when
# a line of ReSkate's they go beside is gone. $Moving is the file as the commit being moved has it:
# the lines are taken from there, as they are now (a fit made by hand included), and should no
# anchor be found they go beside the same line of ReSkate's as there (the one below them or the one
# above, if it is still there just once). So a fit made by hand is followed the next time.
function Fit-File([string]$File, [string[]]$Moving = @()) {
    $path = Join @($ReSkate, $File)
    $text = [System.IO.File]::ReadAllText($path)
    $newline = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.AddRange([string[]]($text -split "`r?`n"))
    $known = New-LineSet $lines # ReSkate's lines
    foreach ($fit in @($fits | Where-Object { $_.File -ceq $File })) {
        if ($fit.ContainsKey('Token')) {
            if (-not (Fit-Token $lines $fit $Moving)) { return $false }
            continue
        }
        if ((Find-Line $lines @($fit.Key)) -ge 0) { continue }
        # The lines as the commit being moved has them: from the key on, up to a line of ReSkate's.
        $block = @($fit.Lines)
        $mine = Find-Line $Moving @($fit.Key)
        if ($mine -ge 0) {
            $block = @()
            for ($i = $mine; $i -lt $Moving.Count -and $block.Count -lt $fit.Lines.Count; ++$i) {
                $line = $Moving[$i].Trim()
                if ($i -gt $mine -and (-not $line -or $known.Contains($line))) { break }
                $block += $line
            }
        }
        $after = $fit.ContainsKey('After')
        $at = Find-Line $lines $(if ($after) { $fit.After } else { $fit.Before })
        if ($at -lt 0 -and $mine -ge 0) {
            $sides = @(@{ At = $mine + $block.Count; After = $false }, @{ At = $mine - 1; After = $true })
            if ($after) { [array]::Reverse($sides) }
            foreach ($side in $sides) {
                $neighbour = if ($side.At -ge 0 -and $side.At -lt $Moving.Count) { $Moving[$side.At].Trim() } else { '' }
                if ($neighbour.Length -le 3) { continue }
                $places = @(for ($i = 0; $i -lt $lines.Count; ++$i) { if ($lines[$i].Trim() -ceq $neighbour) { $i } })
                if ($places.Count -eq 1) {
                    $at = $places[0]
                    $after = $side.After
                    break
                }
            }
        }
        if ($at -lt 0) { return $false }
        $indent = [regex]::Match($lines[$at], '^\s*').Value
        $lines.InsertRange($(if ($after) { $at + 1 } else { $at }), [string[]]@($block | ForEach-Object { $indent + $_ }))
    }
    [System.IO.File]::WriteAllText($path, ($lines -join $newline), (New-Object System.Text.UTF8Encoding $false))
    return $true
}
# Whether every line the commit being moved adds to the file is in it now (for a token fit's line,
# its token), so that a fit never drops a change the $fits list does not know of.
function Test-Fitted([string]$File) {
    $text = [System.IO.File]::ReadAllText((Join @($ReSkate, $File)))
    $now = New-LineSet ($text -split "`r?`n")
    $tokens = @($fits | Where-Object { $_.File -ceq $File -and $_.ContainsKey('Token') } | ForEach-Object { $_.Add.Trim() })
    $diff = Capture git @('diff', '--no-color', '--unified=0', 'REBASE_HEAD~1', 'REBASE_HEAD', '--', $File)
    if ($null -eq $diff) { return $false }
    foreach ($added in @($diff -split "`n" | Where-Object { $_.StartsWith('+') -and -not $_.StartsWith('+++') })) {
        $line = $added.Substring(1).Trim()
        if (-not $line -or $now.Contains($line)) { continue }
        $carried = @($tokens | Where-Object { $line.Contains($_) })
        if (-not $carried.Count -or @($carried | Where-Object { -not $text.Contains($_) }).Count) { return $false }
    }
    return $true
}
# A move onto a new ReSkate stopped where ReSkate changed lines beside Replay Shake's: each such file
# is taken as the new ReSkate has it, with Replay Shake's changes put back (Fit-File), and the move
# goes on. False, the move still stopped, when that cannot be done here: a line of ReSkate's they go
# beside is gone, or the stop is in a commit of your own (that is yours to fit).
function Fit-Rebase {
    for ($round = 0; $round -lt 20; ++$round) {
        # Not stopped part way: the move failed before it began (round 0), or it is done.
        if (-not (Test-Path -LiteralPath (Join @($ReSkate, '.git', 'rebase-merge')))) { return $round -gt 0 }
        $subject = Capture git @('log', '-1', '--format=%s', 'REBASE_HEAD')
        if (-not $subject -or -not $subject.StartsWith('Replay Shake:')) { return $false }
        $conflicted = @((Capture git @('diff', '--name-only', '--diff-filter=U')) -split "`n" | Where-Object { $_ })
        foreach ($file in $conflicted) {
            if (-not @($fits | Where-Object { $_.File -ceq $file }).Count) { return $false }
            $moving = @((Capture git @('show', "REBASE_HEAD:$file")) -split "`n")
            if ((Run git @('checkout', '--ours', '--', $file)) -ne 0 -or -not (Fit-File $file $moving) -or
                -not (Test-Fitted $file) -or (Run git @('add', '--', $file)) -ne 0) { return $false }
            Say "Put Replay Shake's lines back into the new ReSkate's $file."
        }
        if ((Run git @('diff', '--cached', '--quiet')) -ne 0 -and (Run git @('commit', '--quiet', '--no-verify', '-C', 'REBASE_HEAD')) -ne 0) {
            return $false
        }
        Run git @('rebase', '--continue') | Out-Null
    }
    return $false
}

# Where things are, checked and remembered before anything takes long: a run that stops part way
# need not be told again.
$patchFile = ''
if ($Patch) {
    if (-not (Test-Path -LiteralPath $Patch -PathType Leaf)) { Fail "There is no patch $Patch." }
    $patchFile = (Resolve-Path -LiteralPath $Patch).Path
}
# The extra patches: those given; else the known ones beside this script; none with -NoExtra.
$extraFiles = @()
if (-not $NoExtra) {
    $given = @($Extra | Where-Object { $_ })
    if (-not $given.Count) {
        foreach ($kind in $extraKinds) {
            $beside = Join @($PSScriptRoot, $kind.File)
            if (Test-Path -LiteralPath $beside -PathType Leaf) { $given += $beside }
        }
    }
    foreach ($file in $given) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { Fail "There is no patch $file." }
        $resolved = (Resolve-Path -LiteralPath $file).Path
        if (-not (Get-ExtraKind (Get-PatchSubject $resolved))) {
            Fail "$file is not one of the patches this script puts on Replay Shake (stick-pops.patch)."
        }
        $extraFiles += $resolved
    }
}
Assert-Skate
Assert-NotSteamLibrary
Remember 'skate' $Skate
Remember 'reskate' $ReSkate
Save-State

# The first time: ReSkate, and Replay Shake on a branch of its own.
$patched = $false
if (-not (Test-Path -LiteralPath (Join @($ReSkate, '.git')))) {
    if (-not $Patch) { Fail "There is no ReSkate checkout in $ReSkate. The first time, give the patch: -Patch replay-shake.patch" }
    Say "Getting ReSkate into $ReSkate ..."
    if ((Run git @('clone', $Upstream, $ReSkate)) -ne 0) { Fail 'Could not get ReSkate (above).' }
}
# git needs a name for the commits it makes moving Replay Shake along: when you have none set, this
# checkout gets one of its own (by hand too, should a move need fitting).
foreach ($setting in @(@('user.name', 'Replay Shake'), @('user.email', 'replay-shake@localhost'))) {
    if (-not (Capture git @('-C', $ReSkate, 'config', $setting[0]))) {
        Run git @('-C', $ReSkate, 'config', $setting[0], $setting[1]) | Out-Null
    }
}
$shakeCMake = Join @($ReSkate, 'Extension', 'ReplayShake', 'ReplayShake.cmake')
if (-not (Test-Path -LiteralPath $shakeCMake)) {
    $dirty = Capture git @('-C', $ReSkate, 'status', '--porcelain', '--untracked-files=no')
    if ($dirty) { Fail "$ReSkate has changes that are not committed; commit or undo them first." }
    # Replay Shake's branch, should another be checked out; else a new one with the patch on it.
    $exists = Capture git @('-C', $ReSkate, 'rev-parse', '--verify', '--quiet', "refs/heads/$branch")
    if ($exists -and (Run git @('-C', $ReSkate, 'checkout', $branch)) -ne 0) {
        Fail "Could not switch $ReSkate to the branch '$branch' (above)."
    }
    if (-not (Test-Path -LiteralPath $shakeCMake)) {
        if (-not $patchFile) { Fail "$ReSkate has no Replay Shake yet: give the patch, -Patch replay-shake.patch" }
        Say "Putting Replay Shake on the branch '$branch' ..."
        # Onto the ReSkate the patch was made on (its base-commit line), from where it moves onto the
        # newest ReSkate below like any update; else straight onto the newest.
        $start = @()
        if (-not $NoFetch) { Get-Wanted }
        $made = Select-String -LiteralPath $patchFile -Pattern '^base-commit: ([0-9a-f]{40})' | Select-Object -First 1
        if ($made) {
            $madeOn = $made.Matches[0].Groups[1].Value
            if ($null -eq (Capture git @('-C', $ReSkate, 'cat-file', '-e', "$madeOn^{commit}")) -and -not $NoFetch) {
                Run git @('-C', $ReSkate, 'fetch', '--no-tags', $wantedFrom, $madeOn) | Out-Null
            }
            if ($null -ne (Capture git @('-C', $ReSkate, 'cat-file', '-e', "$madeOn^{commit}"))) { $start = @($madeOn) }
        }
        if (-not $exists -and (Run git (@('-C', $ReSkate, 'checkout', '-b', $branch) + $start)) -ne 0) {
            Fail "Could not make the branch '$branch' (above)."
        }
        if ((Run git @('-C', $ReSkate, 'am', '--3way', $patchFile)) -ne 0) {
            Run git @('-C', $ReSkate, 'am', '--abort') | Out-Null
            Fail ("The patch does not go onto this ReSkate (above). Nothing was changed: ask for a patch made for " +
                "this version of ReSkate.") 2
        }
        $patched = $true
    }
    $Force = $true
}

Push-Location -LiteralPath $ReSkate
# The process's own folder too: git cannot take away a folder a program is in, and a move onto a new
# ReSkate takes this script's folder away and puts it back.
$startedIn = [System.IO.Directory]::GetCurrentDirectory()
[System.IO.Directory]::SetCurrentDirectory($ReSkate)
try {
    $git = Join @($ReSkate, '.git')
    # A move onto a new ReSkate fitted by hand (README.md, Staying up to date): finish it.
    if (Test-Path -LiteralPath (Join @($git, 'rebase-merge'))) {
        $unmerged = Capture git @('diff', '--name-only', '--diff-filter=U')
        if (-not $unmerged) {
            Say 'Finishing the move onto the new ReSkate ...'
            # The fitted commit keeps its message and author, committed here so that no editor opens
            # to change them.
            if ((Run git @('diff', '--cached', '--quiet')) -ne 0 -and
                (Run git @('commit', '--quiet', '--no-verify', '-C', 'REBASE_HEAD')) -ne 0) {
                Fail 'git could not commit the fitted files (above). Fix what it says and run this again, or git rebase --abort.' 2
            }
            if ((Run git @('rebase', '--continue')) -eq 0) { $Force = $true }
            $unmerged = Capture git @('diff', '--name-only', '--diff-filter=U')
        }
        if (Test-Path -LiteralPath (Join @($git, 'rebase-merge'))) {
            $what = if ($unmerged) { "Still to fit:`n$unmerged" } else { 'git says what it needs above.' }
            Fail ("Replay Shake is part way onto the new ReSkate. $what`nFit each file (README.md, Staying up to date), " +
                'git add it, and run this again; or run git rebase --abort to leave ReSkate as it was.') 2
        }
    }
    foreach ($part in 'rebase-apply', 'MERGE_HEAD', 'CHERRY_PICK_HEAD') {
        if (Test-Path -LiteralPath (Join @($git, $part))) {
            Fail "A git operation is part way in $ReSkate. Finish it, or run: git rebase --abort (or git am --abort)."
        }
    }
    $current = Capture git @('rev-parse', '--abbrev-ref', 'HEAD')
    if ($current -ne $branch) {
        if ((Run git @('checkout', $branch)) -ne 0) { Fail "Could not switch $ReSkate to the branch '$branch' (above)." }
    }
    $dirty = Capture git @('status', '--porcelain', '--untracked-files=no')
    if ($dirty) {
        Fail ("$ReSkate has changes that are not committed:`n$dirty`nCommit them (git commit -am `"my change`") and they " +
            'come along with every update, or undo them (git checkout -- .).')
    }
    # The ReSkate Replay Shake goes onto (-On): its newest release unless told otherwise, fetched with
    # its tags first (and what -Fetch names).
    if (-not $NoFetch) {
        Say 'Looking for a new ReSkate ...'
        if ((Run git @('fetch', '--tags', 'origin')) -ne 0) { Fail 'Could not reach ReSkate on GitHub (above).' }
        Get-Wanted
    }
    $mainline = Get-Base $On
    if (-not $mainline) { Fail "There is no ReSkate '$On' in $ReSkate to build on (git tag lists the releases)." }
    if ($patchFile -and -not $patched -and (Update-ReplayShake $patchFile $mainline)) { $Force = $true }
    if (Update-Extras $extraFiles $mainline) { $Force = $true }
    $before = Capture git @('rev-parse', 'HEAD')
    $moved = $false
    # Back to where it was, should anything below fail: what is installed stays as it is.
    function Undo {
        if ($moved) { Run git @('reset', '--hard', $before) | Out-Null }
    }
    # Where Replay Shake's commits begin: the ReSkate under them. Only they, and what came after, move.
    $forkedAt = Get-ShakeBase $mainline
    $target = Capture git @('rev-parse', "$mainline^{commit}")
    if (-not $NoFetch) {
        if ($forkedAt -ne $target) {
            Say "Moving Replay Shake onto ReSkate $(Get-BaseName $mainline) ..."
            $move = if ($forkedAt) { @('rebase', '--onto', $mainline, $forkedAt) } else { @('rebase', $mainline) }
            if ((Run git $move) -ne 0 -and -not (Fit-Rebase)) {
                $conflicts = Capture git @('diff', '--name-only', '--diff-filter=U')
                $stopped = Capture git @('log', '-1', '--format=%s', 'REBASE_HEAD')
                Run git @('rebase', '--abort') | Out-Null
                $extraKind = Get-ExtraKind $stopped
                if ($extraKind) {
                    Fail ("ReSkate changed lines $($extraKind.Name) change too, in:`n$conflicts`n`nNothing was installed: the " +
                        "ReSkate you have keeps working. Make a new $($extraKind.File) for it, or run this " +
                        'with -NoExtra to build Replay Shake without it on the new ReSkate.') 2
                }
                $whose = if ($stopped -and -not $stopped.StartsWith('Replay Shake:')) { "your own commit `"$stopped`" changes" }
                         else { "Replay Shake's go beside" }
                Fail ("ReSkate changed the lines $whose, in:`n$conflicts`n`nNothing was installed: the " +
                    'ReSkate you have keeps working. To fit Replay Shake to the new ReSkate, see README.md, Staying up to ' +
                    "date: it starts with git rebase $mainline in $ReSkate.") 2
            }
            $moved = $true
            if ((Run git @('merge-base', '--is-ancestor', $mainline, 'HEAD')) -ne 0) {
                Undo
                Fail 'Replay Shake could not be moved onto the new ReSkate (above). Nothing was installed.' 2
            }
        } elseif (-not $Force -and (Recall 'installed_commit') -eq $before -and $Skate -and
                  (Test-Path -LiteralPath (Join @($Skate, 'ReSkate.dll')))) {
            if (Test-Installed) {
                Say 'ReSkate has not changed and this build is installed: nothing to do (-Force builds it again).'
                exit 0
            }
            Say "What is in $Skate is not this build (the ReSkate launcher may have put a release there): putting it back ..."
        }
    }

    $cmake = Find-CMake
    $ctest = Join @((Split-Path -Parent $cmake), ('ctest' + [System.IO.Path]::GetExtension($cmake)))
    $build = Join @($ReSkate, 'build', 'vs2022-x64')
    Say 'Building ReSkate with Replay Shake (the first time takes a while) ...'
    if ((Recall 'configured_with') -ne $cmake) { Clear-Configure $build }
    if ((Run $cmake @('--preset', 'vs2022-x64', '-DDINGOSDK_BUILD_REPLAY_SHAKE_TESTS=ON')) -ne 0) {
        Clear-Configure $build
        Remember 'configured_with' ''
        Save-State
        Undo
        Fail ((Get-ToolsHint) + 'Setting up the build failed (above). Nothing was installed, and ReSkate is back where it was.') 3
    }
    Remember 'configured_with' $cmake
    Save-State
    if ((Run $cmake @('--build', '--preset', 'release', '--parallel')) -ne 0) {
        Undo
        Fail 'The build failed (above). Nothing was installed, and ReSkate is back where it was.' 3
    }
    if (-not $SkipTests) {
        if ((Run $ctest @('--test-dir', $build, '-C', 'Release', '-R', 'replay_shake', '--output-on-failure')) -ne 0) {
            Undo
            Fail "Replay Shake's tests failed (above). Nothing was installed, and ReSkate is back where it was." 3
        }
    }

    Assert-GameClosed
    $release = Join @($build, 'Release')
    foreach ($name in $files) {
        if (-not (Test-Path -LiteralPath (Join @($release, $name)))) { Undo; Fail "The build made no $name in $release." 3 }
    }
    foreach ($name in $files) {
        $target = Join @($Skate, $name)
        if (Test-Path -LiteralPath $target) {
            if (-not (Test-Path -LiteralPath ($target + '.original'))) { Copy-Item -LiteralPath $target -Destination ($target + '.original') }
            Copy-Item -LiteralPath $target -Destination ($target + '.previous') -Force
        }
        Copy-Item -LiteralPath (Join @($release, $name)) -Destination $target -Force
    }

    $commit = Capture git @('rev-parse', 'HEAD')
    # The ReSkate it is built on: its release tag (ReSkate's own, v...: another repository's fetched
    # with -Fetch are not its) and the changes since, and their date.
    $base = Capture git @('merge-base', 'HEAD', $mainline)
    $reskateVersion = 'unknown'
    if ($base) {
        $reskateVersion = '{0} of {1}' -f (Capture git @('describe', '--tags', '--match', 'v[0-9]*', '--always', $base)),
            (Capture git @('log', '-1', '--format=%ad', '--date=short', $base))
    }
    $supported = ''
    $pins = Join @($ReSkate, 'Engine', 'Game', 'Build', 'supported_build.h')
    if (Test-Path -LiteralPath $pins) {
        $found = Select-String -LiteralPath $pins -Pattern 'steam_build_id = "(\d+)"' | Select-Object -First 1
        if ($found) { $supported = $found.Matches[0].Groups[1].Value }
    }
    $previousBuild = Recall 'game_build'
    Remember 'skate' $Skate
    Remember 'reskate' $ReSkate
    Remember 'installed_commit' $commit
    if ($supported) { Remember 'game_build' $supported }
    Save-State

    Say ''
    Say "Installed in $Skate (the ones before are *.previous; -Rollback puts them back)."
    $with = 'Replay Shake'
    foreach ($file in $extraFiles) { $with += ' and ' + (Get-ExtraKind (Get-PatchSubject $file)).Name }
    Say "ReSkate $reskateVersion, with $with."
    if (@($extraFiles | Where-Object { (Get-ExtraKind (Get-PatchSubject $_)).File -eq 'stick-pops.patch' }).Count) {
        Say 'Pop with L3 / R3 is in the ReSkate menu, Addons > GRIND EXITS (and Mod Options > Player).'
    }
    if ($supported -and $previousBuild -and $supported -ne $previousBuild) {
        Say ("This ReSkate runs skate. build $supported (it was $previousBuild): its launcher fetches that build on its next " +
            "start, and Replay Shake asks you to find the replay editor's camera again (README.md, step 2).")
    }
    Say 'Start the game with the ReSkateLauncher.exe there: a build of your own never replaces itself with a release.'
    if ($On -eq 'release') {
        Say 'When ReSkate puts out a new release, run this again: it moves Replay Shake onto that one.'
    } else {
        Say "It is built on the ReSkate -On names ($On): with another -On, it moves Replay Shake onto that one."
    }
    # What the build said about reading the game another way than Replay Shake knows (ReplayShake.cmake).
    $notes = Join @($build, 'replay_shake_notes.txt')
    if ((Test-Path -LiteralPath $notes) -and (Get-Content -LiteralPath $notes -Raw)) {
        Say ''
        Write-Host 'Replay Shake built, with a note:' -ForegroundColor Yellow
        Get-Content -LiteralPath $notes | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
    }
} finally {
    try { [System.IO.Directory]::SetCurrentDirectory($startedIn) } catch { }
    Pop-Location
}
