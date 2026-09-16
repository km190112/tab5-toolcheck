<#
.SYNOPSIS
  LICENSE と licenses/ の原文から firmware/ToolCheck/src/core/license_text.cpp を生成する。

.DESCRIPTION
  本体の設定画面「このソフトについて」→「ライセンス全文」に出す文。元のファイルを直したら、これを実行して生成し直す。
  元のファイルと一致することは tests/test_license_text.cpp が確かめる (生成し忘れるとテストが落ちる)。
  BOM を落とし、改行を LF にそろえ、1 行ずつ C++ の文字列リテラルにして連結する
  (MSVC の 1 リテラルの上限 約 16KB と、連結後の上限 65,535 バイトの内に収まる)。

.EXAMPLE
  pwsh -NoProfile -File tools/gen_license_text.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$root = Split-Path -Parent $PSScriptRoot
$outPath = Join-Path $root 'firmware\ToolCheck\src\core\license_text.cpp'

# 画面の節の順。source はリポジトリの根からの相対パス (区切りは /)
$sections = @(
    @{ Title = 'ToolCheck (MIT)'; Source = 'LICENSE' },
    @{ Title = 'M5Unified (MIT)'; Source = 'licenses/M5Unified-LICENSE.txt' },
    @{ Title = 'M5GFX (MIT)'; Source = 'licenses/M5GFX-LICENSE.txt' },
    @{ Title = 'Pololu VL53L1X (BSD 3-Clause)'; Source = 'licenses/VL53L1X-LICENSE.txt' },
    @{ Title = 'IPA フォント (IPA Font License v1.0)'; Source = 'licenses/IPA_Font_License_Agreement_v1.0.txt' }
)

# 全文を載せないもの
$notes = @(
    'Arduino core for ESP32 3.3.11: LGPL-2.1-or-later',
    'SD・Preferences などは Apache-2.0',
    'ESP_Video は ESPRESSIF MIT',
    'https://github.com/espressif/arduino-esp32'
)

$maxLiteralBytes = 16000
$utf8 = [Text.UTF8Encoding]::new($false)

function ConvertTo-CppString([string]$s) {
    $b = [Text.StringBuilder]::new()
    foreach ($ch in $s.ToCharArray()) {
        switch ($ch) {
            '\' { [void]$b.Append('\\') }
            '"' { [void]$b.Append('\"') }
            "`t" { [void]$b.Append('\t') }
            default { [void]$b.Append($ch) }
        }
    }
    return $b.ToString()
}

$sb = [Text.StringBuilder]::new()
[void]$sb.Append("// tools/gen_license_text.ps1 が生成した。手で直さない (元は LICENSE と licenses/)`n")
[void]$sb.Append("#include `"license_text.h`"`n`nnamespace toolcheck {`n`nnamespace {`n")

$index = 0
foreach ($sec in $sections) {
    $path = Join-Path $root ($sec.Source -replace '/', '\')
    if (-not (Test-Path -LiteralPath $path)) { throw "元のファイルがありません: $($sec.Source)" }
    $text = [IO.File]::ReadAllText($path, $utf8)
    if ($text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }
    $text = $text -replace "`r`n", "`n"
    if ($utf8.GetByteCount($text) -gt 60000) { throw "長すぎます (連結後の上限に近い): $($sec.Source)" }

    [void]$sb.Append("`n// $($sec.Source)`nconst char kText$index[] =`n")
    $lines = $text -split "`n", 0
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $isLast = ($i -eq $lines.Count - 1)
        if ($isLast -and $lines[$i] -eq '') { break }  # 最後の改行の後ろ
        $piece = $lines[$i] + $(if ($isLast) { '' } else { "`n" })
        if ($utf8.GetByteCount($piece) -gt $maxLiteralBytes) { throw "1 行が長すぎます: $($sec.Source) の $($i + 1) 行目" }
        $lit = (ConvertTo-CppString $piece) -replace "`n", '\n'
        [void]$sb.Append("    `"$lit`"`n")
    }
    if ($text.Length -eq 0) { [void]$sb.Append("    `"`"`n") }
    [void]$sb.Append("    ;`n")
    $index++
}

[void]$sb.Append("`n}  // namespace`n`nconst LicenseSection kLicenseSections[] = {`n")
$index = 0
foreach ($sec in $sections) {
    [void]$sb.Append("    {`"$(ConvertTo-CppString $sec.Title)`", `"$($sec.Source)`", kText$index},`n")
    $index++
}
[void]$sb.Append("};`nconst size_t kLicenseSectionCount = sizeof(kLicenseSections) / sizeof(kLicenseSections[0]);`n`n")
[void]$sb.Append("const char* const kLicenseNoteLines[] = {`n")
foreach ($n in $notes) { [void]$sb.Append("    `"$(ConvertTo-CppString $n)`",`n") }
[void]$sb.Append("};`nconst size_t kLicenseNoteLineCount = sizeof(kLicenseNoteLines) / sizeof(kLicenseNoteLines[0]);`n`n}  // namespace toolcheck`n")

[IO.File]::WriteAllText($outPath, $sb.ToString(), $utf8)
$bytes = (Get-Item -LiteralPath $outPath).Length
Write-Output "生成しました: firmware/ToolCheck/src/core/license_text.cpp ($bytes バイト、$($sections.Count) 節)"
