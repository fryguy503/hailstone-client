#requires -Version 7.0
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)] [string]$SignedDirectory,
    [Parameter(Mandatory)] [ValidatePattern('\A[0-9a-f]{40}\z')] [string]$ExpectedCommit,
    [Parameter(Mandatory)] [string]$SignTool,
    [Parameter(Mandatory)] [string]$PublicKey
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'ReleaseFiles.ps1')

$repository = 'fryguy503/hailstone-client'
$feedBranch = 'bastion-production-feed'
$manifestName = 'hailstone-update.txt'
$feedUri = "https://raw.githubusercontent.com/$repository/$feedBranch/$manifestName"
$signedDirectory = (Resolve-Path -LiteralPath $SignedDirectory).ProviderPath
$verifier = (Resolve-Path -LiteralPath $SignTool).ProviderPath
$pinnedKey = (Resolve-Path -LiteralPath $PublicKey).ProviderPath

function Read-StableManifest([string]$Path) {
    & $verifier verify $pinnedKey $Path | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'The manifest did not verify against the approved Bastion public key.' }
    $channels = @(Get-Content -LiteralPath $Path -Encoding utf8 | Where-Object { $_ -match '\A\s*bastion-channel(?:\s|\z)' })
    if ($channels.Count -ne 1 -or $channels[0] -cne 'bastion-channel stable') {
        throw 'Expected one signed Bastion Production channel record.'
    }
    return Read-BastionManifest $Path -Signed
}

function Read-StableManifestBytes([byte[]]$Content) {
    $temp = [IO.Path]::GetTempFileName()
    try {
        [IO.File]::WriteAllBytes($temp, $Content)
        return Read-StableManifest $temp
    } finally { [IO.File]::Delete($temp) }
}

function Get-BytesHash([byte[]]$Content) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($Content)).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Invoke-RepositoryApi([string]$Method, [string]$Path, [hashtable]$Body) {
    $arguments = @('api', '--hostname', 'github.com', '--method', $Method, "repos/$repository/$Path")
    if ($null -eq $Body) {
        $result = & gh @arguments
    } else {
        $result = ($Body | ConvertTo-Json -Depth 8 -Compress) | & gh @arguments --input -
    }
    if ($LASTEXITCODE -ne 0) { throw "GitHub request failed: $Method $Path." }
    return $result | ConvertFrom-Json -Depth 15
}

function Confirm-PublicAsset([Net.Http.HttpClient]$Client, [uri]$Uri, $Asset, [switch]$AllowAssetRedirect) {
    $timeout = [Threading.CancellationTokenSource]::new([TimeSpan]::FromMinutes(15))
    try {
        $target = $Uri
        for ($redirects = 0; $redirects -le 3; $redirects++) {
            $response = $Client.GetAsync($target, [Net.Http.HttpCompletionOption]::ResponseHeadersRead,
                $timeout.Token).GetAwaiter().GetResult()
            try {
                $status = [int]$response.StatusCode
                if ($status -in 301,302,303,307,308 -and $AllowAssetRedirect) {
                    $location = $response.Headers.Location
                    if ($null -eq $location -or !$location.IsAbsoluteUri -or $location.Scheme -cne 'https' -or
                        $location.DnsSafeHost -cne 'release-assets.githubusercontent.com' -or !$location.IsDefaultPort -or
                        $location.UserInfo -or $location.Fragment -or
                        !$location.AbsolutePath.StartsWith('/github-production-release-asset/', [StringComparison]::Ordinal)) {
                        throw "Unexpected GitHub redirect for $($Asset.File)."
                    }
                    $target = $location
                    continue
                }
                if ($status -ne 200) { throw "HTTP $status fetching $($Asset.File) from GitHub." }
                $stream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
                $hash = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
                try {
                    $buffer = [byte[]]::new(65536)
                    $total = 0L
                    while (($count = $stream.ReadAsync($buffer, 0, $buffer.Length, $timeout.Token).GetAwaiter().GetResult()) -gt 0) {
                        $total += $count
                        if ($total -gt $Asset.Size) { throw "$($Asset.File) exceeds its expected size on GitHub." }
                        $hash.AppendData($buffer, 0, $count)
                    }
                    $actual = [BitConverter]::ToString($hash.GetHashAndReset()).Replace('-', '').ToLowerInvariant()
                    if ($actual -cne $Asset.Sha256 -or $total -ne $Asset.Size) {
                        throw "$($Asset.File) on GitHub does not match the verified release bytes."
                    }
                    return
                } finally {
                    $stream.Dispose()
                    $hash.Dispose()
                }
            } finally { $response.Dispose() }
        }
        throw "Too many GitHub redirects for $($Asset.File)."
    } finally { $timeout.Dispose() }
}

$bytes = [IO.File]::ReadAllBytes((Join-Path $signedDirectory $manifestName))
$manifest = Read-StableManifestBytes $bytes
$tag = "bastion-v$($manifest.Version)"
if ($null -eq $manifest.Setup -or $manifest.Setup.File -cne (Get-BastionSetupFileName $manifest.Version $ExpectedCommit)) {
    throw 'The signed Setup must match the exact approved release commit.'
}
foreach ($asset in $manifest, $manifest.Setup) { Assert-ReleaseAsset $asset $signedDirectory }
if ((Get-ReleaseRuntimeHash (Join-Path $signedDirectory $manifest.File)) -cne $manifest.RuntimeSha256) {
    throw 'The Windows runtime ZIP does not contain the signed Bastion executable.'
}
$manifestHash = Get-BytesHash $bytes
if (!$PSCmdlet.ShouldProcess($feedUri, "Promote already published GitHub release $tag")) { return }

$tagRef = "refs/tags/$tag"
$refs = & git ls-remote "https://github.com/$repository.git" $tagRef "$tagRef^{}"
if ($LASTEXITCODE -ne 0) { throw 'Could not verify the approved GitHub tag.' }
$tagRefs = @{}
foreach ($line in $refs) {
    $fields = $line -split '\t', 2
    if ($fields.Count -eq 2) { $tagRefs[$fields[1]] = $fields[0] }
}
$tagCommit = $tagRefs["$tagRef^{}"] ?? $tagRefs[$tagRef]
if ($tagCommit -cne $ExpectedCommit) {
    throw "GitHub tag $tag does not resolve to the approved release commit."
}
$release = Invoke-RepositoryApi 'GET' "releases/tags/$tag"
if ($release.tag_name -cne $tag -or $release.draft -or $release.prerelease) {
    throw "Publish the reviewed Production release $tag on GitHub before promoting the feed."
}

$ref = "refs/heads/$feedBranch"
$currentRef = Invoke-RepositoryApi 'GET' "git/matching-refs/heads/$feedBranch" | Where-Object ref -CEQ $ref
$parentCommit = if ($currentRef) { $currentRef.object.sha } else { $null }
$treeBody = @{}
$sameManifest = $false
if ($parentCommit) {
    $remoteFile = Invoke-RepositoryApi 'GET' "contents/$($manifestName)?ref=$parentCommit"
    if ($remoteFile.type -cne 'file' -or $remoteFile.encoding -cne 'base64') {
        throw 'The existing Production feed has an unexpected file type.'
    }
    $currentBytes = [Convert]::FromBase64String($remoteFile.content)
    $current = Read-StableManifestBytes $currentBytes
    $currentNumber = [version]($current.Version.Split('-')[0])
    $nextNumber = [version]($manifest.Version.Split('-')[0])
    $currentBuild = [bigint]($current.Version.Split('.')[-1])
    $nextBuild = [bigint]($manifest.Version.Split('.')[-1])
    if ($nextNumber -lt $currentNumber -or ($nextNumber -eq $currentNumber -and $nextBuild -lt $currentBuild)) {
        throw "Publishing $($manifest.Version) would move Production backward from $($current.Version)."
    }
    $sameManifest = (Get-BytesHash $currentBytes) -ceq $manifestHash
    if ($current.Version -ceq $manifest.Version -and !$sameManifest) {
        throw 'That Production version is already published with another signed manifest.'
    }
    $treeBody.base_tree = (Invoke-RepositoryApi 'GET' "git/commits/$parentCommit").tree.sha
}

$handler = [Net.Http.HttpClientHandler]::new()
$handler.AllowAutoRedirect = $false
$client = [Net.Http.HttpClient]::new($handler)
try {
    foreach ($asset in $manifest, $manifest.Setup) {
        Confirm-PublicAsset $client "https://github.com/$repository/releases/download/$tag/$($asset.File)" $asset -AllowAssetRedirect
    }
    if ($parentCommit -and $sameManifest) {
        Write-Host "Production already serves the signed Hailstone $($manifest.Version) release."
        return
    }

    $blob = Invoke-RepositoryApi 'POST' 'git/blobs' @{ encoding = 'base64'; content = [Convert]::ToBase64String($bytes) }
    $treeBody.tree = @(@{ path = $manifestName; mode = '100644'; type = 'blob'; sha = $blob.sha })
    $tree = Invoke-RepositoryApi 'POST' 'git/trees' $treeBody
    $commit = Invoke-RepositoryApi 'POST' 'git/commits' @{
        message = "Publish Hailstone $($manifest.Version) to Production"
        tree = $tree.sha
        parents = @($parentCommit | Where-Object { $_ })
    }
    if ($parentCommit) {
        # Advancing from the previously verified branch tip makes a concurrent promotion fail safely.
        $null = Invoke-RepositoryApi 'PATCH' "git/refs/heads/$feedBranch" @{ sha = $commit.sha; force = $false }
    } else {
        $null = Invoke-RepositoryApi 'POST' 'git/refs' @{ ref = $ref; sha = $commit.sha }
    }
    $record = @{ File = $manifestName; Size = $bytes.LongLength; Sha256 = $manifestHash }
    Confirm-PublicAsset $client "$($feedUri)?published=$($commit.sha)" $record
    Write-Host "Production now serves the signed Hailstone $($manifest.Version) release from GitHub."
} finally { $client.Dispose() }
