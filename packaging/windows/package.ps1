param(
    [Parameter(Mandatory)][string]$Binaries,
    [Parameter(Mandatory)][string]$Support,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{7,40}$')][string]$Revision,
    [ValidatePattern('^[0-9][0-9A-Za-z.-]{0,36}$')][string]$Version = '0.1.0',
    [switch]$Predecessor,
    [string]$OutputDirectory,
    [ValidateSet('Zlib','SolidLzma')][string]$Compressor = 'SolidLzma'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$env:CLIPROPE_BUILD_SESSION -or !(Test-Path -LiteralPath $env:CLIPROPE_BUILD_SESSION)) {
    throw 'Package through tools/build.ps1 -Action Package or Publish while the checkout lock is held.'
}
if (!$Predecessor) {
    $verified = & python (Join-Path $root 'tools/build_artifacts.py') verify --publish
    if ($LASTEXITCODE) { throw 'Production receipt validation failed.' }
    $receipt = ($verified -join "`n") | ConvertFrom-Json
    if ((Resolve-Path -LiteralPath $Binaries).Path -ne (Join-Path $root 'build/app') -or
        (Resolve-Path -LiteralPath $Support).Path -ne (Join-Path $root 'build/app/cliprope_profile_support.exe') -or
        $Revision -ne $receipt.revision) { throw 'Package inputs must match the canonical verified runtime.' }
}
. (Join-Path $PSScriptRoot 'nsis.ps1')
$compiler = Get-ClipRopeNsis $root
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root 'build/packages' }
[void][IO.Directory]::CreateDirectory($OutputDirectory)
$dest = Join-Path (Resolve-Path $OutputDirectory).Path "ClipRope-$Version-windows-x64.exe"
$bin = (Resolve-Path -LiteralPath $Binaries).Path
$supportExe = (Resolve-Path -LiteralPath $Support).Path
$notices = Join-Path $PSScriptRoot 'ThirdPartyNotices.txt'
$pending = "$dest.pending"
$flags = @('/V2',"/DAPP_BIN=$bin","/DSUPPORT=$supportExe","/DOUTPUT=$pending","/DNOTICES=$notices",
           "/DVERSION=$Version","/DREVISION=$Revision","/DAPP_ICON=$(Join-Path $root 'resources/icons/cliprope.ico')")
if (!$Predecessor) {
    if (!(Test-Path -LiteralPath (Join-Path $bin 'ClipRopeWorker.exe') -PathType Leaf)) { throw 'Capture worker missing.' }
    if (!(Test-Path -LiteralPath (Join-Path $bin 'ClipRopePreviewWorker.exe') -PathType Leaf)) { throw 'Screenshot preview worker missing.' }
    if (!(Test-Path -LiteralPath (Join-Path $bin 'ClipRopeTargetWorker.exe') -PathType Leaf)) { throw 'Target verification helper missing.' }
    $flags += '/DHAS_WORKER'
}
if ($Compressor -eq 'SolidLzma') { $flags += '/DCLIPROPE_SOLID_LZMA' }
try {
    & $compiler @flags (Join-Path $PSScriptRoot 'ClipRope.nsi')
    if ($LASTEXITCODE) { throw "NSIS returned $LASTEXITCODE." }
    [IO.File]::Move($pending, $dest, $true)
} finally {
    if (Test-Path -LiteralPath $pending) { Remove-Item -LiteralPath $pending }
}
Get-FileHash -Algorithm SHA256 -LiteralPath $dest
