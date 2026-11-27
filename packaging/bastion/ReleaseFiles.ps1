# The candidate record is authenticated out of band by the selected build run. The updater's
# signed manifest is authenticated by the pinned key and covers the bytes players install.
function Get-ReleaseHash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-BastionReleaseVersion([string]$Tag) {
    if ($Tag -cnotmatch '\Abastion-v((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)-bastion\.[1-9][0-9]*)\z') {
        throw 'Use a tag such as bastion-v0.7.4-bastion.6.'
    }
    return $Matches[1]
}

function Read-BastionManifest([string]$Path, [switch]$Signed) {
    $lines = @(Get-Content -LiteralPath $Path -Encoding utf8)
    if ($lines.Count -lt 3 -or $lines[0] -cne 'hailstone-update 1' -or
        $lines[1] -cnotmatch '\Aversion ([0-9a-z.-]+)\z') {
        throw 'Invalid Bastion update manifest header or version.'
    }
    $version = Get-BastionReleaseVersion "bastion-v$($Matches[1])"
    $packageLines = @($lines | Where-Object { $_ -clike 'package *' })
    if ($packageLines.Count -ne 1 -or
        $packageLines[0] -cnotmatch '\Apackage windows-x64 core ([1-9][0-9]*) ([0-9a-f]{64}) ([a-z0-9.-]+\.zip)\z') {
        throw 'Expected one Windows x64 core package.'
    }
    $bytes = [long]$Matches[1]
    $sha256 = $Matches[2]
    $file = $Matches[3]
    if ($file -cne "hailstone-$version-windows-x64.zip") { throw 'The package filename must match the release version.' }
    $runtimeLines = @($lines | Where-Object { $_ -match '\A\s*bastion-runtime-sha256(?:\s|\z)' })
    $runtimeHash = ''
    if ($Signed) {
        if ($runtimeLines.Count -ne 1 -or $runtimeLines[0] -cnotmatch '\Abastion-runtime-sha256 ([0-9a-f]{64})\z') {
            throw 'Expected one valid signed Bastion runtime SHA-256.'
        }
        $runtimeHash = $Matches[1]
    } elseif ($runtimeLines.Count -ne 0) {
        throw 'Only the signer can add the Bastion runtime SHA-256 to the manifest.'
    }
    return [pscustomobject]@{ Version = $version; Size = $bytes; Sha256 = $sha256; File = $file; RuntimeSha256 = $runtimeHash }
}

function Assert-ReleasePackage($Manifest, [string]$Directory) {
    $package = Get-Item -LiteralPath (Join-Path $Directory $Manifest.File)
    if ($package.PSIsContainer -or ($package.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        $package.Length -ne $Manifest.Size -or (Get-ReleaseHash $package.FullName) -cne $Manifest.Sha256) {
        throw 'The Windows ZIP does not match the manifest.'
    }
}

function Get-ReleaseRuntimeHash([string]$Package) {
    $archive = [IO.Compression.ZipFile]::OpenRead($Package)
    try {
        $entries = @($archive.Entries | Where-Object FullName -IEQ 'hailstone.exe')
        if ($entries.Count -ne 1 -or $entries[0].FullName -cne 'hailstone.exe') {
            throw 'The Windows ZIP must contain exactly one root-level hailstone.exe.'
        }
        $stream = $entries[0].Open()
        try {
            return (Get-FileHash -InputStream $stream -Algorithm SHA256).Hash.ToLowerInvariant()
        } finally { $stream.Dispose() }
    } finally { $archive.Dispose() }
}
