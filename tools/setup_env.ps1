<#
.SYNOPSIS
  ToolCheck をビルドするための arduino-cli 環境 (ボードパッケージ・ライブラリ) を、README と同じ版数で揃える。

.DESCRIPTION
  README の「ビルドと書き込み」に書いてある arduino-cli のコマンド列 (config init・ボードマネージャ URL 追加・
  esp32 コア 3.3.11 のインストール・M5Unified/M5GFX/VL53L1X の指定版インストール) を1本にまとめたもの。
  これを実行しておけば arduino-cli でも Arduino IDE でも同じ版数が使える
  (arduino-cli と Arduino IDE 2.x は既定で同じインストール先 (Arduino15 フォルダ) を共有するため)。

  esp32 コアが 3.3.11 以外のバージョンで入っている場合は、ビルド時にどちらが使われるか曖昧にならないよう
  一度アンインストールしてから 3.3.11 を入れ直す。

  Arduino IDE を開いたままこのスクリプトを実行した場合は、実行後に IDE を再起動してから使うこと。

.EXAMPLE
  pwsh -NoProfile -File tools/setup_env.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$Esp32CoreId = 'esp32:esp32'
$Esp32CoreVersion = '3.3.11'
$BoardManagerUrl = 'https://espressif.github.io/arduino-esp32/package_esp32_index.json'
$Libraries = @('M5Unified@0.2.21', 'M5GFX@0.2.28', 'VL53L1X@1.3.1')

if (-not (Get-Command arduino-cli -ErrorAction SilentlyContinue)) {
    Write-Error 'arduino-cli が見つかりません。https://arduino.github.io/arduino-cli/latest/installation/ から入れてパスを通してから実行し直す'
    exit 1
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

Write-Output '--- ライブラリのインストール ---'
arduino-cli lib install $Libraries
if ($LASTEXITCODE -ne 0) { throw 'ライブラリのインストールに失敗しました' }

Write-Output ''
Write-Output '完了。導入した版数:'
Write-Output "  esp32 コア: $Esp32CoreVersion"
foreach ($lib in $Libraries) { Write-Output "  $lib" }
Write-Output ''
Write-Output 'このまま arduino-cli compile / arduino-cli upload を使えます。'
Write-Output 'Arduino IDE を使う場合、開いたままこのスクリプトを実行したなら一度閉じて開き直してから Tools メニューでボードを選ぶ。'
