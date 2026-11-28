#requires -Version 7.0
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)] [string]$SignedDirectory,
    [Parameter(Mandatory)] [string]$PublishRoot,
    [Parameter(Mandatory)] [uri]$PublicUri,
    [Parameter(Mandatory)] [ValidateSet('preview', 'stable')] [string]$Channel,
    [Parameter(Mandatory)] [string]$SignTool,
    [Parameter(Mandatory)] [string]$PublicKey
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'ReleaseFiles.ps1')

function Read-PromotionManifest([string]$Path) {
    & $signTool verify $publicKey $Path | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'The manifest did not verify against the pinned Bastion public key.' }
    $channels = @(Get-Content -LiteralPath $Path -Encoding utf8 | Where-Object { $_ -clike 'bastion-channel *' })
    if ($channels.Count -ne 1 -or $channels[0] -cne "bastion-channel $Channel") {
        throw 'The signed manifest does not target this channel.'
    }
    return Read-BastionManifest $Path -Signed
}

function Assert-PublicHash([string]$File, [string]$Sha256, [long]$Size) {
    $uri = [uri]::new($channelUri, $File)
    $timeout = [Threading.CancellationTokenSource]::new([TimeSpan]::FromMinutes(15))
    try {
        $response = $http.GetAsync($uri, [Net.Http.HttpCompletionOption]::ResponseHeadersRead, $timeout.Token).GetAwaiter().GetResult()
        try {
            if ([int]$response.StatusCode -ne 200) { throw "HTTP $([int]$response.StatusCode) at $uri." }
            $stream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
            $hash = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
            try {
                $buffer = [byte[]]::new(65536)
                $total = 0L
                while ($count = $stream.ReadAsync($buffer, 0, $buffer.Length, $timeout.Token).GetAwaiter().GetResult()) {
                    $total += $count
                    if ($total -gt $Size) { throw "Public byte count exceeded the expected size at $uri." }
                    $hash.AppendData($buffer, 0, $count)
                }
                $actual = [BitConverter]::ToString($hash.GetHashAndReset()).Replace('-', '').ToLowerInvariant()
                if ($actual -cne $Sha256 -or $total -ne $Size) { throw "Public SHA-256 or size mismatch at $uri." }
            } finally { $hash.Dispose() }
        } finally { $response.Dispose() }
    } finally { $timeout.Dispose() }
}

$Channel = $Channel.ToLowerInvariant()
if (!$PublicUri.IsAbsoluteUri -or $PublicUri.Scheme -ne 'https' -or $PublicUri.UserInfo -or
    $PublicUri.Query -or $PublicUri.Fragment -or !$PublicUri.AbsolutePath.EndsWith('/')) {
    throw 'Supply an HTTPS base URI ending with a slash, for example https://patcher.bastiongame.com/hailstone/.'
}
$signedDirectory = (Resolve-Path -LiteralPath $SignedDirectory).ProviderPath
$publishRoot = (Resolve-Path -LiteralPath $PublishRoot).ProviderPath
$signTool = (Resolve-Path -LiteralPath $SignTool).ProviderPath
$publicKey = (Resolve-Path -LiteralPath $PublicKey).ProviderPath
$channelUri = [uri]::new($PublicUri, "$Channel/")
$channelDirectory = Get-Item -LiteralPath (Join-Path $publishRoot $Channel)
if (!$channelDirectory.PSIsContainer -or ($channelDirectory.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw 'Provision a regular directory for the channel in the updater web root.'
}
$feedName = 'hailstone-update.txt'
$manifestSource = Join-Path $signedDirectory $feedName
$manifest = Read-PromotionManifest $manifestSource
$assets = @($manifest)
if ($null -ne $manifest.Setup) { $assets += $manifest.Setup }
foreach ($asset in $assets) { Assert-ReleaseAsset $asset $signedDirectory }
if ((Get-ReleaseRuntimeHash (Join-Path $signedDirectory $manifest.File)) -cne $manifest.RuntimeSha256) {
    throw 'The signed Bastion runtime SHA-256 does not match the Windows ZIP.'
}
$manifestHash = Get-ReleaseHash $manifestSource

if (!$PSCmdlet.ShouldProcess($channelUri.AbsoluteUri, "Publish Hailstone $($manifest.Version)")) { return }

$lockPath = Join-Path $publishRoot ".bastion-$Channel-promotion.lock"
$guard = [IO.File]::Open($lockPath, 'OpenOrCreate', 'ReadWrite', 'None')
$handler = [Net.Http.HttpClientHandler]::new()
$handler.AllowAutoRedirect = $false
$http = [Net.Http.HttpClient]::new($handler)
$http.DefaultRequestHeaders.CacheControl = [Net.Http.Headers.CacheControlHeaderValue]::new()
$http.DefaultRequestHeaders.CacheControl.NoCache = $true
$nonce = [guid]::NewGuid().ToString('N')
$assetTemp = Join-Path $channelDirectory.FullName ".asset-$nonce.tmp"
$manifestTemp = Join-Path $channelDirectory.FullName ".manifest-$nonce.tmp"
try {
    $manifestTarget = Join-Path $channelDirectory.FullName $feedName
    if (Test-Path -LiteralPath $manifestTarget) {
        $existing = Read-PromotionManifest $manifestTarget
        $currentNumber = [version]($existing.Version.Split('-')[0])
        $nextNumber = [version]($manifest.Version.Split('-')[0])
        $currentSerial = [bigint]($existing.Version.Split('.')[-1])
        $nextSerial = [bigint]($manifest.Version.Split('.')[-1])
        if ($nextNumber -lt $currentNumber -or ($nextNumber -eq $currentNumber -and $nextSerial -lt $currentSerial)) {
            throw "Publishing $($manifest.Version) would move $Channel backward from $($existing.Version)."
        }
        if ($existing.Version -ceq $manifest.Version -and (Get-ReleaseHash $manifestTarget) -cne $manifestHash) {
            throw 'That version is already published with another signed manifest.'
        }
    }

    foreach ($asset in $assets) {
        $assetTarget = Join-Path $channelDirectory.FullName $asset.File
        if (!(Test-Path -LiteralPath $assetTarget)) {
            [IO.File]::Copy((Join-Path $signedDirectory $asset.File), $assetTemp)
            if ((Get-Item -LiteralPath $assetTemp).Length -ne $asset.Size -or
                (Get-ReleaseHash $assetTemp) -cne $asset.Sha256) { throw "Staged $($asset.File) does not match the manifest." }
            [IO.File]::Move($assetTemp, $assetTarget)
        }
        Assert-ReleaseAsset $asset $channelDirectory.FullName
        Assert-PublicHash $asset.File $asset.Sha256 $asset.Size
    }

    [IO.File]::Copy($manifestSource, $manifestTemp)
    if ((Get-ReleaseHash $manifestTemp) -cne $manifestHash) { throw 'Staged manifest SHA-256 mismatch.' }
    [IO.File]::Move($manifestTemp, $manifestTarget, $true)
    Assert-PublicHash $feedName $manifestHash (Get-Item -LiteralPath $manifestTarget).Length
    Write-Host "$Channel serves signed Hailstone $($manifest.Version)."
} finally {
    foreach ($file in $assetTemp, $manifestTemp) { [IO.File]::Delete($file) }
    $http.Dispose()
    $guard.Dispose()
}
