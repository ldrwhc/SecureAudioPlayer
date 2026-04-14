param(
    [string]$AaDir = $PSScriptRoot,
    [string]$Key = "BusAnnouncement@2026",
    [string]$InstallerName = "SecureAudioPlayer_Setup.exe",
    [ValidateSet("combined", "split")]
    [string]$PackMode = "split",
    [switch]$PackOnly
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

function Set-StepProgress {
    param(
        [int]$Current,
        [int]$Total,
        [string]$Status
    )
    $percent = [int](($Current / [Math]::Max($Total, 1)) * 100)
    Write-Progress -Id 1 -Activity "Build install package" -Status $Status -PercentComplete $percent
}

function Ensure-Dir {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -Path $Path -ItemType Directory | Out-Null
    }
}

function Remove-DirIfExists {
    param([string]$Path)
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function Run-ProcessChecked {
    param(
        [string]$FilePath,
        [string[]]$ArgumentList,
        [string]$WorkingDirectory
    )
    $p = Start-Process -FilePath $FilePath -ArgumentList $ArgumentList -WorkingDirectory $WorkingDirectory -PassThru -Wait -WindowStyle Hidden
    if ($p.ExitCode -ne 0) {
        throw "Command failed: $FilePath $($ArgumentList -join ' ') ; exit code=$($p.ExitCode)"
    }
}

function Resolve-WinDeployQt {
    $cmd = Get-Command windeployqt.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $qmake = Get-Command qmake.exe -ErrorAction SilentlyContinue
    if ($qmake) {
        $candidate = Join-Path (Split-Path -Parent $qmake.Source) "windeployqt.exe"
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw "windeployqt.exe not found in PATH."
}

function Resolve-InnoSetupCompiler {
    $candidates = @()
    $cmd = Get-Command iscc.exe -ErrorAction SilentlyContinue
    if ($cmd) { $candidates += $cmd.Source }
    $candidates += "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
    $candidates += "C:\Program Files\Inno Setup 6\ISCC.exe"

    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c)) { return $c }
    }
    return $null
}

function New-InnoSetupIss {
    param(
        [string]$IssPath,
        [string]$AppId,
        [string]$AppName,
        [string]$AppVersion
    )

    $content = @"
[Setup]
AppId=$AppId
AppName=$AppName
AppVersion=$AppVersion
AppPublisher=BusAnnouncement
DefaultDirName={autopf}\$AppName
DefaultGroupName=$AppName
DisableProgramGroupPage=yes
LicenseFile={#MyLicenseFile}
OutputDir={#MyOutputDir}
OutputBaseFilename={#MyOutputBase}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create desktop icon"; GroupDescription: "Additional tasks:"

[Files]
Source: "{#MySourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\$AppName"; Filename: "{app}\SecureAudioPlayer.exe"
Name: "{autodesktop}\$AppName"; Filename: "{app}\SecureAudioPlayer.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\SecureAudioPlayer.exe"; Description: "Launch player after setup"; Flags: nowait postinstall skipifsilent
"@
    Set-Content -LiteralPath $IssPath -Value $content -Encoding UTF8
}

function New-IExpressSed {
    param(
        [string]$SedPath,
        [string]$TargetExe,
        [string]$SourceDir
    )

    $targetEscaped = $TargetExe.Replace('\', '\\')
    $sourceEscaped = ($SourceDir.TrimEnd('\') + '\').Replace('\', '\\')
    $content = @"
[Version]
Class=IEXPRESS
SEDVersion=3
[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=0
HideExtractAnimation=1
UseLongFileName=1
InsideCompressed=0
CAB_FixedSize=0
CAB_ResvCodeSigning=0
RebootMode=N
InstallPrompt=
DisplayLicense=
FinishMessage=Install completed.
TargetName=$targetEscaped
FriendlyName=Secure Audio Player Installer
AppLaunched=cmd /c install.cmd
PostInstallCmd=<None>
AdminQuietInstCmd=cmd /c install.cmd
UserQuietInstCmd=cmd /c install.cmd
SourceFiles=SourceFiles
[SourceFiles]
SourceFiles0=$sourceEscaped
[SourceFiles0]
install.cmd=
payload.zip=
"@
    Set-Content -LiteralPath $SedPath -Value $content -Encoding ASCII
}

$aa = (Resolve-Path -LiteralPath $AaDir).Path
$distDir = Join-Path $aa "dist"
$workDir = Join-Path $aa "_installer_work"
$appStageDir = Join-Path $workDir "app_stage"
$payloadDir = Join-Path $workDir "payload"
$installerPath = Join-Path $distDir $InstallerName
$packScript = Join-Path $aa "pack_secure_audio.ps1"
$buildBat = Join-Path $aa "build_secure_player.bat"
$playerExe = Join-Path $aa "release\SecureAudioPlayer.exe"
$embeddedQrc = Join-Path $aa "embedded_payload.qrc"

Ensure-Dir $distDir
Remove-DirIfExists $workDir
Ensure-Dir $workDir
Ensure-Dir $appStageDir
Ensure-Dir $payloadDir

$totalSteps = 6
if ($PackOnly) { $totalSteps = 2 }

Set-StepProgress -Current 1 -Total $totalSteps -Status "Pack encrypted audio"
& powershell -NoProfile -ExecutionPolicy Bypass -File $packScript -AaDir $aa -Key $Key -PackMode $PackMode -ShowProgress
if ($LASTEXITCODE -ne 0) {
    throw "pack_secure_audio.ps1 failed."
}
if (-not (Test-Path -LiteralPath $embeddedQrc)) {
    throw "embedded_payload.qrc not found. Pack step did not emit embedded resource."
}

if ($PackOnly) {
    Set-StepProgress -Current 2 -Total $totalSteps -Status "Done (pack only)"
    Write-Progress -Id 1 -Activity "Build install package" -Completed
    Write-Host "[DONE] pack only finished."
    exit 0
}

Set-StepProgress -Current 2 -Total $totalSteps -Status "Build player executable"
Run-ProcessChecked -FilePath "cmd.exe" -ArgumentList @("/c", "`"$buildBat`"") -WorkingDirectory $aa
if (-not (Test-Path -LiteralPath $playerExe)) {
    throw "Player exe not found: $playerExe"
}

Set-StepProgress -Current 3 -Total $totalSteps -Status "Deploy Qt runtime"
Copy-Item -LiteralPath $playerExe -Destination (Join-Path $appStageDir "SecureAudioPlayer.exe") -Force
$windeployqt = Resolve-WinDeployQt
Run-ProcessChecked -FilePath $windeployqt -ArgumentList @(
    "--release",
    "--no-translations",
    "--compiler-runtime",
    (Join-Path $appStageDir "SecureAudioPlayer.exe")
) -WorkingDirectory $appStageDir

Set-StepProgress -Current 4 -Total $totalSteps -Status "Assemble app resources"
# Resource payload (pak + seed zip) is embedded into SecureAudioPlayer.exe via embedded_payload.qrc.

$licensePath = Join-Path $aa "EULA.txt"
if (-not (Test-Path -LiteralPath $licensePath)) {
    @(
        "Secure Audio Player License Agreement",
        "",
        "1. This software is for authorized users in legal scenarios only.",
        "2. Unauthorized copying, distribution, or reverse engineering is prohibited.",
        "3. Please comply with all applicable laws and regulations."
    ) | Set-Content -LiteralPath $licensePath -Encoding UTF8
}

Set-StepProgress -Current 5 -Total $totalSteps -Status "Build installer"
if (Test-Path -LiteralPath $installerPath) {
    Remove-Item -LiteralPath $installerPath -Force
}
$iscc = Resolve-InnoSetupCompiler
$outputBase = [System.IO.Path]::GetFileNameWithoutExtension($InstallerName)

if ($iscc) {
    Write-Host "[INFO] Inno Setup detected, building wizard installer."
    $issPath = Join-Path $workDir "installer.iss"
    New-InnoSetupIss -IssPath $issPath -AppId "{F8E4E2E3-82B7-4B78-A00C-8D7C54133711}" -AppName "Secure Audio Player" -AppVersion "1.0.0"
    Run-ProcessChecked -FilePath $iscc -ArgumentList @(
        "/Qp",
        "/DMySourceDir=$appStageDir",
        "/DMyOutputDir=$distDir",
        "/DMyOutputBase=$outputBase",
        "/DMyLicenseFile=$licensePath",
        $issPath
    ) -WorkingDirectory $workDir
} else {
    Write-Host "[WARN] Inno Setup not found. Fallback to IExpress installer (no license/directory wizard)."
    $payloadZip = Join-Path $payloadDir "payload.zip"
    if (Test-Path -LiteralPath $payloadZip) {
        Remove-Item -LiteralPath $payloadZip -Force
    }
    Compress-Archive -Path (Join-Path $appStageDir "*") -DestinationPath $payloadZip -Force

    $installCmd = @'
@echo off
setlocal
set "TARGET=%LOCALAPPDATA%\SecureAudioPlayer"
if exist "%TARGET%" rmdir /s /q "%TARGET%"
mkdir "%TARGET%" >nul 2>nul
powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -Path '%~dp0payload.zip' -DestinationPath '%TARGET%' -Force"
if errorlevel 1 exit /b 1
if exist "%TARGET%\SecureAudioPlayer.exe" start "" "%TARGET%\SecureAudioPlayer.exe"
exit /b 0
'@
    Set-Content -LiteralPath (Join-Path $payloadDir "install.cmd") -Value $installCmd -Encoding ASCII
    $sedPath = Join-Path $workDir "installer.sed"
    New-IExpressSed -SedPath $sedPath -TargetExe $installerPath -SourceDir $payloadDir
    Run-ProcessChecked -FilePath "iexpress.exe" -ArgumentList @("/N", "/Q", $sedPath) -WorkingDirectory $workDir
}

if (-not (Test-Path -LiteralPath $installerPath)) {
    throw "Installer not generated: $installerPath"
}

Set-StepProgress -Current 6 -Total $totalSteps -Status "Done"
Write-Progress -Id 1 -Activity "Build install package" -Completed

# Keep only installer output; clean local build artifacts to avoid confusion with bare exe.
Remove-DirIfExists (Join-Path $aa "release")
Remove-DirIfExists (Join-Path $aa "debug")
if (Test-Path -LiteralPath (Join-Path $aa "Makefile")) { Remove-Item -LiteralPath (Join-Path $aa "Makefile") -Force }
if (Test-Path -LiteralPath (Join-Path $aa "Makefile.Debug")) { Remove-Item -LiteralPath (Join-Path $aa "Makefile.Debug") -Force }
if (Test-Path -LiteralPath (Join-Path $aa "Makefile.Release")) { Remove-Item -LiteralPath (Join-Path $aa "Makefile.Release") -Force }
Remove-DirIfExists $workDir

Write-Host "[DONE] Installer created:"
Write-Host "       $installerPath"
