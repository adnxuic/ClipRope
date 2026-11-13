param(
    [Parameter(Mandatory)][string]$OldPackage,
    [Parameter(Mandatory)][string]$Candidate,
    [Parameter(Mandatory)][string]$FixtureBinaries,
    [Parameter(Mandatory)][string]$EvidenceDirectory,
    [ValidatePattern('^[0-9a-f]{40}$')][string]$ExpectedRevision,
    [ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedImageSha256,
    [switch]$UseEmptyCurrentUserInstallation,
    [string]$PredecessorVersion = '0.1.0-preview.1',
    [string]$CompressionBaseline,
    [string]$PayloadReceipt,
    [switch]$InstalledSlotsOnly
)
$ErrorActionPreference='Stop'
if (!$UseEmptyCurrentUserInstallation) { throw 'An explicit empty current-user installation fixture is required.' }
$identity=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if ($identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run in a standard, unelevated session.' }
$local=[Environment]::GetFolderPath('LocalApplicationData')
$store=Join-Path $local 'ClipRope'
$install=Join-Path $local 'Programs/ClipRope'
$current=Join-Path $install 'current'
$program=Join-Path $current 'ClipRope.exe'
$shortcut=Join-Path ([Environment]::GetFolderPath('Programs')) 'ClipRope.lnk'
$old=(Resolve-Path -LiteralPath $OldPackage).Path
$new=(Resolve-Path -LiteralPath $Candidate).Path
$bins=(Resolve-Path -LiteralPath $FixtureBinaries).Path
$key='Software\Microsoft\Windows\CurrentVersion\Uninstall\ClipRope'
function Version {
    $item=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($key)
    if($item) { try { $item.GetValue('DisplayVersion') } finally { $item.Dispose() } }
}
function Startup {
    $item=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run')
    if($item) { try { $item.GetValue('ClipRope') } finally { $item.Dispose() } }
}
$existing=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($key)
$runKey=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run')
$registered=($null -ne $existing) -or ($runKey -and $runKey.GetValueNames() -contains 'ClipRope')
if ($existing) { $existing.Dispose() }
if ($runKey) { $runKey.Dispose() }
if ($registered -or (Test-Path -LiteralPath 'HKCU:\Software\ClipRope\Startup') -or (Test-Path -LiteralPath $install) -or (Test-Path -LiteralPath $store) -or
    (Test-Path -LiteralPath (Join-Path $local 'ClipRope.Settings')) -or
    (Test-Path -LiteralPath $shortcut)) { throw 'An existing installation, history, shortcut or startup value was found. Rehearsal did not begin.' }
if (Test-Path -LiteralPath $EvidenceDirectory) { throw 'Use a new evidence directory.' }
[void][IO.Directory]::CreateDirectory($EvidenceDirectory)
$evidence=(Resolve-Path -LiteralPath $EvidenceDirectory).Path
$receipt=Join-Path $evidence 'history-fingerprint.txt'
$log=Join-Path $evidence 'installation.txt'
function Record([string]$line) { [IO.File]::AppendAllText($log,$line+[Environment]::NewLine); Write-Output $line }
function RunSetup([string]$exe,[string[]]$options) {
    $proc=Start-Process -FilePath $exe -ArgumentList $options -WindowStyle Hidden -Wait -PassThru
    if($proc.ExitCode -ne 0) { throw "Setup failed with exit code $($proc.ExitCode)." }
}
function AssertInstall([string]$wanted,[bool]$startup) {
    if ((Version) -ne $wanted -or !(Test-Path -LiteralPath $program) -or !(Test-Path -LiteralPath $shortcut)) {
        throw 'Installed binary, shortcut or version receipt missing.'
    }
    $setting=Startup
    if (($startup -and $setting -ne ('"'+$program+'"')) -or (!$startup -and $setting)) {
        throw 'Sign-in setting differs from the requested state.'
    }
}
function TestHistory([string]$mode, [string]$applicationDirectory = $current) {
    & (Join-Path $bins 'cliprope_station_test.exe') (Join-Path $bins 'cliprope_installed_history.exe') $mode $applicationDirectory (Join-Path $bins 'cliprope_ole_source.exe') $receipt
    if($LASTEXITCODE) { throw 'Installed resident or history fingerprint verification failed.' }
}
function AssertPayload {
    if (!$PayloadReceipt) { return }
    $expected = (Get-Content -Raw -LiteralPath $PayloadReceipt | ConvertFrom-Json).payload
    $installedNames = @('ClipRope.exe','cliprope_headless.exe','ClipRopeWorker.exe','ClipRopePreviewWorker.exe','ClipRopeTargetWorker.exe')
    $installedNames += @($expected.PSObject.Properties.Name | Where-Object { $_ -like '*.dll' })
    foreach ($name in $installedNames) {
        if ((Get-FileHash -LiteralPath (Join-Path $current $name)).Hash -ne $expected.$name) {
            throw "Installed bytes differ from Release receipt: $name"
        }
    }
}
function SlotFingerprint {
    # Setup refreshes the uninstaller on rollback to preserve cleanup ownership.
    $files = [ordered]@{}
    foreach ($item in (Get-ChildItem -LiteralPath $current -File | Sort-Object Name)) {
        if ($item.Name -ne 'Uninstall.exe') { $files[$item.Name] = (Get-FileHash -LiteralPath $item.FullName).Hash }
    }
    $files | ConvertTo-Json -Compress
}
function Uninstall([bool]$purge) {
    $options=@('/S')
    if($purge) { $options+='/PURGEHISTORY' }
    RunSetup (Join-Path $current 'Uninstall.exe') $options
    $end=[DateTime]::UtcNow.AddSeconds(15)
    do {
        if (!(Version) -and !(Test-Path -LiteralPath $install) -and !(Test-Path -LiteralPath $shortcut) -and !(Startup)) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $end)
    if ((Version) -or (Test-Path -LiteralPath $install) -or (Test-Path -LiteralPath $shortcut) -or (Startup) -or
        (Test-Path -LiteralPath 'HKCU:\Software\ClipRope\Startup')) {
        throw 'Uninstall left an application file or registration.'
    }
}
Record ('Candidate SHA256 '+(Get-FileHash -LiteralPath $new).Hash)
Record ('Predecessor SHA256 '+(Get-FileHash -LiteralPath $old).Hash)
if ($CompressionBaseline) {
    if (!$PayloadReceipt) { throw 'Compression comparison requires the exact binary receipt.' }
    $packages = @{Zlib=(Resolve-Path -LiteralPath $CompressionBaseline).Path;SolidLzma=$new}
    for ($trial = 1; $trial -le 5; ++$trial) {
        $order = if ($trial % 2) { 'Zlib','SolidLzma' } else { 'SolidLzma','Zlib' }
        foreach ($codec in $order) {
            foreach ($operation in 'fresh','upgrade') {
                if ($operation -eq 'upgrade') { RunSetup $old @('/S','/NOSTARTUP') }
                $timer = [Diagnostics.Stopwatch]::StartNew()
                RunSetup $packages[$codec] @('/S','/NOSTARTUP')
                $timer.Stop()
                AssertInstall '0.1.0' $false
                AssertPayload
                [pscustomobject]@{trial=$trial;codec=$codec;operation=$operation;ms=$timer.Elapsed.TotalMilliseconds;
                    installed_bytes=(Get-ChildItem -LiteralPath $current -File | Measure-Object Length -Sum).Sum;
                    package_bytes=(Get-Item -LiteralPath $packages[$codec]).Length} |
                    Export-Csv -NoTypeInformation -Append -LiteralPath (Join-Path $evidence 'setup.csv')
                Uninstall $false
            }
        }
    }
}
RunSetup $old @('/S','/NOSTARTUP')
AssertInstall $PredecessorVersion $false
TestHistory '--seed-empty-installed-profile'
$oldImage=(Get-FileHash -LiteralPath $program).Hash
$oldSlot=SlotFingerprint
$oldBytes=(Get-ChildItem -LiteralPath $current -File -Recurse | Measure-Object Length -Sum).Sum
Record ('Clean predecessor install, real external copy and native restore passed. Startup off. Image '+$oldImage)
RunSetup $new @('/S')
AssertInstall '0.1.0' $false
Record 'Upgrade retained the disabled legacy sign-in setting.'
RunSetup $new @('/S','/ROLLBACK')
AssertInstall $PredecessorVersion $false
RunSetup $new @('/S','/STARTUP')
AssertInstall '0.1.0' $true
$rcImage=(Get-FileHash -LiteralPath $program).Hash
$newBytes=(Get-ChildItem -LiteralPath $current -File -Recurse | Measure-Object Length -Sum).Sum
[pscustomobject]@{predecessor_bytes=$oldBytes;candidate_bytes=$newBytes;
    predecessor_desktop_sha256=$oldImage;candidate_desktop_sha256=$rcImage} |
    ConvertTo-Json | Set-Content -Encoding utf8 -LiteralPath (Join-Path $evidence 'installed-slots.json')
AssertPayload
if($rcImage -eq $oldImage -or !(Test-Path -LiteralPath (Join-Path $current 'ClipRopeWorker.exe'))) { throw 'Upgrade did not replace the application and install its worker.' }
if (!(Test-Path -LiteralPath (Join-Path $current 'ClipRopePreviewWorker.exe'))) { throw 'Upgrade did not install the screenshot preview worker.' }
if (!(Test-Path -LiteralPath (Join-Path $current 'ClipRopeTargetWorker.exe'))) { throw 'Upgrade did not install the target verification helper.' }
if ($ExpectedRevision) {
    $source=Get-Content -LiteralPath (Join-Path $current 'release.ini')
    if ($source -cnotcontains "Revision=$ExpectedRevision") { throw 'Installed source revision disagrees with the release.' }
    Record ("Installed source revision "+$ExpectedRevision)
}
if ($ExpectedImageSha256 -and $rcImage -ne $ExpectedImageSha256) { throw 'Installed application bytes differ from the qualified release binary.' }
TestHistory '--verify-retained-profile'
TestHistory '--check-installed-worker'
Record ('Upgrade retained both identities and payload hashes. Startup opted in. Image '+$rcImage)
$guard=[Threading.Mutex]::new($false,'Global\ClipRope.Package.Change.v1')
try {
    $rejected=Start-Process -FilePath $new -ArgumentList '/S','/ROLLBACK' -WindowStyle Hidden -Wait -PassThru
    if ($rejected.ExitCode -ne 6) { throw 'Concurrent setup did not refuse the existing transaction.' }
    AssertInstall '0.1.0' $true
} finally { $guard.Dispose() }
$sentinel=Join-Path $install ('previous/install-fixture-'+[Guid]::NewGuid().ToString('N')+'.txt')
$handle=[IO.File]::Open($sentinel,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
$handle.Dispose()
try {
    $rejected=Start-Process -FilePath $new -ArgumentList '/S' -WindowStyle Hidden -Wait -PassThru
    if ($rejected.ExitCode -eq 0 -or !(Test-Path -LiteralPath $sentinel) -or
        (Get-FileHash -LiteralPath (Join-Path $install 'previous/ClipRope.exe')).Hash -ne $oldImage) {
        throw 'Setup modified a previous slot containing an unrecognized file.'
    }
} finally { Remove-Item -LiteralPath $sentinel }
$locked=[IO.File]::Open($program,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
try {
    $rejected=Start-Process -FilePath $new -ArgumentList '/S','/ROLLBACK' -WindowStyle Hidden -Wait -PassThru
    if ($rejected.ExitCode -eq 0) { throw 'Rollback accepted an exclusively retained binary.' }
    AssertInstall '0.1.0' $true
} finally { $locked.Dispose() }
Record 'Concurrent setup, an unknown previous-slot file and a locked binary stopped setup before changing either package.'
RunSetup $new @('/S','/ROLLBACK')
AssertInstall $PredecessorVersion $true
if ((SlotFingerprint) -ne $oldSlot) { throw 'Rollback did not restore the matched predecessor slot.' }
TestHistory '--verify-retained-profile'
Record 'Rollback restored the actual predecessor binary, sign-in choice and seeded history.'
RunSetup $new @('/S','/NOSTARTUP')
AssertInstall '0.1.0' $false
TestHistory '--verify-retained-profile'
Uninstall $false
if (!(Test-Path -LiteralPath (Join-Path $store 'history.db'))) { throw 'Keep-history uninstall removed the store.' }
$install = Join-Path ([IO.Path]::GetTempPath()) ("ClipRope relocated 应用 " + [Guid]::NewGuid().ToString('N'))
$current = Join-Path $install 'current'
$program = Join-Path $current 'ClipRope.exe'
RunSetup $new @('/S',"/D=$install")
AssertInstall '0.1.0' $true
TestHistory '--verify-retained-profile'
TestHistory '--check-installed-worker'
Record 'Reinstallation in a different Unicode path retained both original IDs, state and hashes and launched its worker.'
Uninstall $true
if (Test-Path -LiteralPath $store) { throw 'Explicit purge left the test profile.' }
Record 'Explicit purge removed the synthetic profile, binaries, shortcut and sign-in value.'

# Exercise setup taking ownership of a previously initialized direct copy in the empty test account.
if ($InstalledSlotsOnly) { return }
$install=Join-Path $local 'Programs/ClipRope'
$current=Join-Path $install 'current'
$program=Join-Path $current 'ClipRope.exe'
foreach ($enabled in @($true, $false)) {
    $receipt=Join-Path $evidence "direct-$enabled.txt"
    TestHistory '--seed-empty-installed-profile' $bins
    if ((Startup) -ne ('"'+(Join-Path $bins 'ClipRope.exe')+'"')) { throw 'Direct launch did not register itself.' }
    if (!$enabled) {
        Remove-ItemProperty -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'ClipRope'
        TestHistory '--verify-retained-profile' $bins
        if (Startup) { throw 'Direct launch recreated an externally removed registration.' }
    }
    RunSetup $new @('/S')
    AssertInstall '0.1.0' $enabled
    TestHistory '--verify-retained-profile'
    Uninstall $true
    Record "Setup adopted direct-run history and preserved sign-in enabled=$enabled."
}
