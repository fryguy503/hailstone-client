# Bastion Hailstone Windows releases

The fork builds a Windows x64 release from a `bastion-v*` tag on `bastion-release`. A release has one Hailstone runtime, one Windows Setup for fresh installs and manual repairs, and a signed channel manifest for the Bastion Launcher. The launcher uses a separate bootstrap helper ZIP. Updating Hailstone through the launcher preserves the install path and player data.

## Configure the fork

1. Push the `bastion-release` branch to `fryguy503/hailstone-client`. Tags must point to reviewed commits reachable from that branch. GitHub branch and tag rules can restrict who can publish or move release references.
2. Create a dedicated Bastion Ed25519 core updater key on an offline machine. Configure the repository Actions **variable** `BASTION_HAILSTONE_UPDATE_PUBLIC_KEY` with its 64-digit public key. Configure the same key in the launcher build. Keep the secret key outside the repositories and GitHub Actions. The upstream suggested add-on index keeps its own trust key.
3. Provision `/hailstone/preview/` and `/hailstone/stable/` on the HTTPS updater host. Serve the manifest with revalidation and versioned ZIPs with immutable caching. Disable redirects for these locations. Keep the ZIP and manifest beside one another in each channel. Limit deployment access to release maintainers.

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

Verify the helper ZIP against the chosen candidate's SHA-256 before embedding it with `tools/build-patcher-release.ps1` in `eqemupatcher`. The `runtimeSha256` field in `release-input.json` is the hash of the `hailstone.exe` inside the ZIP and Setup. Use `version=runtimeSha256` as a launcher bootstrap identity when distributing a Setup with the same launcher release. The helper ZIP is a launcher build input and does not go on the client feed.

## Sign and promote

Use an independently trusted `hailstone_sign` binary built from reviewed source on the offline signing system. Confirm the Actions run and candidate commit before signing. Publish release notes, Setup and corresponding source on the exact GitHub release tag when those assets are ready for their intended audience. The Bastion channel is published independently of GitHub Latest.

```powershell
./packaging/bastion/Sign-WindowsRelease.ps1 `
    -CandidateDirectory $candidate -OutputDirectory $signedPreview `
    -ExpectedTag $tag -ExpectedCommit $approvedCommit -Channel preview `
    -SignTool $trustedSigner -SigningKey $offlineSecretKey -PublicKey $pinnedPublicKeyFile
```

The signer verifies the candidate files and checks `runtimeSha256` against `hailstone.exe` inside the ZIP. It adds that hash as `bastion-runtime-sha256` to the signed channel manifest. The launcher can use this signed hash to recognize a new Setup install for enrollment because Setup ships the same executable. The publisher verifies the record against the ZIP before deployment. Transfer the signed manifest and ZIP to the deployment machine. Run the publication script on a machine with access to the updater web root and the public HTTPS endpoint:

```powershell
./packaging/bastion/Publish-WindowsRelease.ps1 `
    -SignedDirectory $signedPreview -PublishRoot $updaterHailstoneRoot `
    -PublicUri 'https://patcher.bastiongame.com/hailstone/' -Channel preview `
    -SignTool $trustedVerifier -PublicKey $pinnedPublicKeyFile
```

Use `-WhatIf` to verify the signed local input without deploying. A real promotion verifies the same bytes over HTTPS before replacing the manifest and checks the public manifest again afterward. Keep the approved candidate for Stable. Sign its identical package for `stable` into a fresh output directory, then publish to `stable` using the same command. A Preview signature is rejected at the Stable URI.

To withdraw a candidate, stop further promotions. Release the last known-good code with a higher Bastion version if players already received the withdrawn version. Keep older immutable package bytes available for clients completing a staged download. The launcher and client updater handle local installation recovery.
