param(
    [string]$AaDir = $PSScriptRoot,
    [string]$Key = "BusAnnouncement@2026",
    [ValidateSet("combined", "split")]
    [string]$PackMode = "split",
    [switch]$ShowProgress
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

function Protect-Bytes {
    param(
        [byte[]]$Data,
        [byte[]]$KeyBytes,
        [byte[]]$Nonce
    )

    $result = New-Object byte[] $Data.Length
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $offset = 0
        [uint32]$counter = 0
        while ($offset -lt $Data.Length) {
            $counterBytes = [BitConverter]::GetBytes($counter) # little-endian
            $seed = New-Object byte[] ($KeyBytes.Length + $Nonce.Length + 4)
            [Array]::Copy($KeyBytes, 0, $seed, 0, $KeyBytes.Length)
            [Array]::Copy($Nonce, 0, $seed, $KeyBytes.Length, $Nonce.Length)
            [Array]::Copy($counterBytes, 0, $seed, $KeyBytes.Length + $Nonce.Length, 4)
            $block = $sha.ComputeHash($seed)
            $n = [Math]::Min($block.Length, $Data.Length - $offset)
            for ($i = 0; $i -lt $n; $i++) {
                $result[$offset + $i] = $Data[$offset + $i] -bxor $block[$i]
            }
            $offset += $n
            $counter++
        }
    } finally {
        $sha.Dispose()
    }
    return ,$result
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

function Update-InnerProgress {
    param(
        [string]$Status,
        [int]$Percent
    )
    if ($ShowProgress) {
        Write-Progress -Id 2 -Activity "Pack encrypted audio" -Status $Status -PercentComplete $Percent
    }
}

function Write-EmbeddedPayloadQrc {
    param(
        [string]$QrcPath,
        [string]$PacksDir,
        [string]$SeedConfigZip,
        [string]$SeedLinesZip
    )

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("<RCC>")
    $lines.Add('  <qresource prefix="/">')

    if (Test-Path -LiteralPath $SeedConfigZip) {
        $lines.Add('    <file alias="payload/seed_config.zip">seed_config.zip</file>')
    }
    if (Test-Path -LiteralPath $SeedLinesZip) {
        $lines.Add('    <file alias="payload/seed_lines.zip">seed_lines.zip</file>')
    }

    Get-ChildItem -LiteralPath $PacksDir -File -Filter "*.pak" -ErrorAction SilentlyContinue |
        Sort-Object Name |
        ForEach-Object {
            $alias = [System.Security.SecurityElement]::Escape(("payload/packs/" + $_.Name).Replace('\', '/'))
            $path = [System.Security.SecurityElement]::Escape(("packs/" + $_.Name).Replace('\', '/'))
            $lines.Add("    <file alias=""$alias"">$path</file>")
        }

    $lines.Add("  </qresource>")
    $lines.Add("</RCC>")
    [System.IO.File]::WriteAllText($QrcPath, ($lines -join [Environment]::NewLine), (New-Object System.Text.UTF8Encoding($false)))
}

function Get-SafePakName {
    param(
        [string]$SourceName,
        [hashtable]$Used
    )
    if ([string]::IsNullOrWhiteSpace($SourceName)) {
        $SourceName = "pack"
    }
    $base = [System.Text.RegularExpressions.Regex]::Replace($SourceName, '[^A-Za-z0-9._-]', '_')
    $base = $base.Trim('_', '.', ' ')
    if ([string]::IsNullOrWhiteSpace($base)) {
        $base = "pack"
    }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($SourceName)
    $sha = [System.Security.Cryptography.SHA1]::Create()
    try {
        $hash = ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString("x2") }) -join ""
    } finally {
        $sha.Dispose()
    }
    $short = $hash.Substring(0, 8)
    $candidate = "$base" + "_" + "$short" + ".pak"
    $idx = 1
    while ($Used.ContainsKey($candidate.ToLower())) {
        $candidate = "$base" + "_" + "$short" + "_" + "$idx" + ".pak"
        $idx++
    }
    $Used[$candidate.ToLower()] = $true
    return $candidate
}

function Get-SafeAliasFileName {
    param(
        [string]$RelativePath,
        [hashtable]$Used
    )
    $origName = [System.IO.Path]::GetFileName($RelativePath)
    $ext = [System.IO.Path]::GetExtension($origName).ToLower()
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($origName)
    $safeStem = [System.Text.RegularExpressions.Regex]::Replace($stem, '[^A-Za-z0-9._-]', '_')
    $safeStem = $safeStem.Trim('_', '.', ' ')
    if ([string]::IsNullOrWhiteSpace($safeStem)) {
        $safeStem = "audio"
    }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($RelativePath)
    $sha = [System.Security.Cryptography.SHA1]::Create()
    try {
        $hash = ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString("x2") }) -join ""
    } finally {
        $sha.Dispose()
    }
    $short = $hash.Substring(0, 10)
    $candidate = "$safeStem" + "_" + "$short" + "$ext"
    $idx = 1
    while ($Used.ContainsKey($candidate.ToLower())) {
        $candidate = "$safeStem" + "_" + "$short" + "_" + "$idx" + "$ext"
        $idx++
    }
    $Used[$candidate.ToLower()] = $true
    return $candidate
}

function Get-PackKind {
    param([string]$SourceName)
    if ([string]::IsNullOrWhiteSpace($SourceName)) { return "misc" }
    $n = $SourceName.ToLower()
    $tipWord = (([string][char]0x63D0) + [char]0x793A).ToLower()
    $dispatchWord = (([string][char]0x53D1) + [char]0x8F66 + [char]0x901A + [char]0x77E5).ToLower()
    if ($n -like "00concateng*") { return "concat_eng" }
    if ($n -like "00concat*") { return "concat" }
    if ($n -like "template*") { return "template" }
    if ($n -like "00prompt*" -or $n -like "tips*" -or $n -like "*$tipWord*" -or $n -like "*$dispatchWord*") { return "prompt" }
    return "misc"
}

$aa = (Resolve-Path -LiteralPath $AaDir).Path
$root = (Resolve-Path -LiteralPath (Join-Path $aa "..")).Path

$packsDir = Join-Path $aa "packs"
$linesDir = Join-Path $aa "lines"
$configDir = Join-Path $aa "config"
$seedConfigZip = Join-Path $aa "seed_config.zip"
$seedLinesZip = Join-Path $aa "seed_lines.zip"
$embeddedQrc = Join-Path $aa "embedded_payload.qrc"
$playerSrcDir = Join-Path $aa "player_src"
$legacyTemplateDir = Join-Path $aa "template"

Ensure-Dir $packsDir
Ensure-Dir $linesDir
Ensure-Dir $configDir
Ensure-Dir $playerSrcDir
if (Test-Path -LiteralPath $legacyTemplateDir) {
    Remove-DirIfExists $legacyTemplateDir
}

Get-ChildItem -LiteralPath $packsDir -File -Filter "*.pak" -ErrorAction SilentlyContinue | ForEach-Object {
    Remove-Item -LiteralPath $_.FullName -Force
}

$templateFiles = @(
    "secure_player.cpp",
    "SecurePlayer.pro",
    "embedded_payload.qrc",
    "build_secure_player.bat",
    "pack_secure_audio.bat",
    "make_install_package.ps1",
    "pack_secure_audio.ps1",
    "README_usage.txt"
)

foreach ($name in $templateFiles) {
    $src = Join-Path $aa $name
    if (Test-Path -LiteralPath $src) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $playerSrcDir $name) -Force
    }
}
Update-InnerProgress -Status "Prepare folders and source files" -Percent 10

$audioExt = @(".mp3", ".wav", ".m4a", ".ogg", ".flac", ".aac", ".wma")
$tipPrefix = ([string][char]0x63D0) + [char]0x793A
$sourceDirs = Get-ChildItem -LiteralPath $root -Directory | Where-Object {
    $_.Name -like "00concat*" -or
    $_.Name -like "00prompt*" -or
    $_.Name -like "tips*" -or
    $_.Name -like "$tipPrefix*" -or
    $_.Name -like "template*"
}
$enterModeName = ([string][char]0x8FDB)+[char]0x5165+[char]0x516C+[char]0x4EA4+[char]0x7EBF+[char]0x8DEF+[char]0x9009+[char]0x62E9+[char]0x6A21+[char]0x5F0F
$switchLineName = ([string][char]0x5207)+[char]0x6362+[char]0x7EBF+[char]0x8DEF+[char]0x4E3A
$standalonePromptNames = @(
    "$enterModeName.wav",
    "$enterModeName.mp3",
    "$switchLineName.wav",
    "$switchLineName.mp3"
)
$standalonePromptFiles = @()
foreach ($n in $standalonePromptNames) {
    $cand = Join-Path $root $n
    if (Test-Path -LiteralPath $cand) {
        $standalonePromptFiles += (Get-Item -LiteralPath $cand)
    }
}

if ((-not $sourceDirs) -and (-not $standalonePromptFiles)) {
    throw "No source folder found under $root. Expected folders like 00concat* or 00prompt*."
}

$generatedPaks = New-Object System.Collections.Generic.List[string]
$usedPakNames = @{}
$packageAliasMaps = @{}
$packageKinds = @{}
$keyBytes = [System.Text.Encoding]::UTF8.GetBytes($Key)
[byte[]]$magic = 0x42,0x55,0x53,0x41,0x55,0x44,0x31,0x00 # BUSAUD1\0

function Build-EncryptedPak {
    param(
        [string]$StagingDir,
        [string]$PakName
    )
    $zipPath = Join-Path ([System.IO.Path]::GetTempPath()) ("secure_pack_" + [Guid]::NewGuid().ToString("N") + ".zip")
    try {
        if (Test-Path -LiteralPath $zipPath) {
            Remove-Item -LiteralPath $zipPath -Force
        }
        Compress-Archive -Path (Join-Path $StagingDir "*") -DestinationPath $zipPath -Force

        [byte[]]$plain = [System.IO.File]::ReadAllBytes($zipPath)
        [byte[]]$nonce = New-Object byte[] 16
        $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
        try {
            $rng.GetBytes($nonce)
        }
        finally {
            $rng.Dispose()
        }
        [byte[]]$cipher = Protect-Bytes -Data $plain -KeyBytes $keyBytes -Nonce $nonce

        [byte[]]$all = New-Object byte[] ($magic.Length + $nonce.Length + $cipher.Length)
        [Array]::Copy($magic, 0, $all, 0, $magic.Length)
        [Array]::Copy($nonce, 0, $all, $magic.Length, $nonce.Length)
        [Array]::Copy($cipher, 0, $all, $magic.Length + $nonce.Length, $cipher.Length)

        $pakPath = Join-Path $packsDir $PakName
        [System.IO.File]::WriteAllBytes($pakPath, $all)
        $generatedPaks.Add($PakName) | Out-Null
    }
    finally {
        if (Test-Path -LiteralPath $zipPath) {
            Remove-Item -LiteralPath $zipPath -Force
        }
    }
}

if ($PackMode -eq "combined") {
    Update-InnerProgress -Status "Packing all sources into one pak" -Percent 55
    $staging = Join-Path ([System.IO.Path]::GetTempPath()) ("secure_pack_stage_all_" + [Guid]::NewGuid().ToString("N"))
    Ensure-Dir $staging
    $aliasUsed = @{}
    $aliasMap = [ordered]@{}
    try {
        foreach ($dir in $sourceDirs) {
            $audioFiles = Get-ChildItem -LiteralPath $dir.FullName -Recurse -File | Where-Object {
                $audioExt -contains $_.Extension.ToLower()
            }
            foreach ($file in $audioFiles) {
                $relative = $file.FullName.Substring($dir.FullName.Length).TrimStart('\','/')
                $relativeForAlias = (Join-Path $dir.Name $relative).Replace('\', '/')
                $alias = Get-SafeAliasFileName -RelativePath $relativeForAlias -Used $aliasUsed
                $target = Join-Path $staging $alias
                $targetDir = Split-Path -Parent $target
                Ensure-Dir $targetDir
                Copy-Item -LiteralPath $file.FullName -Destination $target -Force
                $aliasMap[$alias] = $file.Name
            }
        }
        $pakName = Get-SafePakName -SourceName "all_audio" -Used $usedPakNames
        Build-EncryptedPak -StagingDir $staging -PakName $pakName
        $packageAliasMaps[$pakName] = $aliasMap
        $packageKinds[$pakName] = "mixed"
        Write-Host "[OK] Combined pack -> all_audio.pak"
    }
    finally {
        if (Test-Path -LiteralPath $staging) {
            Remove-Item -LiteralPath $staging -Recurse -Force
        }
    }
} else {
    foreach ($dir in $sourceDirs) {
        $dirIndex = [Array]::IndexOf($sourceDirs, $dir) + 1
        $dirPercent = 10 + [int](($dirIndex / [Math]::Max($sourceDirs.Count, 1)) * 70)
        Update-InnerProgress -Status "Packing $($dir.Name)" -Percent $dirPercent

        $audioFiles = Get-ChildItem -LiteralPath $dir.FullName -Recurse -File | Where-Object {
            $audioExt -contains $_.Extension.ToLower()
        }
        if (-not $audioFiles) {
            Write-Host "[SKIP] $($dir.Name) (no audio files)"
            continue
        }

        $staging = Join-Path ([System.IO.Path]::GetTempPath()) ("secure_pack_stage_" + [Guid]::NewGuid().ToString("N"))
        Ensure-Dir $staging
        $aliasUsed = @{}
        $aliasMap = [ordered]@{}

        try {
            foreach ($file in $audioFiles) {
                $relative = $file.FullName.Substring($dir.FullName.Length).TrimStart('\','/')
                $alias = Get-SafeAliasFileName -RelativePath $relative -Used $aliasUsed
                $target = Join-Path $staging $alias
                $targetDir = Split-Path -Parent $target
                Ensure-Dir $targetDir
                Copy-Item -LiteralPath $file.FullName -Destination $target -Force
                $aliasMap[$alias] = $file.Name
            }
            $pakName = Get-SafePakName -SourceName $dir.Name -Used $usedPakNames
            Build-EncryptedPak -StagingDir $staging -PakName $pakName
            $packageAliasMaps[$pakName] = $aliasMap
            $packageKinds[$pakName] = Get-PackKind -SourceName $dir.Name
            Write-Host "[OK] $($dir.Name) -> $pakName"
        }
        finally {
            if (Test-Path -LiteralPath $staging) {
                Remove-Item -LiteralPath $staging -Recurse -Force
            }
        }
    }
}

if ($standalonePromptFiles.Count -gt 0) {
    Update-InnerProgress -Status "Packing standalone prompt files" -Percent 84
    $stagingPrompt = Join-Path ([System.IO.Path]::GetTempPath()) ("secure_pack_stage_prompt_" + [Guid]::NewGuid().ToString("N"))
    Ensure-Dir $stagingPrompt
    $aliasUsedPrompt = @{}
    $aliasMapPrompt = [ordered]@{}
    try {
        foreach ($file in $standalonePromptFiles) {
            $alias = Get-SafeAliasFileName -RelativePath $file.Name -Used $aliasUsedPrompt
            $target = Join-Path $stagingPrompt $alias
            Copy-Item -LiteralPath $file.FullName -Destination $target -Force
            $aliasMapPrompt[$alias] = $file.Name
        }
        $pakName = Get-SafePakName -SourceName "standalone_prompt" -Used $usedPakNames
        Build-EncryptedPak -StagingDir $stagingPrompt -PakName $pakName
        $packageAliasMaps[$pakName] = $aliasMapPrompt
        $packageKinds[$pakName] = "prompt"
        Write-Host "[OK] standalone prompt files -> $pakName"
    }
    finally {
        if (Test-Path -LiteralPath $stagingPrompt) {
            Remove-Item -LiteralPath $stagingPrompt -Recurse -Force
        }
    }
}

$srcLines = ""
$lineCandidates = @(
    (Join-Path $aa "00lines"),
    (Join-Path $root "00lines")
)
foreach ($cand in $lineCandidates) {
    if (-not (Test-Path -LiteralPath $cand)) { continue }
    $lineFiles = Get-ChildItem -LiteralPath $cand -File -ErrorAction SilentlyContinue | Where-Object {
        $_.Extension -ieq ".xlsx" -or $_.Extension -ieq ".txt"
    }
    if ($lineFiles -and $lineFiles.Count -gt 0) {
        $srcLines = $cand
        break
    }
}
Get-ChildItem -LiteralPath $linesDir -File -ErrorAction SilentlyContinue | ForEach-Object {
    Remove-Item -LiteralPath $_.FullName -Force
}
if ($srcLines -and (Test-Path -LiteralPath $srcLines)) {
    Get-ChildItem -LiteralPath $srcLines -File | Where-Object {
        $_.Extension -ieq ".xlsx" -or $_.Extension -ieq ".txt"
    } | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $linesDir $_.Name) -Force
    }
    Write-Host "[OK] Synced line files to lines/"
}
Update-InnerProgress -Status "Sync lines and configs" -Percent 90

$srcConfig = ""
$configCandidates = @(
    (Join-Path $aa "00config"),
    (Join-Path $root "00config")
)
foreach ($cand in $configCandidates) {
    if (-not (Test-Path -LiteralPath $cand)) { continue }
    $cfgFiles = Get-ChildItem -LiteralPath $cand -File -ErrorAction SilentlyContinue | Where-Object {
        $_.Extension -ieq ".json" -and $_.Name -notlike "pack_manifest.json"
    }
    if ($cfgFiles -and $cfgFiles.Count -gt 0) {
        $srcConfig = $cand
        break
    }
}
Get-ChildItem -LiteralPath $configDir -File -Filter "*.json" -ErrorAction SilentlyContinue | ForEach-Object {
    Remove-Item -LiteralPath $_.FullName -Force
}
if ($srcConfig -and (Test-Path -LiteralPath $srcConfig)) {
    Get-ChildItem -LiteralPath $srcConfig -File | Where-Object { $_.Extension -ieq ".json" } | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $configDir $_.Name) -Force
    }
    Write-Host "[OK] Synced template configs from 00config/"
} else {
    Write-Host "[WARN] 00config not found, creating fallback template config."
    $fallback = [ordered]@{
        name = "fallback_template"
        eng = $false
        resources = @{}
        sequences = [ordered]@{
            start_station = @('$CURRENT_STATION')
            enter_station = @('$CURRENT_STATION')
            next_station = @('$NEXT_STATION')
            terminal_station = @('$CURRENT_STATION')
        }
    }
    $fallback | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $configDir "fallback_template.json") -Encoding UTF8
}

$aliasesObj = [ordered]@{}
foreach ($pakName in ($packageAliasMaps.Keys | Sort-Object)) {
    $m = $packageAliasMaps[$pakName]
    $inner = [ordered]@{}
    foreach ($alias in ($m.Keys | Sort-Object)) {
        $inner[$alias] = [string]$m[$alias]
    }
    $aliasesObj[$pakName] = $inner
}

$kindsObj = [ordered]@{}
foreach ($pakName in ($packageKinds.Keys | Sort-Object)) {
    $kindsObj[$pakName] = [string]$packageKinds[$pakName]
}

$manifest = [ordered]@{
    key = $Key
    packages = $generatedPaks
    aliases = $aliasesObj
    kinds = $kindsObj
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $configDir "pack_manifest.json") -Encoding UTF8

if (Test-Path -LiteralPath $seedConfigZip) { Remove-Item -LiteralPath $seedConfigZip -Force }
if (Test-Path -LiteralPath $seedLinesZip) { Remove-Item -LiteralPath $seedLinesZip -Force }

$cfgSeedFiles = Get-ChildItem -LiteralPath $configDir -File -Filter "*.json" -ErrorAction SilentlyContinue
if ($cfgSeedFiles) {
    Compress-Archive -Path (Join-Path $configDir "*") -DestinationPath $seedConfigZip -Force
    Write-Host "[OK] Built seed config zip: $seedConfigZip"
}

$lineSeedFiles = Get-ChildItem -LiteralPath $linesDir -File -ErrorAction SilentlyContinue | Where-Object {
    $_.Extension -ieq ".xlsx" -or $_.Extension -ieq ".txt"
}
if ($lineSeedFiles) {
    Compress-Archive -Path (Join-Path $linesDir "*") -DestinationPath $seedLinesZip -Force
    Write-Host "[OK] Built seed lines zip: $seedLinesZip"
}

Write-EmbeddedPayloadQrc -QrcPath $embeddedQrc -PacksDir $packsDir -SeedConfigZip $seedConfigZip -SeedLinesZip $seedLinesZip
Copy-Item -LiteralPath $embeddedQrc -Destination (Join-Path $playerSrcDir "embedded_payload.qrc") -Force

Write-Host ""
Write-Host "[DONE] Output folders:"
Write-Host "       packs : $packsDir"
Write-Host "       lines : $linesDir"
Write-Host "       config: $configDir"
Write-Host "       seed config zip: $seedConfigZip"
Write-Host "       seed lines zip : $seedLinesZip"
Write-Host "       embedded qrc   : $embeddedQrc"
Write-Host "       src   : $playerSrcDir"

if ($ShowProgress) {
    Write-Progress -Id 2 -Activity "Pack encrypted audio" -Completed
}
