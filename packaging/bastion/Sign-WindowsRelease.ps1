#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$CandidateDirectory,
    [Parameter(Mandatory)] [string]$OutputDirectory,
    [Parameter(Mandatory)] [string]$ExpectedTag,
    [Parameter(Mandatory)] [string]$ExpectedCommit,
    [Parameter(Mandatory)] [ValidateSet('preview', 'stable')] [string]$Channel,
    [Parameter(Mandatory)] [string]$SignTool,
    [Parameter(Mandatory)] [string]$SigningKey,
    [Parameter(Mandatory)] [string]$PublicKey
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'ReleaseFiles.ps1')

$Channel = $Channel.ToLowerInvariant()
$version = Get-BastionReleaseVersion $ExpectedTag
$candidateDirectory = (Resolve-Path -LiteralPath $CandidateDirectory).ProviderPath
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
$signTool = (Resolve-Path -LiteralPath $SignTool).ProviderPath
$signingKey = (Resolve-Path -LiteralPath $SigningKey).ProviderPath
$publicKey = (Resolve-Path -LiteralPath $PublicKey).ProviderPath
$candidate = Get-Content -LiteralPath (Join-Path $candidateDirectory 'release-input.json') -Raw | ConvertFrom-Json -AsHashtable
if ($ExpectedCommit -cnotmatch '^[0-9a-f]{40}$' -or $candidate.commit -cne $ExpectedCommit -or
    $candidate.tag -cne $ExpectedTag -or $candidate.version -cne $version) {
    throw 'The candidate does not match the approved tag and commit.'
}
foreach ($entry in $candidate.files.GetEnumerator()) {
    if ($entry.Key -cnotmatch '\A[a-zA-Z0-9][a-zA-Z0-9.-]+\z' -or $entry.Value -cnotmatch '^[0-9a-f]{64}$') {
        throw 'Invalid filename or hash in the candidate record.'
    }
    if ((Get-ReleaseHash (Join-Path $candidateDirectory $entry.Key)) -cne $entry.Value) {
        throw "Candidate SHA-256 mismatch for $($entry.Key)."
    }
}
$manifestFile = Join-Path $candidateDirectory 'hailstone-update.txt'
$inputLines = @(Get-Content -LiteralPath $manifestFile -Encoding utf8)
$manifest = Read-BastionManifest $manifestFile
if (!$candidate.files.Contains('hailstone-update.txt') -or $inputLines.Count -ne 4 -or
    $inputLines[2] -cne "notes https://github.com/fryguy503/hailstone-client/releases/tag/$ExpectedTag") {
    throw 'Expected the unsigned candidate manifest from Build-WindowsRelease.ps1.'
}
if ($manifest.Version -cne $version -or !$candidate.files.Contains($manifest.File)) {
    throw 'The manifest and candidate record describe different builds.'
}
Assert-ReleaseAsset $manifest $candidateDirectory
$runtimeHash = Get-ReleaseRuntimeHash (Join-Path $candidateDirectory $manifest.File)
if ($runtimeHash -cne $candidate.runtimeSha256) { throw 'The staged executable hash does not match the candidate record.' }
$setupFile = Get-BastionSetupFileName $version $ExpectedCommit
if (!$candidate.files.Contains($setupFile)) { throw 'The candidate is missing the approved Windows Setup.' }
$setupPath = Get-Item -LiteralPath (Join-Path $candidateDirectory $setupFile)
if ($setupPath.PSIsContainer -or ($setupPath.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
    $setupPath.Length -lt 1 -or $setupPath.Length -gt 1GB) { throw 'Bastion Setup must be a regular file between 1 byte and 1 GiB.' }
$setup = [pscustomobject]@{ File = $setupFile; Size = $setupPath.Length; Sha256 = $candidate.files[$setupFile] }
Assert-ReleaseAsset $setup $candidateDirectory
$pinnedKey = (Get-Content -LiteralPath $publicKey -Raw).Trim().ToLowerInvariant()
$binaryKey = (Get-Content -LiteralPath (Join-Path $candidateDirectory 'bastion-update-key.pub') -Raw).Trim().ToLowerInvariant()
if ($pinnedKey -cnotmatch '^[0-9a-f]{64}$' -or $binaryKey -cne $pinnedKey) {
    throw 'The candidate must embed the pinned Bastion public key.'
}
if ((Test-Path -LiteralPath $output) -and (Get-ChildItem -LiteralPath $output -Force)) {
    throw 'Use an empty output directory for the signed release.'
}
New-Item -ItemType Directory -Path $output -Force | Out-Null
$signed = Join-Path $output 'hailstone-update.txt'
$signedLines = @($inputLines[0], $inputLines[1], $inputLines[2], "bastion-channel $Channel",
    "bastion-runtime-sha256 $runtimeHash", "bastion-setup $($setup.Size) $($setup.Sha256) $($setup.File)", $inputLines[3])
[IO.File]::WriteAllText($signed, ($signedLines -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
& $signTool sign $signingKey $signed
if ($LASTEXITCODE -ne 0) { throw 'Manifest signing failed.' }
& $signTool verify $publicKey $signed
if ($LASTEXITCODE -ne 0) { throw 'The signed manifest did not verify against the pinned public key.' }
$signedManifest = Read-BastionManifest $signed -Signed
if ($signedManifest.RuntimeSha256 -cne $runtimeHash) {
    throw 'The signed runtime SHA-256 differs from the staged executable.'
}
if ($null -eq $signedManifest.Setup -or $signedManifest.Setup.File -cne $setup.File -or
    $signedManifest.Setup.Size -ne $setup.Size -or $signedManifest.Setup.Sha256 -cne $setup.Sha256) {
    throw 'The signed Setup record differs from the staged Setup.'
}
foreach ($asset in $signedManifest, $signedManifest.Setup) {
    Copy-Item -LiteralPath (Join-Path $candidateDirectory $asset.File) -Destination $output
    Assert-ReleaseAsset $asset $output
}
Write-Host "Signed $version for $Channel. Publish the signed directory at $output."
