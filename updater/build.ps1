# Builds ReplayPlusUpdater.exe with the C# compiler that comes with Windows' .NET Framework 4 (nothing to install).
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
& $csc /nologo /target:winexe /optimize+ /out:ReplayPlusUpdater.exe /win32manifest:app.manifest /win32icon:icon.ico `
    /r:System.Web.Extensions.dll /r:System.Windows.Forms.dll /r:System.Drawing.dll ReplayPlusUpdater.cs
if ($LASTEXITCODE -ne 0) { "BUILD FAILED ($LASTEXITCODE)"; exit 1 }
$exe = Get-Item ReplayPlusUpdater.exe
"built $($exe.Name), $($exe.Length) bytes, sha256 $((Get-FileHash $exe).Hash.ToLower())"
