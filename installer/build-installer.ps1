# Builds dist\BatteryHub-Setup-<version>.exe and dist\BatteryHub-<version>-portable.zip.
# Needs: BatteryHub.exe already built (make / build.bat) and Inno Setup 6 or 7 installed.
param([string]$Version = "1.0.0", [switch]$G435Preview)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
& (Join-Path $root 'scripts\verify-windows-version.ps1') -ExpectedVersion $Version

# Bundled HeadsetControl: pinned release, verified against the SHA-256 GitHub publishes for it.
$hscVersion = "4.3.0"
$hscSha256  = "e371da043710ddb8fc70af77f2ccad1cd03f8c6f49ba423e08764ad7a94a3c40"
$hscDir = Join-Path $root "third_party\headsetcontrol"
$hscExe = Join-Path $hscDir "headsetcontrol.exe"
New-Item -ItemType Directory -Force $hscDir | Out-Null
if (-not (Test-Path $hscExe) -or (Get-FileHash $hscExe -Algorithm SHA256).Hash -ne $hscSha256) {
    Write-Host "Downloading HeadsetControl $hscVersion..."
    Invoke-WebRequest -UseBasicParsing "https://github.com/Sapd/HeadsetControl/releases/download/$hscVersion/headsetcontrol-windows-x86_64.exe" -OutFile $hscExe
}
if ((Get-FileHash $hscExe -Algorithm SHA256).Hash -ne $hscSha256) {
    Remove-Item -LiteralPath $hscExe
    throw "HeadsetControl checksum mismatch - refusing to bundle it."
}

if (-not (Test-Path (Join-Path $root "BatteryHub.exe"))) { throw "Build BatteryHub.exe first (make or build.bat)." }

$iscc = foreach ($v in 7, 6) {
    foreach ($base in ${env:ProgramFiles(x86)}, $env:ProgramFiles, "$env:LOCALAPPDATA\Programs") { "$base\Inno Setup $v\ISCC.exe" }
}
$iscc = $iscc | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { $iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source }
if (-not $iscc) { throw "Inno Setup not found (https://jrsoftware.org/isdl.php)." }
$defines = @("/DAppVersion=$Version")
$output = Join-Path $root "dist"
$name = "BatteryHub"
if ($G435Preview) {
    $defines += "/DG435Preview"
    $output = Join-Path $output "G435-WirelessBattery"
    $name = "BatteryHub-G435-Preview"
}
& $iscc @defines (Join-Path $PSScriptRoot "BatteryHub.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC failed." }

# Portable zip: same files, no installer.
$stage = Join-Path $root "build\portable"
if (Test-Path -LiteralPath $stage) {
    $item = Get-Item -LiteralPath $stage -Force
    $build = Get-Item -LiteralPath (Join-Path $root "build") -Force
    if (($item.Attributes -bor $build.Attributes) -band [IO.FileAttributes]::ReparsePoint) {
        throw "Portable staging cannot use a reparse point."
    }
    Remove-Item -LiteralPath $item.FullName -Recurse -Force
}
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item BatteryHub.exe, $hscExe, installer\THIRD_PARTY_NOTICES.txt $stage
Copy-Item LICENSE (Join-Path $stage "LICENSE.txt")
$zip = "$name-$Version-portable.zip"
Compress-Archive -Path "$stage\*" -DestinationPath (Join-Path $output $zip) -Force
$setup = "$name-Setup-$Version.exe"
$assets = @($setup, $zip)
if ($G435Preview) {
    Copy-Item "$stage\*" $output
    $sourceName = "headsetcontrol-$hscVersion.tar.gz"
    $source = Join-Path $hscDir $sourceName
    $sourceSha256 = "918dcacf8aef347744b36b2d259d76880fa64b3869f0f8ef26b6576b873d28a7"
    if (-not (Test-Path $source) -or (Get-FileHash $source -Algorithm SHA256).Hash -ne $sourceSha256) {
        Invoke-WebRequest -UseBasicParsing "https://github.com/Sapd/HeadsetControl/releases/download/$hscVersion/$sourceName" -OutFile $source
    }
    if ((Get-FileHash $source -Algorithm SHA256).Hash -ne $sourceSha256) {
        throw "HeadsetControl source checksum mismatch."
    }
    Copy-Item $source $output
    $assets += $sourceName
}
# Checksums of what the app can install or users download. The maintainer signs this file on their own
# PC (scripts\sign-release.ps1) and uploads SHA256SUMS.txt.sig; the app only installs a signed setup.
# sha256sum format with LF line ends, so `sha256sum -c SHA256SUMS.txt` also works.
$sums = foreach ($asset in $assets) {
    "$((Get-FileHash (Join-Path $output $asset) -Algorithm SHA256).Hash.ToLower())  $asset"
}
[IO.File]::WriteAllText((Join-Path $output 'SHA256SUMS.txt'), (($sums -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))
Write-Host "Built $setup, $zip and SHA256SUMS.txt in $output"
