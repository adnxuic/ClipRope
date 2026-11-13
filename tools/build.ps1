#requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Build','Test','CleanRebuild','RunDev','Package','Publish','Verify')][string]$Action = 'Build',
    [ValidateSet('Production','Qualification')][string]$Variant = 'Production',
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [string]$TestRegex = '^(BUILD-|P1-(01|02|03|04|07)[.]|I18N-01[.]|UI-18[.]|UNINSTALL-01[.])',
    [string[]]$RequiredTests = @(),
    [switch]$Interactive,
    [switch]$CI,
    [ValidatePattern('^cliprope_[a-z0-9_]+$')][string[]]$Targets = @(),
    [string]$BackupArchive,
    [ValidateSet('Zlib','SolidLzma')][string]$PackageCompressor
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $root 'build'
$app = Join-Path $build 'app'
$localArtifacts = Join-Path $root '.local-artifacts'
$helper = Join-Path $PSScriptRoot 'build_artifacts.py'
$payload = @('ClipRope.exe','ClipRopeWorker.exe','ClipRopePreviewWorker.exe','ClipRopeTargetWorker.exe','cliprope_headless.exe',
             'cliprope_profile_support.exe','Uninstall.exe','ClipRope.build.ini')
$coreTests = '^(BUILD-|P1-(01|02|03|04|07)[.]|I18N-01[.]|UI-18[.]|UNINSTALL-01[.])'
$lock = $null
$session = $null
$oldSession = $env:CLIPROPE_BUILD_SESSION

function Invoke-Checked([string]$Command, [string[]]$Arguments) {
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE." }
}
function Read-Helper([string[]]$Arguments) {
    $value = & python $helper @Arguments
    if ($LASTEXITCODE -ne 0) { throw 'Artifact verification failed.' }
    return ($value -join "`n" | ConvertFrom-Json)
}
function Save-Json($Value, [string]$Path) {
    $Value | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $Path -Encoding utf8
}
function Assert-BuildPath([string]$Path) {
    $resolved = [IO.Path]::GetFullPath($Path)
    if ($resolved -ne $build -and !$resolved.StartsWith($build + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside this checkout's build directory: $resolved"
    }
    for ($part = [IO.DirectoryInfo]::new($resolved); $null -ne $part; $part = $part.Parent) {
        if ($part.Exists -and ($part.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Linked path: $($part.FullName)" }
    }
    return $resolved
}
function Get-DailyExecutable {
    $user = [Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser', 'Registry64')
    try {
        $key = $user.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Uninstall\ClipRope')
        if ($key) {
            try {
                $location = $key.GetValue('InstallLocation')
                if (!$location -or ![IO.Path]::IsPathFullyQualified($location)) { throw 'Invalid registered installation folder.' }
                return Join-Path $location 'current/ClipRope.exe'
            } finally { $key.Dispose() }
        }
    } finally { $user.Dispose() }
    return Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Programs/ClipRope/current/ClipRope.exe'
}
function Assert-Idle([switch]$IncludeDaily) {
    if ($IncludeDaily) { $daily = Get-DailyExecutable }
    foreach ($process in Get-Process) {
        $path = $process.Path
        if (!$path) {
            if ($process.ProcessName -in @('ClipRope','ClipRopeWorker','ClipRopePreviewWorker','ClipRopeTargetWorker')) {
                throw "Cannot inspect ClipRope process $($process.Id); no files were replaced."
            }
            continue
        }
        if ($path.StartsWith($build + '\', [StringComparison]::OrdinalIgnoreCase) -or
            ($IncludeDaily -and $path.StartsWith((Split-Path (Split-Path $daily)) + '\', [StringComparison]::OrdinalIgnoreCase))) {
            throw "Exit the application before this operation: $path (PID $($process.Id))."
        }
    }
}
function Assert-WritablePayload {
    foreach ($name in $payload) {
        $path = Join-Path $app $name
        if (Test-Path -LiteralPath $path) {
            try {
                $handle = [IO.File]::Open($path, 'Open', 'ReadWrite', 'None')
                $handle.Dispose()
            } catch { throw "Runtime file is occupied or not writable: $path" }
        }
    }
}
function Find-CMake {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or !$vs) { throw 'Visual Studio 2022 C++ tools are required.' }
    $script:cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    $script:ctest = Join-Path (Split-Path $script:cmake) 'ctest.exe'
    if (!(Test-Path -LiteralPath $script:cmake)) { throw 'Visual Studio CMake is unavailable.' }
}
function Build-RuntimeCore([string]$Kind, [string]$Config) {
    [void](Read-Helper @('inventory','--allow-empty'))
    Assert-Idle
    Assert-WritablePayload
    $script:runtimeMutationStarted = $true
    $before = Read-Helper @('fingerprint')
    $receiptPath = Join-Path $build 'build-receipt.json'
    $prior = if (Test-Path -LiteralPath $receiptPath) { Get-Content -Raw -LiteralPath $receiptPath | ConvertFrom-Json } else { $null }
    $switching = !$prior -or $prior.variant -ne $Kind -or $prior.configuration -ne $Config
    # Never retain a valid receipt across a failed compilation.
    if (Test-Path -LiteralPath $receiptPath) { Remove-Item -LiteralPath (Assert-BuildPath $receiptPath) }
    $preset = if ($CI) { 'windows-ci' } elseif ($Kind -eq 'Qualification') { 'windows-qualification' } else { 'windows-rc' }
    $testing = if ($Kind -eq 'Qualification') { 'ON' } else { 'OFF' }
    $input = if ($Kind -eq 'Qualification' -and $Interactive) { 'ON' } else { 'OFF' }
    $configure = @('--preset',$preset,"-DBUILD_TESTING:BOOL=$testing","-DCLIPROPE_DESKTOP_PROBES:BOOL=$testing",
        "-DCLIPROPE_TEST_DESKTOP_INPUT:BOOL=$input","-DCLIPROPE_TEST_TRAY_ICON:BOOL=$input",
        "-DCLIPROPE_TEST_OFFICE:BOOL=$input","-DCLIPROPE_TEST_OFFICE_MATRIX:BOOL=$input")
    Invoke-Checked $script:cmake $configure
    if ($switching) {
        # MSBuild's other configuration can consider shared output timestamps current.
        # Removing its small runtime set forces relinking without discarding object caches.
        foreach ($file in Get-ChildItem -LiteralPath $app -File -Filter '*.exe' -ErrorAction SilentlyContinue) {
            Remove-Item -LiteralPath (Assert-BuildPath $file.FullName)
        }
    }
    $arguments = @('--build',(Join-Path $build 'cmake'),'--config',$Config)
    if ($Kind -eq 'Production') { $arguments += @('--target','cliprope_desktop','cliprope_headless','cliprope_profile_support') }
    $arguments += @('--','/m:2','/nr:false')
    Invoke-Checked $script:cmake $arguments
    if ($Kind -eq 'Qualification') {
        Invoke-Checked $script:cmake @('--build',(Join-Path $build 'cmake'),'--config',$Config,'--target','cliprope_profile_support','--','/m:2','/nr:false')
    }
    if ($Targets.Count) {
        Invoke-Checked $script:cmake (@('--build',(Join-Path $build 'cmake'),'--config',$Config,'--target') + $Targets + @('--','/m:2','/nr:false'))
    }
    if ((Read-Helper @('fingerprint')) -ne $before) { throw 'Sources changed during the build; rebuild before verification.' }
    $receipt = Read-Helper @('receipt','--variant',$Kind,'--configuration',$Config)
    Save-Json $receipt $receiptPath
    Write-Host "Verified $Kind $Config runtime: $app\ClipRope.exe"
}
function Get-RecoveryPayload($Receipt) {
    $names = @($Receipt.payload.PSObject.Properties.Name)
    $expected = @($payload)
    if ('ClipRopeTargetWorker.exe' -notin $names) {
        $expected = @($payload | Where-Object { $_ -ne 'ClipRopeTargetWorker.exe' })
    }
    if (Compare-Object $expected $names) { throw 'Unrecognized production recovery payload.' }
    return $expected
}
function Build-Runtime([string]$Kind, [string]$Config) {
    $script:runtimeMutationStarted = $false
    $rollback = Join-Path $localArtifacts 'last-production.zip'
    $receiptPath = Join-Path $build 'build-receipt.json'
    if (Test-Path -LiteralPath $receiptPath) {
        $saved = Get-Content -Raw -LiteralPath $receiptPath | ConvertFrom-Json
        if ($saved.variant -eq 'Production' -and $saved.configuration -eq 'Release') {
            $savedNames = @(Get-RecoveryPayload $saved)
            $intact = $true
            foreach ($name in $savedNames) {
                $file = Join-Path $app $name
                if (!(Test-Path -LiteralPath $file) -or (Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() -ne $saved.payload.$name) { $intact = $false; break }
            }
            if ($intact) {
                $zip = [IO.Compression.ZipFile]::Open($rollback + '.pending', 'Create')
                try {
                    foreach ($name in $savedNames) { [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,(Join-Path $app $name),$name) }
                    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$receiptPath,'build-receipt.json')
                } finally { $zip.Dispose() }
                Move-Item -LiteralPath ($rollback + '.pending') -Destination $rollback -Force
            }
        }
    }
    try { Build-RuntimeCore $Kind $Config }
    catch {
        $failure = $_
        if ($script:runtimeMutationStarted -and (Test-Path -LiteralPath $rollback)) {
            Assert-Idle
            Assert-WritablePayload
            [void](Assert-BuildPath $app)
            $zip = [IO.Compression.ZipFile]::OpenRead($rollback)
            try {
                $reader = [IO.StreamReader]::new($zip.GetEntry('build-receipt.json').Open())
                try { $backup = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
                $restoreNames = @(Get-RecoveryPayload $backup)
                foreach ($name in $restoreNames) {
                    if (!$zip.GetEntry($name)) { throw "Incomplete rollback archive: $name" }
                }
                foreach ($name in $restoreNames) {
                    $entry = $zip.GetEntry($name)
                    [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,(Join-Path $app $name),$true)
                }
                foreach ($name in $payload) {
                    $unused = Assert-BuildPath (Join-Path $app $name)
                    if ($name -notin $restoreNames -and (Test-Path -LiteralPath $unused)) { Remove-Item -LiteralPath $unused }
                }
            } finally { $zip.Dispose() }
            # Retained bytes are a recovery copy, not a receipt for the failed source/configuration.
            if (Test-Path -LiteralPath $receiptPath) { Remove-Item -LiteralPath (Assert-BuildPath $receiptPath) }
            Write-Warning 'Restored the last production payload; rebuild and retest before publishing.'
        }
        if ($script:runtimeMutationStarted) {
            Save-Json ([ordered]@{status='failed';source_fingerprint=(Read-Helper @('fingerprint'));reason='build failure'}) (Join-Path $build 'test-receipt.json')
        }
        throw $failure
    }
}
function Test-Runtime {
    if ($Interactive) { Assert-Idle -IncludeDaily }
    $requiredCases = @((@('AR3.history_link', 'AR3.history_closure') + $RequiredTests) | Select-Object -Unique)
    $fingerprint = Read-Helper @('fingerprint')
    $report = [ordered]@{status='failed';source_fingerprint=$fingerprint;regex=$coreTests;interactive=[bool]$Interactive;utc=[DateTime]::UtcNow.ToString('o')}
    Save-Json $report (Join-Path $build 'test-receipt.json')
    $failed = $null
    try {
        Build-Runtime 'Qualification' $Configuration
        $report.qualification = Get-Content -Raw -LiteralPath (Join-Path $build 'build-receipt.json') | ConvertFrom-Json
        # The minimum regression gate is mandatory even for a narrower user filter.
        $selection = "($coreTests)|($TestRegex)"
        $exactNames = ($requiredCases | ForEach-Object { [regex]::Escape($_) }) -join '|'
        $selection += "|^($exactNames)$"
        $report.regex = $selection
        $resultsPath = Join-Path $build 'test-results.xml'
        if (Test-Path -LiteralPath $resultsPath) { Remove-Item -LiteralPath $resultsPath }
        Invoke-Checked $script:ctest @('--test-dir',(Join-Path $build 'cmake'),'-C',$Configuration,
            '-R',$selection,'--output-on-failure','--no-tests=error','--output-junit',$resultsPath)
        [xml]$junit = Get-Content -Raw -LiteralPath $resultsPath
        $passed = @($junit.SelectNodes('//testcase[@status="run" and not(failure or error or skipped)]') |
            ForEach-Object { $_.GetAttribute('name') })
        foreach ($name in $requiredCases) {
            if ($name -cnotin $passed) { throw "Required test did not pass: $name" }
        }
        if ((Read-Helper @('fingerprint')) -ne $fingerprint) { throw 'Sources changed while tests were running.' }
        $report.status = 'passed'
    } catch { $failed = $_; $report.status = 'failed' }
    finally {
        # Restore the non-probe runtime even when tests fail. A restoration failure is fatal.
        try { Build-Runtime 'Production' 'Release' }
        catch { $report.status = 'failed'; $failed = $_ }
        Save-Json $report (Join-Path $build 'test-receipt.json')
    }
    if ($failed) { throw $failed }
    if (!$CI) { [void](Read-Helper @('verify','--publish')) }
}
function Clean-Build {
    Assert-Idle
    if (Test-Path -LiteralPath $build) {
        [void](Assert-BuildPath $build)
        if ($BackupArchive) {
            Invoke-Checked python @($helper,'check-archive','--destination',([IO.Path]::GetFullPath($BackupArchive)))
        } else {
            $archive = Join-Path $localArtifacts ('build-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '.zip')
            Invoke-Checked python @($helper,'archive','--destination',$archive)
        }
        # Backup checks every file and rejects reparse points. Repeat immediately before removal.
        Assert-Idle
        Invoke-Checked python @($helper,'ordinary')
        foreach ($file in Get-ChildItem -LiteralPath $build -Recurse -File -Force) {
            $handle = [IO.File]::Open($file.FullName, 'Open', 'Read', 'None')
            $handle.Dispose()
        }
        $target = Assert-BuildPath $build
        Remove-Item -LiteralPath $target -Recurse -Force
    }
    [void][IO.Directory]::CreateDirectory($build)
    Build-Runtime 'Production' 'Release'
}
function New-VerifiedPackage($Receipt) {
    $options = @{
        Binaries = $app
        Support = Join-Path $app 'cliprope_profile_support.exe'
        Revision = $Receipt.revision
    }
    if ($PackageCompressor) {
        $options.Compressor = $PackageCompressor
        $options.OutputDirectory = Join-Path $build "packages/$PackageCompressor"
    }
    & (Join-Path $root 'packaging/windows/package.ps1') @options
    if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
}
function Publish-Runtime {
    Assert-Idle -IncludeDaily
    $receipt = Read-Helper @('verify','--publish')
    $runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    $saved = Get-ItemProperty -LiteralPath $runKey
    $startup = $saved.PSObject.Properties['ClipRope']
    $startupOption = if ($startup -and $startup.Value) { '/STARTUP' } else { '/NOSTARTUP' }
    # package.ps1 repeats receipt validation; production packaging never copies a loose desktop.
    New-VerifiedPackage $receipt
    $installer = Join-Path $build 'packages/ClipRope-0.1.0-windows-x64.exe'
    Assert-Idle -IncludeDaily
    $process = Start-Process -FilePath $installer -ArgumentList @('/S',$startupOption) -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "Installer failed with exit code $($process.ExitCode); no successful publish was recorded." }
    $daily = Get-DailyExecutable
    foreach ($name in @('ClipRope.exe','ClipRopeWorker.exe','ClipRopePreviewWorker.exe','ClipRopeTargetWorker.exe','cliprope_headless.exe')) {
        if ((Get-FileHash -LiteralPath (Join-Path (Split-Path $daily) $name)).Hash.ToLowerInvariant() -ne $receipt.payload.$name) {
            throw "Installed payload differs from the verified build: $name"
        }
    }
    $owner = (Get-ItemProperty -LiteralPath 'HKCU:\Software\ClipRope\Startup').Owner
    if ($owner -ne $daily) { throw 'Installed startup ownership did not migrate.' }
    $currentRun = (Get-ItemProperty -LiteralPath $runKey).PSObject.Properties['ClipRope']
    if (($startupOption -eq '/NOSTARTUP' -and $currentRun) -or
        ($startupOption -eq '/STARTUP' -and (!$currentRun -or $currentRun.Value -ne ('"' + $daily + '"')))) { throw 'Startup preference changed unexpectedly.' }
    Save-Json ([ordered]@{utc=[DateTime]::UtcNow.ToString('o');installed=$daily;receipt=$receipt;startup=$startupOption}) (Join-Path $build 'publish-receipt.json')
    Write-Host "Published daily installation: $daily"
}

Push-Location $root
try {
    if ($PackageCompressor -and $Action -ne 'Package') { throw '-PackageCompressor requires -Action Package.' }
    [void][IO.Directory]::CreateDirectory($localArtifacts)
    try { $lock = [IO.File]::Open((Join-Path $localArtifacts 'build.lock'), 'OpenOrCreate', 'ReadWrite', 'None') }
    catch { throw 'Another build, cleanup, test or publication owns this checkout. Try again after it finishes.' }
    $session = Join-Path $localArtifacts ('session-' + [Guid]::NewGuid().ToString('N'))
    [IO.File]::WriteAllText($session, [string]$PID)
    $env:CLIPROPE_BUILD_SESSION = $session
    if ($Action -ne 'Package') { Find-CMake }
    if ($Action -ne 'CleanRebuild') { [void](Read-Helper @('inventory','--allow-empty')) }
    if ($Action -in @('Build','Test','CleanRebuild')) { [void][IO.Directory]::CreateDirectory($build) }
    switch ($Action) {
        'Build' { Build-Runtime $Variant $Configuration }
        'Test' { Test-Runtime }
        'CleanRebuild' { Clean-Build }
        'Verify' { [void](Read-Helper @('verify')); Write-Host 'Unique runtime and build receipt verified.' }
        'Package' { New-VerifiedPackage (Read-Helper @('verify','--publish')) }
        'Publish' { Publish-Runtime }
        'RunDev' {
            [void](Read-Helper @('verify'))
            Assert-Idle -IncludeDaily
            $profile = Join-Path $build 'test-data/manual'
            [void][IO.Directory]::CreateDirectory($profile)
            $process = Start-Process -FilePath (Join-Path $app 'ClipRope.exe') -ArgumentList @('--profile',('"' + $profile + '"'),'--show') -PassThru -Wait
            if ($process.ExitCode -ne 0) { throw "Development runtime exited with $($process.ExitCode)." }
        }
    }
} finally {
    $env:CLIPROPE_BUILD_SESSION = $oldSession
    if ($session -and (Test-Path -LiteralPath $session)) { Remove-Item -LiteralPath $session }
    if ($lock) { $lock.Dispose() }
    Pop-Location
}
