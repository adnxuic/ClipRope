#requires -Version 7.0
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$env:CLIPROPE_BUILD_SESSION -or !(Test-Path -LiteralPath $env:CLIPROPE_BUILD_SESSION)) {
    throw 'Run package registration checks through tools/build.ps1 -Action Test.'
}
. (Join-Path $root 'packaging/windows/nsis.ps1')
$compiler = Get-ClipRopeNsis $root
$id = [Guid]::NewGuid().ToString('N')
$fixture = Join-Path ([IO.Path]::GetTempPath()) "ClipRope.Package-$id Unicode 卸载"
$namespace = "Software\ClipRope.PackageTest.$id"
$reg = [Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser','Registry64')
[void][IO.Directory]::CreateDirectory($fixture)
try {
    & $compiler /V2 "/DFIXTURE=$fixture" "/DREGISTRY=$namespace" "/DSOURCE=$root" (Join-Path $PSScriptRoot 'package_registration.nsi')
    if ($LASTEXITCODE) { throw 'Fixture compilation failed.' }
    $p = Start-Process (Join-Path $fixture 'Setup.exe') -WindowStyle Hidden -Wait -PassThru
    if ($p.ExitCode) { throw 'Fixture uninstaller generation failed.' }
    function Put([string]$key, [string]$name, [string]$value) {
        $item = $reg.CreateSubKey("$namespace\$key")
        try { $item.SetValue($name, $value) } finally { $item.Dispose() }
    }
    function Read([string]$key, [string]$name) {
        $item = $reg.OpenSubKey("$namespace\$key")
        if ($item) { try { $item.GetValue($name) } finally { $item.Dispose() } }
    }
    $installed = Join-Path $fixture 'installed'
    $owned = "$installed\current\ClipRope.exe"
    $foreign = 'C:\foreign\ClipRope.exe'
    $cases = @(
        @{Name='disabled'; Owner=$owned},
        @{Name='missing-startup'},
        @{Name='locked-shortcut'; Owner=$owned; LockedShortcut=$true; Exit=4},
        @{Name='brief-shortcut-lock'; Owner=$owned; LockedShortcut=$true; ReleaseShortcut=$true},
        @{Name='case-insensitive-owner'; Owner=$owned.ToLowerInvariant()},
        @{Name='enabled'; Owner=$owned; Run='"'+$owned+'"'},
        @{Name='missing-owner'; Run='"'+$owned+'"'},
        @{Name='foreign-run'; Owner=$owned; Run='"'+$foreign+'"'; Foreign=$true},
        @{Name='foreign-owner-disabled'; Owner=$foreign; Foreign=$true},
        @{Name='foreign-owner-owned-run'; Owner=$foreign; Run='"'+$owned+'"'; Foreign=$true},
        @{Name='foreign-both'; Owner=$foreign; Run='"'+$foreign+'"'; Foreign=$true},
        @{Name='purge-owned'; Owner=$owned; Purge=$true},
        @{Name='purge-foreign'; Owner=$foreign; Purge=$true; Foreign=$true; Exit=4},
        @{Name='purge-foreign-run'; Owner=$owned; Run='"'+$foreign+'"'; Purge=$true; Foreign=$true; Exit=4},
        @{Name='foreign-product'; Owner=$owned; Product='C:\foreign'; Exit=4},
        @{Name='owner-changes-before-purge'; Owner=$owned; Purge=$true; ChangeOwner=1; Foreign=$true; Exit=4},
        @{Name='owner-changes-before-removal'; Owner=$owned; ChangeBeforeRemoval=1}
    )
    foreach ($case in $cases) {
        $reg.DeleteSubKeyTree($namespace, $false)
        if ([IO.Path]::GetFullPath($installed) -ne (Join-Path ([IO.Path]::GetFullPath($fixture)) 'installed')) {
            throw 'Unsafe installed fixture path.'
        }
        if (Test-Path -LiteralPath $installed) { Remove-Item -LiteralPath $installed -Recurse -Force }
        foreach ($slot in @('current','previous','incoming')) {
            $directory = Join-Path $installed $slot
            [void][IO.Directory]::CreateDirectory($directory)
            foreach ($file in @('ClipRope.exe','ClipRopeWorker.exe','ClipRopePreviewWorker.exe','ClipRopeTargetWorker.exe','cliprope_headless.exe',
                                'Uninstall.exe','ThirdPartyNotices.txt')) {
                [IO.File]::WriteAllText((Join-Path $directory $file), 'Synthetic fixture; not executable.')
            }
            [IO.File]::WriteAllText((Join-Path $directory 'release.ini'), "[ClipRope]`r`nVersion=fixture`r`n")
        }
        [IO.File]::WriteAllText((Join-Path $fixture 'history.txt'), 'Retained synthetic history')
        [IO.File]::WriteAllText((Join-Path $fixture 'ClipRope.lnk'), 'Synthetic shortcut')
        [IO.File]::WriteAllText((Join-Path $fixture 'actions.txt'), '')
        [IO.File]::WriteAllText((Join-Path $fixture 'case.ini'), "[Fixture]`r`nChangeOwner=$($case.ChangeOwner)`r`nChangeBeforeRemoval=$($case.ChangeBeforeRemoval)`r`n")
        Put 'Product' 'InstallLocation' $(if ($case.Product) { $case.Product } else { $installed })
        Put 'Startup' 'Unrelated' 'keep'
        Put 'Run' 'Unrelated' 'keep'
        if ($case.Owner) { Put 'Startup' 'Owner' $case.Owner }
        if ($case.Run) { Put 'Run' 'ClipRope' $case.Run }
        $purge = if ($case.Purge) { '/PURGEHISTORY' } else { '' }
        $lockedShortcut = if ($case.LockedShortcut) {
            [IO.File]::Open((Join-Path $fixture 'ClipRope.lnk'),'Open','Read','Read')
        }
        try {
            $p = Start-Process (Join-Path $fixture 'Uninstall.exe') -ArgumentList "/S $purge _?=$installed\current" -WindowStyle Hidden -PassThru
            if ($case.ReleaseShortcut) {
                $until = [datetime]::UtcNow.AddSeconds(5)
                while (!(Get-Content -LiteralPath (Join-Path $fixture 'actions.txt') -Raw) -and [datetime]::UtcNow -lt $until) {
                    Start-Sleep -Milliseconds 10
                }
                Start-Sleep -Milliseconds 200
                $lockedShortcut.Dispose()
                $lockedShortcut = $null
            }
            if (!$p.WaitForExit(30000)) { throw "Uninstaller timed out in $($case.Name)." }
        } finally {
            if ($lockedShortcut) { $lockedShortcut.Dispose() }
        }
        $expected = if ($case.Exit) { $case.Exit } else { 0 }
        if ($p.ExitCode -ne $expected) { throw "$($case.Name): expected $expected, observed $($p.ExitCode)." }
        $actions = [IO.File]::ReadAllText((Join-Path $fixture 'actions.txt'))
        if ($expected) {
            if (!(Test-Path -LiteralPath $owned) -or !(Read 'Product' 'InstallLocation') -or
                !(Test-Path -LiteralPath (Join-Path $fixture 'history.txt')) -or
                $actions.Contains('--confirm-delete-installed-history') -or $actions.Contains('--remove-preview-container')) {
                throw "$($case.Name): rejected uninstall changed payload, registration or shared data."
            }
        } else {
            if ((Test-Path -LiteralPath $installed) -or (Test-Path -LiteralPath (Join-Path $fixture 'ClipRope.lnk')) -or
                (Read 'Product' 'InstallLocation')) { throw "$($case.Name): installed files or registration remain." }
            $wantedOwner = if ($case.Owner -eq $foreign -or $case.ChangeBeforeRemoval) { $foreign } else { $null }
            $wantedRun = if ($case.Run -eq ('"'+$foreign+'"') -or $case.ChangeBeforeRemoval) { '"'+$foreign+'"' } else { $null }
            if ((Read 'Startup' 'Owner') -ne $wantedOwner -or (Read 'Run' 'ClipRope') -ne $wantedRun) {
                throw "$($case.Name): foreign values were changed or owned values remain."
            }
            if ($case.Foreign -and $actions.Contains('--remove-preview-container')) { throw 'Foreign preview container removed.' }
            if ((Test-Path -LiteralPath (Join-Path $fixture 'history.txt')) -eq [bool]$case.Purge) { throw 'History retention differs.' }
        }
        if ((Read 'Startup' 'Unrelated') -ne 'keep' -or (Read 'Run' 'Unrelated') -ne 'keep') { throw 'Unrelated registration changed.' }
        Write-Output "$($case.Name) passed ($expected)."
    }
    $reg.DeleteSubKeyTree($namespace, $false)
    $directoryFixture = Join-Path $fixture 'directory'
    [void][IO.Directory]::CreateDirectory($directoryFixture)
    & python (Join-Path $PSScriptRoot 'package_directory.py') $compiler $directoryFixture $namespace
    if ($LASTEXITCODE) { throw 'Installation folder regression failed.' }
} finally {
    $reg.DeleteSubKeyTree($namespace, $false)
    $reg.Dispose()
    if ($fixture -ne [IO.Path]::GetFullPath($fixture) -or !(Split-Path $fixture -Leaf).StartsWith("ClipRope.Package-$id ") -or
        (Split-Path $fixture -Parent) -ne [IO.Path]::GetTempPath().TrimEnd('\')) { throw 'Unsafe package fixture cleanup.' }
    Remove-Item -LiteralPath $fixture -Recurse -Force
}
