<#
.SYNOPSIS
  ToolCheck を M5Stack Tab5 にビルドして書き込み、起動したところまで確かめる。

.DESCRIPTION
  ビルド → 書き込み → 起動確認を 1 本にまとめたもの。

  compile と upload を別々に叩くと、compile が失敗していても upload まで進んでしまい、
  esptool の「ToolCheck.ino.partitions.bin が無い」という、原因とかけ離れたエラーだけが出る。
  このスクリプトは compile が失敗したらそこで止まり、原因の当たりを付けて表示する。

  ビルドの出力先は既定で <リポジトリ>\build に固定する (arduino-cli 既定のハッシュ付き
  一時フォルダに依存しない)。upload は --input-dir でそこを読む。

  書き込んだ後はシリアルで info を送り、チップの rev から ChipVariant が合っているかまで見る。

.PARAMETER Port
  Tab5 の COM ポート。省略すると自動で選ぶ (候補が複数あるときは止まる)。

.PARAMETER ChipVariant
  prev3 = ESP32-P4 rev 3.00 未満 (現状出回っている個体はほぼこちら) / postv3 = 3.00 以降。

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\flash.ps1
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\flash.ps1 -Port COM4
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\flash.ps1 -BuildOnly
#>
[CmdletBinding()]
param(
    [string]$Port = '',
    [ValidateSet('prev3', 'postv3')]
    [string]$ChipVariant = 'prev3',
    [string]$BuildPath = '',
    [switch]$BuildOnly,
    [switch]$SkipVerify,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding = [Text.Encoding]::UTF8
# PowerShell 7.4 以降は native コマンドの非 0 終了を $ErrorActionPreference='Stop' で例外にする。
# ここでは終了コードを自分で見て案内を出したいので無効にする (5.1 にはこの変数が無い)
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) { $PSNativeCommandUseErrorActionPreference = $false }

. "$PSScriptRoot\common.ps1"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Sketch = Join-Path $RepoRoot 'firmware\ToolCheck'
$Fqbn = "esp32:esp32:m5stack_tab5:ChipVariant=$ChipVariant"
if (-not $BuildPath) { $BuildPath = Join-Path $RepoRoot 'build' }

function Write-Step([string]$Text) {
    Write-Host ''
    Write-Host "=== $Text ==="
}

if (-not (Get-Command arduino-cli -ErrorAction SilentlyContinue)) {
    Write-Host 'arduino-cli が見つかりません。'
    Write-Host 'https://arduino.github.io/arduino-cli/latest/installation/ から入れてパスを通し、'
    Write-Host 'PowerShell を開き直してから tools\setup_env.ps1 を実行してください。'
    exit 1
}
if (-not (Test-Path -LiteralPath $Sketch)) {
    Write-Host "スケッチが見つかりません: $Sketch"
    Write-Host 'リポジトリの中から実行してください。'
    exit 1
}

# --- 1. ビルド ---------------------------------------------------------------

if ($Clean -and (Test-Path -LiteralPath $BuildPath)) {
    Write-Step 'ビルド結果を消します'
    Remove-Item -LiteralPath $BuildPath -Recurse -Force
}

Write-Step "ビルド (ChipVariant=$ChipVariant)"
Write-Host "  スケッチ: $Sketch"
Write-Host "  出力先  : $BuildPath"
Write-Host '  初回は数分かかります。'

arduino-cli compile --fqbn $Fqbn --build-path $BuildPath $Sketch
$compileExit = $LASTEXITCODE

if ($compileExit -ne 0) {
    Write-Host ''
    Write-Host '--- ビルドに失敗しました。書き込みには進みません ---'
    Write-Host '上のエラーの 1 行目を読んでください。よくある原因:'
    Write-Host ''
    Write-Host '  ESP_Video.h / M5Unified.h / M5GFX.h / VL53L1X.h が無いと言われる'
    Write-Host '    → tools\setup_env.ps1 をまだ実行していないか、途中で失敗しています。'
    Write-Host '      Arduino IDE を閉じてから実行し直してください。'
    Write-Host ''
    Write-Host '  esp32:esp32:m5stack_tab5 というボードが無いと言われる'
    Write-Host '    → esp32 のボードパッケージが入っていません。tools\setup_env.ps1 を実行してください。'
    exit 1
}

$appBin = Join-Path $BuildPath 'ToolCheck.ino.bin'
$partBin = Join-Path $BuildPath 'ToolCheck.ino.partitions.bin'
foreach ($f in @($appBin, $partBin)) {
    if (-not (Test-Path -LiteralPath $f)) {
        Write-Host ''
        Write-Host "ビルドは成功したように見えますが $([IO.Path]::GetFileName($f)) が出ていません。"
        Write-Host "-Clean を付けて実行し直してください。"
        exit 1
    }
}
Write-Host ''
Write-Host "ビルド成功: $([IO.Path]::GetFileName($appBin)) $((Get-Item -LiteralPath $appBin).Length) バイト"

if ($BuildOnly) {
    Write-Host '-BuildOnly が指定されたのでここで終わります。'
    exit 0
}

# --- 2. 書き込み -------------------------------------------------------------

try {
    $Port = Resolve-SerialPort -Requested $Port
}
catch {
    Write-Host ''
    Write-Host $_.Exception.Message
    exit 1
}

Write-Step "書き込み ($Port)"
$uploadLog = @()
arduino-cli upload --fqbn $Fqbn -p $Port --input-dir $BuildPath $Sketch 2>&1 |
    ForEach-Object {
        $line = [string]$_
        $uploadLog += $line
        Write-Host $line
    }
$uploadExit = $LASTEXITCODE

if ($uploadExit -ne 0) {
    Write-Host ''
    Write-Host '--- 書き込みに失敗しました ---'
    Write-Host "  ・$Port を他のアプリ (Arduino IDE のシリアルモニタ、tools\serial.ps1) が掴んでいないか確認してください"
    Write-Host '  ・USB-C ケーブルを挿し直して、すぐにもう一度実行すると通ることがあります'
    Write-Host '  ・ケーブルが充電専用でないか (データ通信対応か) を確認してください'
    exit 1
}

# esptool が書き込みの頭で報告するチップの rev を拾っておく (例: Chip is ESP32-P4 (revision v1.3))
$revFromEsptool = ''
foreach ($line in $uploadLog) {
    if ($line -match 'ESP32-P4.*revision\s+v?([0-9]+)\.([0-9]+)') {
        $revFromEsptool = "$($Matches[1]).$($Matches[2])"
        break
    }
}
if ($revFromEsptool) { Write-Host "esptool が報告したチップ rev: v$revFromEsptool" }

if ($SkipVerify) {
    Write-Host ''
    Write-Host "書き込みました。-SkipVerify が指定されたので起動確認はしません。"
    exit 0
}

# --- 3. 起動確認 -------------------------------------------------------------

Write-Step '起動確認 (シリアルで info を送ります)'
Start-Sleep -Seconds 2

$serial = Join-Path $PSScriptRoot 'serial.ps1'
$out = @()
try {
    $out = @(& $serial -Port $Port -Send 'info' -Seconds 10 2>&1 | ForEach-Object { [string]$_ })
}
catch {
    # serial.ps1 はポートを開けないと終了する。ここでは失敗として扱い、下の案内に落とす
    Write-Host $_.Exception.Message
}
foreach ($line in $out) { Write-Host $line }

$info = @($out | Where-Object { $_ -match '^#INFO ' })
if ($info.Count -eq 0) {
    Write-Host ''
    Write-Host '--- 書き込みは終わりましたが、装置から応答がありません ---'
    $other = if ($ChipVariant -eq 'prev3') { 'postv3' } else { 'prev3' }
    Write-Host "  ・画面が真っ黒 / 文字化けしているなら ChipVariant が個体と合っていません。"
    Write-Host "    次を試してください: tools\flash.ps1 -Port $Port -ChipVariant $other"
    Write-Host '  ・電源スイッチ (背面) が ON か確認してください'
    Write-Host "  ・$Port を他のアプリが掴んでいないか確認してください"
    exit 1
}

$fw = ''
$rev = -1
if ($info[0] -match 'fw=([^\s]+)') { $fw = $Matches[1] }
if ($info[0] -match 'rev=([0-9]+)') { $rev = [int]$Matches[1] }

Write-Host ''
Write-Host '--- 起動しました ---'
Write-Host "  版数      : $fw"
if ($rev -ge 0) {
    $expected = Get-ExpectedChipVariant -Revision $rev
    $revText = '{0}.{1:d2}' -f [int]($rev / 100), ($rev % 100)
    Write-Host "  チップ rev: $rev (v$revText)"
    Write-Host "  ChipVariant: $ChipVariant"
    if ($expected -ne $ChipVariant) {
        Write-Host ''
        Write-Host "この個体の rev には $expected が合っています。焼き直してください:"
        Write-Host "  tools\flash.ps1 -Port $Port -ChipVariant $expected"
        exit 1
    }
}
Write-Host ''
Write-Host '書き込みと起動確認が終わりました。'
Write-Host "画面の使い方は README の「使い方の要点」を見てください。"
Write-Host "シリアルを開くには: powershell -NoProfile -ExecutionPolicy Bypass -File tools\serial.ps1 -Port $Port"
exit 0
