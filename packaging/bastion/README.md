# Bastion Hailstone Windows releases

The fork builds a Windows x64 release from a `bastion-v*` tag on `bastion-release`. A release has one Hailstone runtime, one Windows Setup for fresh installs and manual repairs, and a signed channel manifest for the Bastion Launcher. The launcher uses a separate bootstrap helper ZIP. Updating Hailstone through the launcher preserves the install path and player data.

## Routine Production updates

Use this request in the update workspace.

> Build and publish the next Bastion Production Hailstone update from `<reviewed ref>`. Stage the tested GitHub draft and give me the exact offline signing command. When I return the signed manifest, publish the draft, advance the Production feed, and verify what the Bastion Launcher sees.

1. Build and test the exact increasing `bastion-v*` tag, then stage its Setup, runtime and source in one GitHub draft. Record the tag, full commit, candidate directory, draft URL and signing command for the operator.
2. The operator signs the candidate with the offline key and returns the signed `hailstone-update.txt`. The update workspace pairs that manifest with the byte-identical ZIP and Setup from the original candidate.
3. Run `Publish-GitHubProduction.ps1 -PublishDraft` with the approved commit, signed files and pinned public key. Report the live release, the verified feed version and the Production launcher check.

Launcher 2.9.27 and later using this Production feed check for each new signed version from the same GitHub branch. Routine Hailstone updates require only a client release and feed promotion.

## Configure the fork

1. Push the `bastion-release` branch to `fryguy503/hailstone-client`. Tags must point to reviewed commits reachable from that branch. GitHub branch and tag rules can restrict who can publish or move release references.
2. Create a dedicated Bastion Ed25519 core updater key on an offline machine. Configure the repository Actions **variable** `BASTION_HAILSTONE_UPDATE_PUBLIC_KEY` with its 64-digit public key. Configure the same key in the launcher build. Keep the secret key outside the repositories and GitHub Actions. The upstream suggested add-on index keeps its own trust key.
3. Limit write access to the GitHub `bastion-production-feed` branch and protect it against force pushes and deletion. Publish the Production runtime ZIP and Setup as assets on each exact `bastion-v*` GitHub Release. The promotion script creates the feed branch on first use and advances it only after verifying those assets.

The Windows candidate workflow takes no release or deployment credentials. It builds on Windows Server 2022 with MSVC v143 14.44 and SDK 10.0.26100.0. It verifies pinned glslang and Inno Setup versions and uses the client's pinned dependencies. Tags trigger candidate builds without requiring a workflow on the fork's default branch.

## Build and check a candidate

Merge the upstream revision and Bastion changes you intend to ship into `bastion-release`. Decide the actual increasing version once and tag the release commit. For example, `bastion-v0.7.4-bastion.6` builds client version `0.7.4-bastion.6`. The initial version must sort above the `0.7.3-beta` internal version used by the previous Preview 04 and 05 installers.

The workflow runs all configured Windows CTest shards and an isolated Inno Setup install, repair and uninstall. It records the immutable tag, commit and SHA-256 hashes in `release-input.json`. Download the candidate artifact from that exact run. Its `release/` contents are:

| Files | Purpose |
| --- | --- |
| `hailstone-<version>-windows-x64.zip` and `hailstone-update.txt` | Complete runtime and unsigned manifest input. |
| `Hailstone-<version>-<revision>-Windows-x64-Setup.exe` | Setup from the same runtime stage. |
| `Hailstone-<version>-Source.zip` | Corresponding tracked source at the release commit. |
| `hailstone-update-helper-windows-x64.zip` | Bootstrap helper for embedding into the Bastion Launcher build. |
| `bastion-update-key.pub`, `release-input.json`, and `SHA256SUMS` | Build identity and package verification. |

For a local build, fetch the tag and the protected branch, check out the tag in a clean workspace, and run:

```powershell
./packaging/bastion/Build-WindowsRelease.ps1 `
    -ReleaseTag $tag -ExpectedCommit (git rev-parse HEAD) `
    -OutputDirectory $emptyBuildDirectory -PublicKey $bastionPublicKey
```

The local build needs PowerShell 7, Git, Python, the Visual Studio C++ workload, and Inno Setup 6.7.4. Use `-Iscc` or `-Glslang` to supply an existing matching tool. Run `-TestInstaller` on an isolated Windows VM without an existing Hailstone registration. To check real installer upgrades, use `Test-InstallerUpgrade.ps1 -PreviousInstaller <verified-old-setup> -Installer <candidate-setup> -Stage <output-directory>/payload` on the same kind of VM. Run the existing Preview 04 and Preview 05 scenarios before the first promotion. These checks exercise the runtime and user file fixtures. Validate a full linked-account launch through the launcher separately before release.

For repeated workstation builds, pass `-BuildDirectory` pointing to a dedicated Ninja build for the exact source checkout. Use a new `-OutputDirectory` for each candidate. `-DependencyCacheDirectory` accepts a prepared dependency cache with a `cache.json` recipe matching the checked-out dependency definitions. `-Python` can point to a specific interpreter.

The GitHub build uses MSVC 14.44. A workstation can explicitly select an installed toolset with `-MsvcToolset`, which requires a separate build cache if the compiler changes. Both paths use Windows SDK 10.0.26100.0. The candidate records the MSVC toolset, SDK, and release script SHA-256. Approve the exact candidate and its runtime hash before signing or embedding its launcher helper.

Verify the helper ZIP against the chosen candidate's SHA-256 before embedding it with `tools/build-patcher-release.ps1` in `eqemupatcher`. The `runtimeSha256` field in `release-input.json` is the hash of the `hailstone.exe` inside the ZIP and Setup. Use `version=runtimeSha256` as a launcher bootstrap identity when distributing a Setup with the same launcher release. The helper ZIP is a launcher build input and does not go on the client feed.

## Prepare Production

Give the release its own version heading in `CHANGELOG.md` so the client’s **What's new** panel displays the reviewed player notes. The managed client uses the fork as its project page and leaves core update checks to the launcher.

Configure the Production launcher with the verified GitHub-capable helper ZIP and the chosen candidate's bootstrap identity. The launcher uses the dedicated Bastion update public key and reads the signed `stable` manifest from `https://raw.githubusercontent.com/fryguy503/hailstone-client/bastion-production-feed/hailstone-update.txt`. It downloads the ZIP and Setup from the exact `bastion-v<version>` GitHub Release named by that signed version. The same Windows Setup also supports manual installation and repair.

## Sign and publish Production on GitHub

Stage the release from the tested candidate before handing it off for signing. Confirm the exact tag is reachable from the remote `bastion-release` branch. Keep the draft private until the signed manifest is ready.

```powershell
$repo = 'fryguy503/hailstone-client'
$version = $tag.Substring('bastion-v'.Length)
$setup = Join-Path $candidate "Hailstone-$version-$($approvedCommit.Substring(0, 8))-Windows-x64-Setup.exe"
$runtime = Join-Path $candidate "hailstone-$version-windows-x64.zip"
$source = Join-Path $candidate "Hailstone-$version-Source.zip"

git push https://github.com/fryguy503/hailstone-client.git "refs/tags/$tag"
if ($LASTEXITCODE -ne 0) { throw 'Could not push the reviewed tag.' }
gh release create $tag $setup $runtime $source --repo $repo --verify-tag --draft --latest=false `
    --title "Hailstone $version" --notes-file $releaseNotes
if ($LASTEXITCODE -ne 0) { throw 'Could not stage the reviewed GitHub draft.' }
```

Use an independently trusted `hailstone_sign` binary built from reviewed source on the offline signing system. Confirm the candidate's provenance and its exact tagged commit before signing.

```powershell
./packaging/bastion/Sign-WindowsRelease.ps1 `
    -CandidateDirectory $candidate -OutputDirectory $signedProduction `
    -ExpectedTag $tag -ExpectedCommit $approvedCommit -Channel stable `
    -SignTool $trustedSigner -SigningKey $offlineSecretKey -PublicKey $pinnedPublicKeyFile
```

The signer verifies the candidate files and checks `runtimeSha256` against `hailstone.exe` inside the ZIP. It adds that hash as `bastion-runtime-sha256` and adds the Setup byte count, SHA-256 and filename as `bastion-setup`. The launcher verifies Setup before running it and uses the signed runtime hash to recognize the installed executable.

Return the signed manifest to the update workspace. Put it in a directory with the exact Setup and runtime ZIP from the candidate. Use PowerShell 7, authenticated GitHub CLI and Git to publish and verify the release in one command.

```powershell
./packaging/bastion/Publish-GitHubProduction.ps1 `
    -SignedDirectory $signedProduction -ExpectedCommit $approvedCommit `
    -SignTool $trustedVerifier -PublicKey $pinnedPublicKeyFile -PublishDraft
```

The script verifies the signed local files and remote tag, checks that the feed can advance, and verifies the draft asset sizes and SHA-256 hashes. It publishes the draft without changing GitHub Latest, checks both public downloads, then advances the dedicated feed. Run the same command to resume if the release is already public. `-WhatIf` verifies only the local signed manifest, Setup, ZIP and executable. Keep each published version's asset bytes available for downloads already in progress.

## Publish Preview to the updater host

To use the existing Preview channel, sign the candidate in a fresh output directory with `-Channel preview`. Provision `/hailstone/preview/` on the HTTPS updater host and serve the manifest with revalidation. Keep the ZIP, Setup and manifest beside one another and disable redirects for this location.

```powershell
./packaging/bastion/Publish-WindowsRelease.ps1 `
    -SignedDirectory $signedPreview -PublishRoot $updaterHailstoneRoot `
    -PublicUri 'https://patcher.bastiongame.com/hailstone/' -Channel preview `
    -SignTool $trustedVerifier -PublicKey $pinnedPublicKeyFile
```

Use `-WhatIf` to verify the signed local inputs before deploying. Preview promotion verifies the ZIP and Setup bytes over HTTPS before replacing the manifest, then checks the public manifest. Existing Preview manifests without a `bastion-setup` record still support updates. Use a new release version to add Setup when a manifest for the current version is already live.

To withdraw a candidate, stop further promotions. Release the last known-good code with a higher Bastion version if players already received the withdrawn version. Keep older immutable package bytes available for clients completing a staged download. The launcher and client updater handle local installation recovery.
