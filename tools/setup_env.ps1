<#
.SYNOPSIS
  ToolCheck をビルドするための arduino-cli 環境 (ボードパッケージ・ライブラリ) を、README と同じ版数で揃える。

.DESCRIPTION
  esp32 コア 3.3.11 と、M5Unified / M5GFX / VL53L1X を指定版で入れる。

  ライブラリの入り先は arduino-cli と Arduino IDE で違うことがある。
  IDE は %USERPROFILE%\.arduinoIDE\arduino-cli.yaml の directories.user を見るが、
  素の arduino-cli は %USERPROFILE%\Documents\Arduino を見る。OneDrive が Documents を
  バックアップしている Windows では、この 2 つが別の場所になる。
  そのため、両方が使う場所すべてにライブラリを入れる。

  実行後に、ビルドに要るものが本当に揃ったかを検査する。1 つでも欠けていれば終了コード 1 で終わる。

  Arduino IDE を開いたままこのスクリプトを実行した場合は、実行後に IDE を再起動してから使うこと。

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\setup_env.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding = [Text.Encoding]::UTF8
# PowerShell 7.4 以降は native コマンドの非 0 終了を $ErrorActionPreference='Stop' で例外にする。
# ここでは終了コードを自分で見て案内を出したいので無効にする (5.1 にはこの変数が無い)
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) { $PSNativeCommandUseErrorActionPreference = $false }

$Esp32CoreId = 'esp32:esp32'
$Esp32CoreVersion = '3.3.11'
$BoardManagerUrl = 'https://espressif.github.io/arduino-esp32/package_esp32_index.json'
$Libraries = @('M5Unified@0.2.21', 'M5GFX@0.2.28', 'VL53L1X@1.3.1')
$IdeConfig = Join-Path $env:USERPROFILE '.arduinoIDE\arduino-cli.yaml'

if (-not (Get-Command arduino-cli -ErrorAction SilentlyContinue)) {
    Write-Error 'arduino-cli が見つかりません。https://arduino.github.io/arduino-cli/latest/installation/ から入れてパスを通してから実行し直す'
    exit 1
}

function Invoke-ArduinoCli {
    # $ConfigFile が空なら arduino-cli 既定の設定で、指定されていればその設定ファイルで叩く
    param([string]$ConfigFile, [string[]]$CliArgs)
    if ($ConfigFile) { $CliArgs = @($CliArgs) + @('--config-file', $ConfigFile) }
    & arduino-cli @CliArgs
}

function Get-SketchbookDir {
    param([string]$ConfigFile)
    $out = Invoke-ArduinoCli -ConfigFile $ConfigFile -CliArgs @('config', 'get', 'directories.user') 2>$null
    if ($LASTEXITCODE -ne 0) { return '' }
    return ([string]$out).Trim()
}

Write-Output '--- 設定ファイルの確認 ---'
arduino-cli config dump *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Output '設定ファイルが無いので作成する'
    arduino-cli config init
    if ($LASTEXITCODE -ne 0) { throw 'arduino-cli config init に失敗しました' }
}

Write-Output '--- ボードマネージャ URL の追加 (既に入っていれば何もしない) ---'
arduino-cli config add board_manager.additional_urls $BoardManagerUrl
if ($LASTEXITCODE -ne 0) { Write-Warning 'ボードマネージャ URL の追加でエラーが出たが、既に入っている場合はここで止まらなくてよい' }

Write-Output '--- ボードインデックスの更新 ---'
arduino-cli core update-index
if ($LASTEXITCODE -ne 0) { throw 'arduino-cli core update-index に失敗しました (ネットワークを確認する)' }

Write-Output '--- 入っている esp32 コアのバージョン確認 ---'
$coreListJson = arduino-cli core list --format json
if ($LASTEXITCODE -ne 0) { throw 'arduino-cli core list に失敗しました' }
# arduino-cli の版によって { "platforms": [...] } と、配列そのものの2通りの形がある。両方に対応する
$coreListRaw = @($coreListJson | ConvertFrom-Json)
$installedCores = @(
    if ($coreListRaw.Count -eq 1 -and $coreListRaw[0].PSObject.Properties.Name -contains 'platforms') {
        $coreListRaw[0].platforms
    } else {
        $coreListRaw
    }
)
$existingEsp32 = @($installedCores | Where-Object { $_.id -eq $Esp32CoreId })

if ($existingEsp32.Count -gt 0 -and $existingEsp32[0].installed_version -ne $Esp32CoreVersion) {
    Write-Output "esp32 コア $($existingEsp32[0].installed_version) が入っているので、$Esp32CoreVersion に入れ替える"
    arduino-cli core uninstall $Esp32CoreId
    if ($LASTEXITCODE -ne 0) { throw '既存の esp32 コアのアンインストールに失敗しました' }
}

Write-Output "--- esp32 コア $Esp32CoreVersion のインストール ---"
arduino-cli core install "${Esp32CoreId}@${Esp32CoreVersion}"
if ($LASTEXITCODE -ne 0) { throw "esp32 コア $Esp32CoreVersion のインストールに失敗しました" }

# --- ライブラリ: arduino-cli と Arduino IDE が見る場所すべてに入れる -----------

$targets = @()
$cliSketchbook = Get-SketchbookDir -ConfigFile ''
$targets += [pscustomobject]@{ Name = 'arduino-cli'; ConfigFile = ''; Sketchbook = $cliSketchbook }

if (Test-Path -LiteralPath $IdeConfig) {
    $ideSketchbook = Get-SketchbookDir -ConfigFile $IdeConfig
    if ($ideSketchbook -and ($ideSketchbook -ne $cliSketchbook)) {
        Write-Output ''
        Write-Output 'Arduino IDE のライブラリの置き場所が arduino-cli と違います。両方に入れます:'
        Write-Output "  arduino-cli : $cliSketchbook"
        Write-Output "  Arduino IDE : $ideSketchbook"
        $targets += [pscustomobject]@{ Name = 'Arduino IDE'; ConfigFile = $IdeConfig; Sketchbook = $ideSketchbook }
    }
}

foreach ($t in $targets) {
    Write-Output ''
    Write-Output "--- ライブラリのインストール ($($t.Name): $($t.Sketchbook)) ---"
    Invoke-ArduinoCli -ConfigFile $t.ConfigFile -CliArgs (@('lib', 'install') + $Libraries)
    if ($LASTEXITCODE -ne 0) { throw "ライブラリのインストールに失敗しました ($($t.Name))" }
}

# --- 検査: ビルドに要るものが本当に揃ったか -----------------------------------

Write-Output ''
Write-Output '--- 検査 ---'
$problems = @()

# 1. esp32 コアが 3.3.11 単独で入っているか
$dataDir = (arduino-cli config get directories.data 2>$null)
$dataDir = ([string]$dataDir).Trim()
$coreRoot = Join-Path $dataDir 'packages\esp32\hardware\esp32'
$coreVersions = @()
if (Test-Path -LiteralPath $coreRoot) {
    $coreVersions = @(Get-ChildItem -LiteralPath $coreRoot -Directory | Select-Object -ExpandProperty Name)
}
if ($coreVersions -notcontains $Esp32CoreVersion) {
    $problems += "esp32 コア $Esp32CoreVersion が入っていません (見つかったもの: $($coreVersions -join ', '))"
} else {
    Write-Output "  [OK] esp32 コア $Esp32CoreVersion"
}
if ($coreVersions.Count -gt 1) {
    $problems += "esp32 コアが複数の版で入っています ($($coreVersions -join ', '))。ボードマネージャで $Esp32CoreVersion 以外を削除してください"
}

# 2. カメラで使う ESP_Video がコアに同梱されているか (Library Manager では入らない)
$espVideo = Join-Path $coreRoot "$Esp32CoreVersion\libraries\ESP_Video\src\ESP_Video.h"
if (Test-Path -LiteralPath $espVideo) {
    Write-Output '  [OK] ESP_Video (esp32 コア同梱)'
} else {
    $problems += "ESP_Video.h が見つかりません ($espVideo)。esp32 コアを入れ直してください"
}

# 3. ライブラリが各スケッチブックに指定版で入っているか
foreach ($t in $targets) {
    foreach ($spec in $Libraries) {
        $name, $want = $spec -split '@', 2
        $propsPath = Join-Path $t.Sketchbook "libraries\$name\library.properties"
        if (-not (Test-Path -LiteralPath $propsPath)) {
            $problems += "$name が $($t.Name) の場所に入っていません ($propsPath)"
            continue
        }
        $versionLine = @(Get-Content -LiteralPath $propsPath -Encoding UTF8 | Where-Object { $_ -match '^version=' })
        $got = if ($versionLine.Count -gt 0) { ($versionLine[0] -replace '^version=', '').Trim() } else { '' }
        if ($got -ne $want) {
            $problems += "$name の版数が違います ($($t.Name): $got / 欲しいのは $want)"
        } else {
            Write-Output "  [OK] $name $got ($($t.Name))"
        }
    }
}

# 4. M5Stack のボードパッケージが入っていると、IDE のボード一覧に M5Tab5 が 2 つ出る
$m5Core = @($installedCores | Where-Object { $_.id -eq 'm5stack:esp32' })
if ($m5Core.Count -gt 0) {
    Write-Output ''
    Write-Warning 'M5Stack のボードパッケージ (m5stack:esp32) も入っています。'
    Write-Warning 'Arduino IDE の「ツール」→「ボード」に M5Tab5 が 2 つ出ます。'
    Write-Warning '必ず esp32 (by Espressif Systems) の方の M5Tab5 を選んでください。'
    Write-Warning 'M5Stack 版を選ぶと ESP_Video.h が無いというエラーでコンパイルに失敗します。'
}

Write-Output ''
if ($problems.Count -gt 0) {
    Write-Output '検査で問題が見つかりました:'
    foreach ($p in $problems) { Write-Output "  [NG] $p" }
    Write-Output ''
    Write-Output 'Arduino IDE を閉じてから、このスクリプトを実行し直してください。'
    exit 1
}

Write-Output '検査は全て OK。導入した版数:'
Write-Output "  esp32 コア: $Esp32CoreVersion"
foreach ($lib in $Libraries) { Write-Output "  $lib" }
Write-Output ''
Write-Output '次はこれを実行すれば、ビルドから書き込みまで通ります:'
Write-Output '  powershell -NoProfile -ExecutionPolicy Bypass -File tools\flash.ps1'
Write-Output ''
Write-Output 'Arduino IDE を使う場合、開いたままこのスクリプトを実行したなら一度閉じて開き直してから、'
Write-Output '「ツール」→「ボード」→ esp32 (by Espressif Systems) → M5Tab5 を選ぶ。'
exit 0
