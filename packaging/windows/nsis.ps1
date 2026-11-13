function Get-ClipRopeNsis([string]$Repository) {
    $tools = Join-Path $Repository 'build/packaging-tools'
    $archive = Join-Path $Repository '.local-artifacts/nsis-3.13.zip'
    $expected = 'BA63DFFC4410EE89193E1CB5A41989991BD77C61068DA17E3156D136B7B0B3D8'
    if (!(Test-Path -LiteralPath $archive -PathType Leaf)) {
        Invoke-WebRequest 'https://downloads.sourceforge.net/project/nsis/NSIS%203/3.13/nsis-3.13.zip' -OutFile $archive
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash -ne $expected) {
        throw 'The portable NSIS source archive did not match the pinned SHA-256.'
    }
    $compiler = Join-Path $tools 'nsis-3.13/makensis.exe'
    Expand-Archive -LiteralPath $archive -DestinationPath $tools -Force
    return $compiler
}
