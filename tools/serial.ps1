<#
.SYNOPSIS
  Tab5 (ESP32-P4) の USB シリアルを、DTR/RTS を落としてから開いて読み書きする。

.DESCRIPTION
  DTR=true で開くシリアルモニタだと、ESP32-P4 のネイティブ USB
  (USB-Serial/JTAG) ではチップをダウンロードモードに落とす恐れがある。
  ここでは Open() の前に DtrEnable / RtsEnable を false にしてから開く。
  装置が再起動してポートが一度消えても、指定時間内なら開き直して読み続ける。

  -Send は '|' 区切りで複数のコマンドを送る (コマンド自体にカンマを含められるように)。
  -Screenshot は "screenshot" を送り、#BEGIN png ～ #END png の base64 を PNG に保存する。

.EXAMPLE
  pwsh -NoProfile -File tools/serial.ps1 -Seconds 5
  pwsh -NoProfile -File tools/serial.ps1 -Send 'info|scan' -Seconds 3
  pwsh -NoProfile -File tools/serial.ps1 -Screenshot tools/out/screen.png
#>
[CmdletBinding()]
param(
    [string]$Port = 'COM3',
    [int]$Baud = 115200,
    [double]$Seconds = 5,
    [string]$Send = '',
    [string]$Screenshot = '',
    [int]$ScreenshotTimeoutSec = 60,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

function Open-SerialPort {
    $sp = [System.IO.Ports.SerialPort]::new($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    # Open() の前に落とす。開いた後に落としても、開いた瞬間のアサートでチップが落ちる
    $sp.DtrEnable = $false
    $sp.RtsEnable = $false
    $sp.Handshake = [System.IO.Ports.Handshake]::None
    $sp.ReadTimeout = 100
    $sp.WriteTimeout = 2000
    $sp.ReadBufferSize = 1MB
    $sp.Open()
    return $sp
}

function Open-Until([datetime]$Deadline) {
    while ((Get-Date) -lt $Deadline) {
        try { return Open-SerialPort }
        catch { Start-Sleep -Milliseconds 300 }
    }
    return $null
}

function Send-Line($Sp, [string]$Line) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Line + "`n")
    $Sp.Write($bytes, 0, $bytes.Length)
}

$waitSec = if ($Screenshot) { $ScreenshotTimeoutSec } else { [Math]::Max($Seconds, 1) }
$deadline = (Get-Date).AddSeconds($waitSec)

$sp = Open-Until $deadline
if (-not $sp) {
    Write-Error "$Port を開けませんでした。USB が繋がっているか、他のアプリ (Arduino IDE のシリアルモニタ等) が掴んでいないか確認してください"
    exit 1
}

$decoder = [Text.Encoding]::UTF8.GetDecoder()
$buf = [byte[]]::new(65536)
$chars = [char[]]::new(65536)
$pending = [Text.StringBuilder]::new()
$pngB64 = [Text.StringBuilder]::new()
$inPng = $false
$pngDone = $false
$pngExpected = -1

Start-Sleep -Milliseconds 200
if ($Send) {
    foreach ($cmd in ($Send -split '\|')) {
        if ($cmd.Trim()) {
            Send-Line $sp $cmd.Trim()
            Start-Sleep -Milliseconds 50
        }
    }
}
if ($Screenshot) { Send-Line $sp 'screenshot' }

while ((Get-Date) -lt $deadline -and -not $pngDone) {
    try {
        $n = $sp.BytesToRead
        if ($n -le 0) {
            Start-Sleep -Milliseconds 20
            continue
        }
        $read = $sp.Read($buf, 0, [Math]::Min($n, $buf.Length))
        $cn = $decoder.GetChars($buf, 0, $read, $chars, 0)
        [void]$pending.Append($chars, 0, $cn)

        $text = $pending.ToString()
        $lastNl = $text.LastIndexOf("`n")
        if ($lastNl -lt 0) { continue }
        [void]$pending.Clear()
        [void]$pending.Append($text.Substring($lastNl + 1))

        foreach ($raw in ($text.Substring(0, $lastNl) -split "`n")) {
            $line = $raw.TrimEnd("`r")
            if ($Screenshot) {
                if ($line.StartsWith('#BEGIN png')) {
                    $inPng = $true
                    [void]$pngB64.Clear()
                    $sizeText = $line.Substring('#BEGIN png'.Length).Trim()
                    if ($sizeText -match '^\d+$') { $pngExpected = [int]$sizeText }
                    if (-not $Quiet) { Write-Output $line }
                    continue
                }
                if ($inPng) {
                    if ($line.StartsWith('#END png')) {
                        $inPng = $false
                        $pngDone = $true
                        if (-not $Quiet) { Write-Output $line }
                        break
                    }
                    [void]$pngB64.Append($line)
                    continue
                }
            }
            if (-not $Quiet) { Write-Output $line }
        }
    }
    catch [System.IO.IOException], [System.UnauthorizedAccessException], [System.InvalidOperationException] {
        # 装置の再起動で USB が一度切れる。開き直して読み続ける
        Write-Output "[serial.ps1] 切断を検知しました。開き直します"
        try { $sp.Dispose() } catch { }
        $sp = Open-Until $deadline
        if (-not $sp) { break }
    }
}

if ($pending.Length -gt 0 -and -not $Quiet) { Write-Output $pending.ToString() }
if ($sp) {
    try { $sp.Close(); $sp.Dispose() } catch { }
}

if ($Screenshot) {
    if (-not $pngDone) {
        Write-Error "スクリーンショットを受け取れませんでした (#END png が来ませんでした)"
        exit 1
    }
    $full = if ([IO.Path]::IsPathRooted($Screenshot)) { $Screenshot } else { Join-Path (Get-Location).Path $Screenshot }
    $dir = Split-Path -Parent $full
    if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    # USB シリアルで行が丸ごと落ちても base64 としては読めてしまい、壊れた PNG を保存していた (2026-09-14)。
    # 装置が #BEGIN png で知らせた大きさと合わなければ保存しない
    try {
        $bytes = [Convert]::FromBase64String($pngB64.ToString())
    }
    catch {
        Write-Error "スクリーンショットの base64 を読めませんでした (USB シリアルで行が落ちた・混ざった)。取り直してください"
        exit 1
    }
    if ($pngExpected -ge 0 -and $bytes.Length -ne $pngExpected) {
        Write-Error "スクリーンショットの大きさが合いません (受け取り $($bytes.Length) / 装置 $pngExpected バイト。USB シリアルで行が落ちた)。取り直してください"
        exit 1
    }
    [IO.File]::WriteAllBytes($full, $bytes)
    Write-Output "[serial.ps1] 保存しました: $full ($($bytes.Length) バイト)"
}
exit 0
