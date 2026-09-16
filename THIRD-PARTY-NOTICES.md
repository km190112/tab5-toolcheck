# Third-party notices

このリポジトリには依存ライブラリのソースコードを含めていない。ビルドすると以下がファームウェアに組み込まれる。
ライセンスの原文は [`licenses/`](licenses/) にあり、本体の設定画面「このソフトについて」→「ライセンス全文」でも全文を表示する (`tools/gen_license_text.ps1` で `firmware/ToolCheck/src/core/license_text.cpp` に生成)。
**ビルドしたファームウェアを配布する場合は、各ライセンスの表示義務 (特に BSD 3-Clause の第 2 条) を満たすこと。**

| 名前 | 版 | ライセンス | 使い方 |
|---|---|---|---|
| [M5Unified](https://github.com/m5stack/M5Unified) | 0.2.21 | MIT (Copyright (c) 2021 M5Stack) | 本体ファーム |
| [M5GFX](https://github.com/m5stack/M5GFX) | 0.2.28 | MIT (Copyright (c) 2021 M5Stack) | 本体ファーム (画面) |
| M5GFX 同梱の日本語フォント (`lgfxJapanGothicP_*`、IPAex / IPA フォントの変換) | — | IPA Font License Agreement v1.0 | 本体ファーム (画面の文字) |
| [Pololu VL53L1X](https://github.com/pololu/vl53l1x-arduino) | 1.3.1 | BSD 3-Clause (Copyright (c) 2017, STMicroelectronics / Copyright (c) 2018-2022, Pololu Corporation) | 本体ファーム (Unit ToF4M) |
| [Arduino core for ESP32](https://github.com/espressif/arduino-esp32) | 3.3.11 | LGPL-2.1-or-later (同梱ライブラリの `SD` `Preferences` などは Apache-2.0、`ESP_Video` は ESPRESSIF MIT) | 本体ファーム |
| [M5UnitQRCode](https://github.com/m5stack/M5Unit-QRCode) | 1.0.0 | MIT (Copyright (c) 2023 M5Stack) | `spikes/` の確認用スケッチだけ |
| [qrcode-generator](https://github.com/kazuhikoarase/qrcode-generator) | 1.4.4 | MIT (Kazuhiko Arase) | `tools/test_qr.html` が CDN から読み込む |

## Pololu VL53L1X (BSD 3-Clause)

```
Copyright (c) 2017, STMicroelectronics
Copyright (c) 2018-2022, Pololu Corporation
All Rights Reserved

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software
without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## M5Unified / M5GFX / M5UnitQRCode (MIT)

全文は [`licenses/M5Unified-LICENSE.txt`](licenses/M5Unified-LICENSE.txt) と [`licenses/M5GFX-LICENSE.txt`](licenses/M5GFX-LICENSE.txt)。M5UnitQRCode は同じ MIT (Copyright (c) 2023 M5Stack) で、リポジトリの `LICENSE` を参照。

## IPA フォント

全文は [`licenses/IPA_Font_License_Agreement_v1.0.txt`](licenses/IPA_Font_License_Agreement_v1.0.txt) (M5GFX の `src/lgfx/Fonts/IPA/` にあるものの写し)。
