#requires -Version 7.0
$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('ClipRope-workflow-' + [Guid]::NewGuid().ToString('N'))
$fixture = [IO.Path]::GetFullPath($fixture)
if (!$fixture.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()),[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe fixture root' }
[void][IO.Directory]::CreateDirectory((Join-Path $fixture 'tools'))
[void][IO.Directory]::CreateDirectory((Join-Path $fixture 'build/app'))
[void][IO.Directory]::CreateDirectory((Join-Path $fixture '.local-artifacts'))
Copy-Item -LiteralPath (Join-Path $source 'tools/build.ps1'),(Join-Path $source 'tools/build_artifacts.py') -Destination (Join-Path $fixture 'tools')
$script = Join-Path $fixture 'tools/build.ps1'
$image = Join-Path $fixture 'build/app/ClipRope.exe'
[IO.File]::WriteAllText($image,'Synthetic image; never executable')
function Reject([string]$Pattern, [string]$Action = 'Build') {
    $result = & pwsh -NoProfile -File $script -Action $Action 2>&1
    if ($LASTEXITCODE -eq 0 -or ($result -join "`n") -notmatch $Pattern) { throw "Expected rejection $Pattern; observed: $result" }
    if ((Get-Content -Raw -LiteralPath $image) -ne 'Synthetic image; never executable') { throw 'Preflight changed the image.' }
}
try {
    $guard = [IO.File]::Open((Join-Path $fixture '.local-artifacts/build.lock'),'OpenOrCreate','ReadWrite','None')
    try {
        Reject 'Another build, cleanup, test or publication' 'Verify'
        Reject 'Another build, cleanup, test or publication' 'Package'
    } finally { $guard.Dispose() }
    Reject 'build-receipt.json' 'Package'
    if (Test-Path -LiteralPath (Join-Path $fixture 'build/packages')) { throw 'A rejected package left installer output.' }
    [void][IO.Directory]::CreateDirectory((Join-Path $fixture 'build/old'))
    [IO.File]::WriteAllText((Join-Path $fixture 'build/old/ClipRope.exe'),'Duplicate')
    Reject 'Expected only build/app/ClipRope.exe'
    Remove-Item -LiteralPath (Join-Path $fixture 'build/old/ClipRope.exe')
    $guard = [IO.File]::Open($image,'Open','Read','Read')
    try { Reject 'Runtime file is occupied or not writable' } finally { $guard.Dispose() }
    Write-Output 'Checkout lock, duplicate output and occupied-file preflights preserved the existing payload.'
} finally {
    if ($fixture -ne [IO.Path]::GetFullPath($fixture) -or !$fixture.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()),[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe fixture cleanup' }
    Remove-Item -LiteralPath $fixture -Recurse -Force
}
