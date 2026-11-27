#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$ReleaseTag,
    [Parameter(Mandatory)] [string]$ExpectedCommit,
    [Parameter(Mandatory)] [string]$OutputDirectory,
    [Parameter(Mandatory)] [string]$PublicKey,
    [string]$SourceDirectory = (Join-Path $PSScriptRoot '../..'),
    [string]$ReleaseBranch = 'refs/remotes/origin/bastion-release',
    [string]$Glslang,
    [string]$Iscc,
    [switch]$ProvisionInno,
    [switch]$TestInstaller,
    [ValidateRange(1, 20)] [int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'ReleaseFiles.ps1')

if (!$IsWindows) { throw 'Build the Windows release on Windows.' }
$version = Get-BastionReleaseVersion $ReleaseTag
if ($ExpectedCommit -cnotmatch '^[0-9a-f]{40}$') { throw 'Supply the exact Git commit SHA.' }
if ($PublicKey -notmatch '^[0-9a-f]{64}$') { throw 'Supply the Bastion Ed25519 public key as 64 hex digits.' }
$PublicKey = $PublicKey.ToLowerInvariant()
$source = (Resolve-Path -LiteralPath $SourceDirectory).ProviderPath
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)

function Assert-ReleaseSource {
    $head = & git -C $source rev-parse --verify HEAD
    if ($LASTEXITCODE -ne 0 -or $head -cne $ExpectedCommit) { throw 'The checkout differs from the expected commit.' }
    $tagCommit = & git -C $source rev-parse --verify "refs/tags/${ReleaseTag}^{commit}"
    if ($LASTEXITCODE -ne 0 -or $tagCommit -cne $head) { throw 'The release tag must resolve to that commit.' }
    & git -C $source merge-base --is-ancestor $head $ReleaseBranch
    if ($LASTEXITCODE -ne 0) { throw "The release commit is outside $ReleaseBranch." }
    $changes = & git -C $source status --porcelain
    if ($LASTEXITCODE -ne 0 -or $changes) { throw 'A release requires a clean checkout.' }
}

function Invoke-ReleaseTool([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $Program) failed with exit code $LASTEXITCODE." }
}

function Get-PinnedTool([string]$Name, [uri]$Uri, [string]$Sha256) {
    $target = Join-Path $toolsDirectory $Name
    Invoke-WebRequest -Uri $Uri -OutFile $target
    if ((Get-ReleaseHash $target) -cne $Sha256) { throw "SHA-256 mismatch for $Name." }
    return $target
}

function Assert-X64Binary([string]$File) {
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($File))
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Invalid executable header in $File." }
        $reader.BaseStream.Position = 0x3C
        $reader.BaseStream.Position = $reader.ReadUInt32()
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) {
            throw "Expected a Windows x64 executable or DLL at $File."
        }
    } finally { $reader.Dispose() }
}

Assert-ReleaseSource
if ($output -ieq $source -or $output.StartsWith($source.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Build and stage outside the source checkout.'
}
if ((Test-Path -LiteralPath $output) -and (Get-ChildItem -LiteralPath $output -Force)) {
    throw 'Use an empty output directory for a release candidate.'
}
$toolsDirectory = (New-Item -ItemType Directory -Path (Join-Path $output 'tools') -Force).FullName
$release = (New-Item -ItemType Directory -Path (Join-Path $output 'release')).FullName
$stage = Join-Path $output 'payload'
$build = Join-Path $output 'build'

if (!$Glslang) {
    $shaderZip = Get-PinnedTool 'glslang.zip' `
        'https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip' `
        '82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b'
    Expand-Archive -LiteralPath $shaderZip -DestinationPath (Join-Path $toolsDirectory 'glslang')
    $glslangFiles = @(Get-ChildItem -LiteralPath (Join-Path $toolsDirectory 'glslang') -Recurse -File -Filter 'glslang.exe')
    if ($glslangFiles.Count -ne 1) { throw 'Expected one glslang.exe in the pinned shader compiler archive.' }
    $Glslang = $glslangFiles[0].FullName
}
$Glslang = (Resolve-Path -LiteralPath $Glslang).ProviderPath
$shaderVersion = (& $Glslang --version) -join "`n"
if ($LASTEXITCODE -ne 0 -or $shaderVersion -notmatch 'Glslang Version: \d+:16\.6\.0\b') {
    throw 'Bastion Windows builds use glslang 16.6.0.'
}

if (!$Iscc) {
    $installedCompiler = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'
    if ((Test-Path -LiteralPath $installedCompiler -PathType Leaf) -and
        (Get-Item -LiteralPath $installedCompiler).VersionInfo.FileVersion -match '^6\.7\.4(?:\.0)?\b') {
        $Iscc = $installedCompiler
    } elseif ($ProvisionInno) {
        $setup = Get-PinnedTool 'innosetup-6.7.4.exe' `
            'https://github.com/jrsoftware/issrc/releases/download/is-6_7_4/innosetup-6.7.4.exe' `
            'a549929319f9120716677c685535ca4671e4b937b5fdc87e2585f32d8eb30e8c'
        $innoDirectory = Join-Path $toolsDirectory 'inno'
        $options = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/DIR=`"$innoDirectory`"")
        $result = Start-Process -FilePath $setup -ArgumentList $options -Wait -PassThru -WindowStyle Hidden
        if ($result.ExitCode -ne 0) { throw "Inno Setup compiler installation failed with exit code $($result.ExitCode)." }
        $Iscc = Join-Path $innoDirectory 'ISCC.exe'
    } else {
        throw 'Install Inno Setup 6.7.4 or supply its compiler path with -Iscc.'
    }
}
$Iscc = (Resolve-Path -LiteralPath $Iscc).ProviderPath
if ((Get-Item -LiteralPath $Iscc).VersionInfo.FileVersion -notmatch '^6\.7\.4(?:\.0)?\b') {
    throw 'Bastion Windows builds use Inno Setup 6.7.4.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or !$vs) { throw 'Visual Studio with the x64 C++ workload is required.' }
$vsTools = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$cmake = Join-Path $vsTools 'CMake\bin\cmake.exe'
$ctest = Join-Path $vsTools 'CMake\bin\ctest.exe'
$ninja = Join-Path $vsTools 'Ninja\ninja.exe'
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$vsEnvironment = & $env:ComSpec /d /c ('call "{0}" 10.0.26100.0 -vcvars_ver=14.44 >nul && set' -f $vcvars)
if ($LASTEXITCODE -ne 0) { throw 'Install MSVC v143 14.44 and Windows SDK 10.0.26100.0.' }
foreach ($line in $vsEnvironment) {
    $equals = $line.IndexOf('=')
    if ($equals -gt 0) {
        [Environment]::SetEnvironmentVariable($line.Substring(0, $equals), $line.Substring($equals + 1))
    }
}
if ($env:VCToolsVersion -notmatch '^14\.44\.' -or $env:WindowsSDKVersion.TrimEnd('\') -ne '10.0.26100.0') {
    throw 'The active MSVC toolset or Windows SDK did not match the requested versions.'
}
$env:CMAKE_NINJA_FORCE_RESPONSE_FILE = '1'

# Published legacy Bastion plug-ins used these earlier host table layouts.
$legacyBulkHashes = @(
    '070044cfc84455a61a758256b85aa4164e1ef547cec487966759724a82bdd098'
    'dd1a15e36a76f48afea151c483d5bb209142b2aad534faa358cd58f823ebef55'
    '0df6e7c562cc62862ed7ffe71d2eab3f8e1be1cda4f8ca20c8c4923f68f867f9'
    '0a28e6b6bfff90bff8b218c81a738a5f8408dd0f82afde8eba2d75ab01b2190a'
    'bca8ac75ca21b02a46b4b1173b3c58c5c3a34388bdd7233ac4e151ddffff0aeb'
)
Invoke-ReleaseTool $cmake @(
    '-S', $source, '-B', $build, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$ninja", '-DCMAKE_BUILD_TYPE=Release'
    '-DHAILSTONE_BROWSER=ON', '-DHAILSTONE_BUILD_TESTS=ON', '-DHAILSTONE_MANAGED_UPDATES=ON'
    '-DHAILSTONE_ALLOW_IN_EQ_FOLDER=OFF', '-DHAILSTONE_SANITIZE=OFF'
    "-DHAILSTONE_RELEASE_VERSION_OVERRIDE=$version", "-DHAILSTONE_UPDATE_KEY=$PublicKey"
    '-DHAILSTONE_LEGACY_POPUP_PLUGIN_SHA256=5a486fc92dd07539fee018b84808e756c1c16c7b36cc006dfd34a34e037bb9b0'
    '-DHAILSTONE_LEGACY_POPUP_UI_PLUGIN_SHA256=3eaf7ce3339fcf95702984f37e7c55a67763045040eabba932711917a77ffead'
    "-DHAILSTONE_LEGACY_POPUP_BULK_PLUGIN_SHA256=$($legacyBulkHashes -join ';')"
    '-DHAILSTONE_LEGACY_POPUP_SPELL_PLUGIN_SHA256=9c26936573eade7c02b90aa91f660c90851165f1c82bc4997f098c8953e57fa8'
    '-DHAILSTONE_LEGACY_PLACEMENT_PLUGIN_SHA256=c28146ea5b8e9bf137b59c893ef8cfab4882a4868912faa6776c1a92347cdafc'
    '-DCMAKE_DISABLE_FIND_PACKAGE_SDL3=TRUE', '-DCMAKE_DISABLE_FIND_PACKAGE_glm=TRUE'
    '-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE', '-DHAILSTONE_GLSLC=OFF', "-DHAILSTONE_GLSLANG=$Glslang"
)
Invoke-ReleaseTool $cmake @('--build', $build, '--parallel', $Jobs.ToString())
foreach ($name in 'HAILSTONE_LIVE_LOGIN', 'NMSC_LIVE_LOGIN', 'LIVE_LOGIN', 'EQ_GAME_DIR', 'HAILSTONE_VECTORS_DIR') {
    [Environment]::SetEnvironmentVariable($name, $null)
}
$env:HAILSTONE_DATA_DIR = Join-Path $output 'test-data'
Invoke-ReleaseTool $ctest @('--test-dir', $build, '--output-on-failure', '--no-tests=error', '--timeout', '900', '-j', $Jobs.ToString())
Assert-ReleaseSource

$buildInfo = Get-Content -LiteralPath (Join-Path $build 'generated/build_info.inc') -Raw
$compiled = [regex]::Match($buildInfo, '(?m)^#define HAILSTONE_BUILD_VERSION "(\S+) \(([0-9a-f]{7,40})\)"\r?$')
$compiledVersion = '#define HAILSTONE_RELEASE_VERSION "' + $version + '"'
if (!$compiled.Success -or $compiled.Groups[1].Value -cne $version -or
    !$ExpectedCommit.StartsWith($compiled.Groups[2].Value, [StringComparison]::Ordinal) -or
    !$buildInfo.Contains($compiledVersion, [StringComparison]::Ordinal)) {
    throw 'The compiled release version or revision does not match the tag.'
}
$revision = $compiled.Groups[2].Value

Invoke-ReleaseTool $cmake @('--install', $build, '--config', 'Release', '--prefix', $stage)
$development = (New-Item -ItemType Directory -Path (Join-Path $output 'development')).FullName
foreach ($name in 'bin', 'include', 'lib', 'share') {
    $path = Join-Path $stage $name
    if (Test-Path -LiteralPath $path -PathType Container) { Move-Item -LiteralPath $path -Destination $development }
}
$crt = @(Get-ChildItem -LiteralPath (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT')
if ($crt.Count -ne 1) { throw 'Could not identify the x64 Visual C++ distributable runtime.' }
$crtDlls = @(Get-ChildItem -LiteralPath $crt[0].FullName -File -Filter '*.dll')
foreach ($dll in $crtDlls) {
    Copy-Item -LiteralPath $dll.FullName -Destination $stage
    Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $stage 'browser')
}
foreach ($entry in Get-ChildItem -LiteralPath $stage -Recurse -Force) {
    if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $entry.Name.StartsWith('.')) {
        throw "The runtime stage contains an unexpected link or hidden package entry at $($entry.FullName)."
    }
}
foreach ($binary in 'hailstone.exe', 'SDL3.dll', 'zlib1.dll', 'browser/hailstone-web.exe', 'browser/libcef.dll') {
    Assert-X64Binary (Join-Path $stage $binary)
}
$binaryText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Join-Path $stage 'hailstone.exe')))
if (!$binaryText.Contains("$version ($revision)" + [char]0, [StringComparison]::Ordinal)) {
    throw 'The staged Hailstone executable does not match the inspected build.'
}

$keyFile = Join-Path $release 'bastion-update-key.pub'
Set-Content -LiteralPath $keyFile -Encoding ascii -Value $PublicKey
$notes = "https://github.com/fryguy503/hailstone-client/releases/tag/$ReleaseTag"
Invoke-ReleaseTool 'python' @((Join-Path $source 'tools/make_update.py'), '--version', $version, '--platform',
    'windows-x64', '--dist', $stage, '--key', $keyFile, '--notes', $notes, '--out', $release)

$artwork = Join-Path $output 'artwork'
& (Join-Path $PSScriptRoot 'installer/New-WizardArtwork.ps1') -SourceRoot $stage -OutputDirectory $artwork
$setupIcon = Join-Path $source 'cmake/windows/hailstone.ico'
$setupName = "Hailstone-$version-$revision-Windows-x64-Setup"
$numericVersion = $version.Split('-')[0]
Invoke-ReleaseTool $Iscc @('/Qp', "/O$release", "/F$setupName", "/DStage=$stage", "/DArtwork=$artwork",
    "/DSetupIcon=$setupIcon", "/DClientVersion=$version", "/DNumericVersion=$numericVersion", "/DBuildRevision=$revision",
    (Join-Path $PSScriptRoot 'installer/Hailstone.iss'))
if ($TestInstaller) {
    & (Join-Path $PSScriptRoot 'Test-InstallerUpgrade.ps1') -Installer (Join-Path $release "$setupName.exe") `
        -Stage $stage -ExpectedDisplayVersion $version -LogDirectory $output
}

$helperStage = (New-Item -ItemType Directory -Path (Join-Path $output 'helper')).FullName
$helperFiles = @((Join-Path $build 'hailstone_update.exe'), (Join-Path $stage 'SDL3.dll'), (Join-Path $stage 'zlib1.dll'))
$helperFiles += @('msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll' | ForEach-Object {
    Join-Path $crt[0].FullName $_
})
foreach ($file in $helperFiles) {
    Assert-X64Binary $file
    Copy-Item -LiteralPath $file -Destination $helperStage
}
$helperOutput = & (Join-Path $helperStage 'hailstone_update.exe')
if ($LASTEXITCODE -ne 1 -or 'status=error' -cnotin $helperOutput) {
    throw 'The staged bootstrap helper did not start and report invalid arguments.'
}
Compress-Archive -Path (Join-Path $helperStage '*') -DestinationPath (Join-Path $release 'hailstone-update-helper-windows-x64.zip')

Assert-ReleaseSource
Invoke-ReleaseTool 'git' @('-C', $source, 'archive', '--format=zip', "--prefix=hailstone-$version/",
    "--output=$(Join-Path $release "Hailstone-$version-Source.zip")", $ExpectedCommit)

$files = [ordered]@{}
foreach ($file in Get-ChildItem -LiteralPath $release -File | Sort-Object Name) {
    $files[$file.Name] = Get-ReleaseHash $file.FullName
}
[ordered]@{
    version = $version
    tag = $ReleaseTag
    commit = $ExpectedCommit
    runtimeSha256 = Get-ReleaseHash (Join-Path $stage 'hailstone.exe')
    files = $files
} |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $release 'release-input.json') -Encoding utf8
$files.GetEnumerator() | ForEach-Object { "$($_.Value)  $($_.Key)" } |
    Set-Content -LiteralPath (Join-Path $release 'SHA256SUMS') -Encoding ascii
Write-Host "Bastion $version candidate is ready at $release. The update manifest still requires signing."
