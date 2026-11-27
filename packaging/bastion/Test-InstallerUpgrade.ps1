#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$PreviousInstaller,
    [Parameter(Mandatory)] [string]$Installer,
    [Parameter(Mandatory)] [string]$Stage,
    [string]$PreviousStage,
    [ValidatePattern('^Hailstone\.Client(?:\.UpgradeTest\.[0-9A-Fa-f]{8,24})?$')]
    [string]$AppId = 'Hailstone.Client',
    [string]$ExpectedDisplayVersion,
    [string[]]$ExistingPluginDll,
    [string[]]$ExistingScriptDirectory,
    [string]$LogDirectory = [IO.Path]::GetTempPath()
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$upgrade = [bool]$PreviousInstaller
$PreviousInstaller = if ($upgrade) { (Resolve-Path -LiteralPath $PreviousInstaller).ProviderPath }
$Installer = (Resolve-Path -LiteralPath $Installer).ProviderPath
$firstInstaller = if ($upgrade) { $PreviousInstaller } else { $Installer }
if ($PreviousStage -and !$upgrade) { throw 'Supply -PreviousInstaller when testing an existing stage.' }
$addonFiles = @{}
foreach ($path in $ExistingPluginDll) {
    $file = Get-Item -LiteralPath $path
    if ($file.PSIsContainer -or $file.Extension -ine '.dll' -or
        ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Supply a regular native plugin DLL at $path."
    }
    $addonFiles.Add("plugins\$($file.BaseName)\$($file.Name)", $file.FullName)
}
foreach ($path in $ExistingScriptDirectory) {
    $dir = Get-Item -LiteralPath $path
    if (!$dir.PSIsContainer -or ($dir.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        !(Test-Path -LiteralPath (Join-Path $dir.FullName 'init.lua') -PathType Leaf)) {
        throw "Supply a local Lua add-on directory at $path."
    }
    foreach ($file in Get-ChildItem -LiteralPath $dir.FullName -File -Recurse -Force) {
        if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Expected a regular add-on file at $($file.FullName)."
        }
        $relative = [IO.Path]::GetRelativePath($dir.FullName, $file.FullName)
        $addonFiles.Add("scripts\$($dir.Name)\$relative", $file.FullName)
    }
}
$testIdentity = $AppId -ne 'Hailstone.Client'
$registeredInstall = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\${AppId}_is1"
$possibleInstallKeys = @(
    $registeredInstall
    "HKCU:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\${AppId}_is1"
    "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\${AppId}_is1"
    "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\${AppId}_is1"
)

function Get-Registrations {
    $possibleInstallKeys | Where-Object { Test-Path -LiteralPath $_ }
}

if ($testIdentity) {
    # An upgrade runs without /DIR. Check the test build marker before executing either installer.
    foreach ($path in $firstInstaller, $Installer) {
        $description = (Get-Item -LiteralPath $path).VersionInfo.FileDescription
        if (!$description -or $description.Trim() -ne $AppId) {
            throw "The installer at $path must be compiled with AppId and VersionInfoDescription set to $AppId."
        }
    }
}
if (@(Get-Registrations).Count) { throw "An existing installation already uses $AppId." }
if (!$testIdentity -and (Test-Path -LiteralPath 'C:\hailstone')) {
    throw 'C:\hailstone exists. Use an isolated Windows environment or separately compiled upgrade test installers.'
}
if ($upgrade -and (Get-FileHash -LiteralPath $PreviousInstaller).Hash -eq (Get-FileHash -LiteralPath $Installer).Hash) {
    throw 'Supply two different Hailstone installers to test an upgrade.'
}

function Get-PayloadHashes([string]$Folder) {
    $resolved = (Resolve-Path -LiteralPath $Folder).ProviderPath
    $hashes = @{}
    foreach ($file in Get-ChildItem -LiteralPath $resolved -Recurse -File -Force) {
        $relative = [IO.Path]::GetRelativePath($resolved, $file.FullName)
        $hashes[$relative] = (Get-FileHash -LiteralPath $file.FullName).Hash
    }
    if (!$hashes.ContainsKey('hailstone.exe')) { throw "Missing hailstone.exe in payload $resolved" }
    return $hashes
}

function Assert-Hashes([string]$Folder, [hashtable]$Hashes) {
    foreach ($relative in $Hashes.Keys) {
        $path = Join-Path $Folder $relative
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-FileHash -LiteralPath $path).Hash -ne $Hashes[$relative]) {
            throw "Missing file or SHA-256 mismatch at $path"
        }
    }
}

$newPayload = Get-PayloadHashes $Stage
$oldPayload = if ($PreviousStage) { Get-PayloadHashes $PreviousStage }
$run = 'installer-upgrade-' + [Guid]::NewGuid().ToString('N').Substring(0, 12)
$logs = (New-Item -ItemType Directory -Path (Join-Path $LogDirectory $run) -Force).FullName
$install = Join-Path $logs 'Hailstone custom install'
$uninstaller = Join-Path $install 'unins000.exe'
$summary = Join-Path $logs 'summary.txt'
$commonArgs = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')

function Write-TestResult([string]$Message) {
    Add-Content -LiteralPath $summary -Value $Message -Encoding utf8
    Write-Host $Message
}

function Invoke-InstallerStep([string]$Program, [string]$LogName, [string[]]$Options = @()) {
    $logPath = Join-Path $logs $LogName
    $arguments = $commonArgs + ('/LOG="{0}"' -f $logPath) + $Options
    $process = Start-Process -FilePath $Program -ArgumentList $arguments -PassThru -Wait -WindowStyle Hidden
    if ($process.ExitCode -ne 0) {
        throw "$LogName failed with exit code $($process.ExitCode). See $logPath"
    }
}

function Assert-InstallRegistration([string]$DisplayVersion) {
    $keys = @(Get-Registrations)
    if ($keys.Count -ne 1 -or $keys[0] -ne $registeredInstall) {
        throw 'Expected exactly one per-user Hailstone uninstall registration.'
    }
    $entry = Get-ItemProperty -LiteralPath $registeredInstall
    if ($entry.InstallLocation.TrimEnd('\') -ne $install) {
        throw "Hailstone was registered at $($entry.InstallLocation) instead of $install."
    }
    if ($DisplayVersion) {
        $version = $entry.PSObject.Properties['DisplayVersion']
        if (!$version -or $version.Value -cne $DisplayVersion) {
            $actual = if ($version) { $version.Value } else { '<missing>' }
            throw "The uninstall entry reports version '$actual', expected '$DisplayVersion'."
        }
    }
    $uninstallers = @(Get-ChildItem -LiteralPath $install -Filter 'unins*.exe' -File)
    if ($uninstallers.Count -ne 1 -or $uninstallers[0].FullName -ne $uninstaller) {
        throw 'Expected a single uninstaller in the fixture installation folder.'
    }
}

$setupOptions = @('/SP-', '/NOICONS', '/TASKS=""', '/NOCLOSEAPPLICATIONS')
Write-TestResult "Upgrade test logs $logs"
Write-TestResult "First installer $firstInstaller"
Write-TestResult "Current installer $Installer"
Write-TestResult "Installer application ID $AppId"
try {
    Invoke-InstallerStep $firstInstaller '01-first-install.log' ($setupOptions + ('/DIR="{0}"' -f $install))
    Assert-InstallRegistration
    if ($oldPayload) { Assert-Hashes $install $oldPayload }
    $oldExeHash = (Get-FileHash -LiteralPath (Join-Path $install 'hailstone.exe')).Hash
    if ($upgrade) {
        if ($oldExeHash -eq $newPayload['hailstone.exe']) { throw 'Both installers contain the same Hailstone executable.' }
    } else {
        Assert-Hashes $install $newPayload
        Set-Content -LiteralPath (Join-Path $install 'hailstone.exe') -Value 'Fixture damaged runtime' -Encoding ascii
    }
    Write-TestResult "Installed the first client in $install"
    Write-TestResult "First hailstone.exe SHA-256 $oldExeHash"

    $fixtures = @{
        'hailstone.ini'              = 'Fixture player preferences'
        'hailstone-windows.ini'      = 'Fixture window positions'
        'servers\fixture\player.ini' = 'Fixture character settings'
        'cfg\fixture.cfg'             = 'Fixture macro configuration'
        'ui\settings\fixture.ini'    = 'Fixture UI settings'
        'ui\custom-fixture\init.lua' = '-- Fixture custom UI'
        'addons\fixture\init.lua'    = '-- Fixture custom add-on'
        'ui-addons\fixture\init.lua' = '-- Fixture custom UI add-on'
        'update\keep.txt'            = 'Fixture updater data'
    }
    $profileHashes = @{}
    foreach ($relative in $fixtures.Keys) {
        $path = Join-Path $install $relative
        if ($newPayload.ContainsKey($relative) -or (Test-Path -LiteralPath $path)) {
            throw "A shipped file conflicts with the profile fixture at $relative"
        }
        [void][IO.Directory]::CreateDirectory((Split-Path -LiteralPath $path))
        Set-Content -LiteralPath $path -Value $fixtures[$relative] -Encoding ascii
        $profileHashes[$relative] = (Get-FileHash -LiteralPath $path).Hash
    }
    foreach ($relative in $addonFiles.Keys) {
        $path = Join-Path $install $relative
        if ($newPayload.ContainsKey($relative) -or (Test-Path -LiteralPath $path)) {
            throw "A shipped file conflicts with the add-on fixture at $relative"
        }
        [void][IO.Directory]::CreateDirectory((Split-Path -LiteralPath $path))
        $original = $addonFiles[$relative]
        [IO.File]::Copy($original, $path)
        $profileHashes[$relative] = (Get-FileHash -LiteralPath $original).Hash
    }

    # The new setup must discover the old directory through its own registry lookup.
    Invoke-InstallerStep $Installer '02-upgrade.log' $setupOptions
    Assert-InstallRegistration -DisplayVersion $ExpectedDisplayVersion
    Assert-Hashes $install $newPayload
    Assert-Hashes $install $profileHashes
    Write-TestResult "Setup selected the existing folder without /DIR and retained one per-user uninstall entry."
    if ($ExpectedDisplayVersion) { Write-TestResult "Apps & Features reports $ExpectedDisplayVersion." }
    Write-TestResult "All $($newPayload.Count) current payload files and $($profileHashes.Count) profile fixtures matched."
    Write-TestResult "Current hailstone.exe SHA-256 $($newPayload['hailstone.exe'])"

    $helpOutput = Join-Path $logs '02-client-help.txt'
    $client = Start-Process -FilePath (Join-Path $install 'hailstone.exe') -ArgumentList '--help' `
        -WorkingDirectory $install -PassThru -WindowStyle Hidden -RedirectStandardOutput $helpOutput
    if (!$client.WaitForExit(30000)) {
        $client.Kill()
        $client.WaitForExit()
        throw 'The upgraded client timed out while printing help.'
    }
    if ($client.ExitCode -ne 0 -or !(Select-String -LiteralPath $helpOutput -SimpleMatch '--game-dir' -Quiet)) {
        throw "The upgraded client did not print its help text. See $helpOutput"
    }
    Write-TestResult 'The upgraded client started and printed its help text.'

    Invoke-InstallerStep $uninstaller '03-uninstall.log'
    if (@(Get-Registrations).Count) { throw 'A Hailstone uninstall registration remains.' }
    $installedFiles = @($newPayload.Keys)
    if ($oldPayload) { $installedFiles += $oldPayload.Keys }
    foreach ($relative in $installedFiles) {
        if (Test-Path -LiteralPath (Join-Path $install $relative)) { throw "Uninstall left a shipped file at $relative" }
    }
    Assert-Hashes $install $profileHashes
    Write-TestResult 'Uninstall removed registered program files and preserved all player fixtures.'
} catch {
    Write-TestResult "FAILED $($_.Exception.Message)"
    throw
} finally {
    # Only clean up the installation that this run created inside its unique fixture folder.
    if (Test-Path -LiteralPath $uninstaller -PathType Leaf) {
        $entry = Get-ItemProperty -LiteralPath $registeredInstall -ErrorAction SilentlyContinue
        if (!$entry -or $entry.InstallLocation.TrimEnd('\') -eq $install) {
            Invoke-InstallerStep $uninstaller '99-cleanup.log'
        }
    }
}
