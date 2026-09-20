<#
.SYNOPSIS
  tools\*.ps1 が共通で使う処理。ドットソースで読み込む (`. "$PSScriptRoot\common.ps1"`)。

.DESCRIPTION
  日本語を含むので UTF-8 BOM 付き + CRLF で保存する。
  BOM 無しだと Windows PowerShell 5.1 が CP932 として読み、文字列が壊れる。
#>

# 繋がっている COM ポートのうち、Espressif の USB (VID 0x303A) を持つものを優先して返す。
# Tab5 (ESP32-P4) は USB-Serial/JTAG が内蔵なので、この VID で絞れる。
function Get-SerialPortCandidate {
    $espressif = @()
    try {
        $espressif = @(
            Get-CimInstance -ClassName Win32_PnPEntity -ErrorAction Stop |
                Where-Object { $_.Name -match '\(COM\d+\)' -and (($_.HardwareID -join ';') -match 'VID_303A') } |
                ForEach-Object { if ($_.Name -match '\((COM\d+)\)') { $Matches[1] } }
        )
    }
    catch {
        # WMI/CIM が使えない環境では黙って「全ポート」に落とす
        $espressif = @()
    }
    if ($espressif.Count -gt 0) { return , $espressif }
    return , @([System.IO.Ports.SerialPort]::GetPortNames())
}

# -Port が指定されていればそれを使う。省略されたら自動で選ぶ。
# 候補が 0 個または 2 個以上なら、黙って間違ったポートを掴まずに止める。
function Resolve-SerialPort {
    param([string]$Requested)

    if ($Requested) { return $Requested }

    $candidates = @(Get-SerialPortCandidate)

    if ($candidates.Count -eq 1) {
        Write-Host "[tools] ポートを自動で選びました: $($candidates[0])"
        return $candidates[0]
    }
    if ($candidates.Count -eq 0) {
        throw 'COM ポートが1つも見つかりません。USB-C ケーブルがデータ通信対応か (充電専用ではないか)、Tab5 の電源が入っているかを確認してください'
    }
    throw "COM ポートが複数あります ($($candidates -join ', '))。どれが Tab5 か分からないので -Port COM4 のように指定してください"
}

# ESP.getChipRevision() の値 (103 = v1.03) から、使うべき ChipVariant を返す。
function Get-ExpectedChipVariant {
    param([int]$Revision)
    if ($Revision -ge 300) { return 'postv3' }
    return 'prev3'
}
