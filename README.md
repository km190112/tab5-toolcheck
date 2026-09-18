# ToolCheck — M5Stack Tab5 の物品持ち出し管理

M5Stack Tab5 を工具箱の横に 1 台置き、治具・工具・計測器の持出と返却を記録する単体機器 (PoC)。
PC やサーバ、Wi-Fi は使わない。

- **QR コードで登録する。** 物品の QR を読み、最後に名札の QR を読むと持出が確定する。貸出中の物品を読むとその場で返却になる
- **貸出中を一覧で見られる。** 物品番号・借りた人・持ち出してからの経過時間。返却履歴 100 件・開閉ログ 200 件
- **無登録の持ち出しを警告する。** 引き出しの開放を ToF センサで検知し、QR を読まないまま 40 秒・60 秒で 2 段階に警告し、打ち切ると画面上部に赤帯を残す
- **撮影もできる (既定はオフ)。** 開放を検知すると内蔵カメラで撮り、microSD があれば保存する。登録せずに持ち出した人の写真を画面に出し続ける
- **電源を切っても記録が残る。** 記録は NVS に書き、書く順番で電源断のときも整合を保つ
- **モバイルバッテリーでも動かせる。** 使わない電源を切り、無操作で減光・消灯する (実測 0.49A → 0.26A)
- ToF を付けない運用、音を鳴らさない運用、英字の利用者 ID (`TANAKA_` など) にも設定で対応する

## ハードウェア

| 部品 | 用途 |
|---|---|
| M5Stack Tab5 (C145、ESP32-P4) | 本体。5 インチ 720×1280 のタッチ画面・内蔵カメラ・microSD・RTC |
| Unit QRCode (U173、I2C 0x21) | 物品と名札の QR を読む。**側面のスイッチを I2C に** |
| Unit ToF4M (U172、VL53L1X、I2C 0x29) | 引き出しの開閉を検知する (任意) |
| Unit PaHub v2.1 (U040-B-V21、PCA9548AP) | Tab5 の Port A を分ける。挿す ch は起動時と 5 秒ごとに自動で探す |
| microSD | 写真の保存 (任意) |
| USB 充電器 / モバイルバッテリー | 給電 |

```
Tab5 Port A ── Unit PaHub v2.1 ─┬─ Unit QRCode … 画面の横
                                └─ Unit ToF4M  … 工具箱の上端、床向き
```

## ビルドと書き込み

Windows + PowerShell を前提にしています (`tools/serial.ps1` も Windows 専用)。GUI の Arduino IDE を使う場合も、下記と同じ版数のボードパッケージ・ライブラリを入れれば手順は読み替えられます。

**必要なもの:** M5Stack Tab5 本体 / Windows PC / USB-C ケーブル (**充電専用ではなくデータ通信対応**のもの)

### 1. 開発環境の準備 (最初の 1 回だけ)

1. [Arduino CLI](https://arduino.github.io/arduino-cli/latest/installation/) を入れる。公式サイトの Windows 版 zip を展開し、`arduino-cli.exe` にパスを通す
2. ESP32 のボードパッケージを追加する。**版数を必ず指定する** (最新版だと ESP32-P4 の挙動や `ChipVariant` の値が変わることがある)

   ```powershell
   arduino-cli config init
   arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   arduino-cli core update-index
   arduino-cli core install esp32:esp32@3.3.11
   ```

3. ライブラリを入れる (版数を指定)。本体ファームは M5UnitQRCode を使わない。理由は [spikes/README.md](spikes/README.md)

   ```powershell
   arduino-cli lib install "M5Unified@0.2.21" "M5GFX@0.2.28" "VL53L1X@1.3.1"
   ```

   `VL53L1X` は Pololu 製 (ToF センサ用)。同名の別ライブラリと出た場合は作者が Pololu のものを選ぶ

### 2. リポジトリを取得する

GitHub の緑の **Code** ボタン → **Download ZIP** で展開するか、

```powershell
git clone https://github.com/km190112/tab5-toolcheck.git
cd tab5-toolcheck
```

### 3. Tab5 を PC に繋ぎ、ポート番号を確認する

USB-C ケーブルで PC に繋ぎ、Windows の「デバイスマネージャー」→「ポート (COM と LPT)」を開いて増えた番号 (`COM3` など) を控える。`arduino-cli board list` でも確認できる。

### 4. コンパイルして書き込む

リポジトリのルートで実行する。`-p COM3` は手順 3 で控えた番号に置き換える。

```powershell
arduino-cli compile --fqbn esp32:esp32:m5stack_tab5:ChipVariant=prev3 firmware/ToolCheck
arduino-cli upload  --fqbn esp32:esp32:m5stack_tab5:ChipVariant=prev3 -p COM3 firmware/ToolCheck
```

- `ChipVariant=prev3` は ESP32-P4 の rev が 3.00 未満の個体向け (**まず prev3 で試す**。現状出回っている個体はほぼこちら)。手元の個体の rev は書き込み前には分からないので、書き込んで起動を確認してから決める (下記「うまく書き込めないとき」参照)
- `firmware/ToolCheck/partitions.csv` (記録用の NVS 領域 `tooldb` 1MB) はスケッチ直下にあるのでビルドで自動的に使われる。**一度書き込んだら変えない** (領域が動くと記録が消える)
- `compile` は初回数分、2 回目以降は数十秒。`upload` は 15 秒ほど

### うまく書き込めないとき

- **`arduino-cli` コマンドが見つからないと言われる** → 手順 1 でパスを通したか確認する。新しく開いた PowerShell ウィンドウで試す (パスの反映には再起動が要ることがある)
- **ポートが出てこない / `board list` に Tab5 が出ない** → ケーブルがデータ通信対応か確認し、別の USB ポート・ケーブルでも試す。Tab5 の電源が入っているか (画面が真っ黒でも背面の電源スイッチが ON か) も確認する
- **`upload` が失敗する / 途中で切れる** → `tools/serial.ps1` やシリアルモニタなど同じ COM ポートを使う他のツールを閉じてから再実行する。ケーブルを挿し直して直後にもう一度実行するだけで通ることもある
- **書き込みは終わったが、画面が真っ黒 / 文字化けする、起動しない** → `ChipVariant` が個体の rev と合っていない可能性が高い。`prev3` ⇔ `postv3` を入れ替えて `compile`・`upload` をやり直す (上のコマンドの 2 箇所とも変える)
- **起動はしたが版数・チップ rev を確認したい** → 設定画面の「このソフトについて」、またはシリアルで `info` を送ると `chip=ESP32-P4 rev=...` が出る (下記「シリアルの開き方」参照)

### シリアルの開き方

ESP32-P4 のネイティブ USB (USB-Serial/JTAG) は、**DTR を立てて開くとダウンロードモードに落ちることがある**。
`tools/serial.ps1` は DTR/RTS を落としてから開く。

```powershell
pwsh -NoProfile -File tools/serial.ps1 -Seconds 5 -Send 'info|power'
pwsh -NoProfile -File tools/serial.ps1 -Screenshot tools/out/screen.png   # 画面を PNG で保存
```

主なコマンド: `info` / `cfg` / `cfg set <名前> <値>` / `time set 2026-09-12T10:00` (現地時刻) / `dump loans|hist|openlog|alerts|cfg|all` (CSV) /
`tof` / `tof calibrate` / `i2c` / `about` / `screenshot`。
開閉や QR の注入 (`sim open` / `sim qr <コード>` など) は `cfg set dev_sim 1` のときだけ受け付ける。

## 使い方の要点

- 初回は時刻を合わせる (設定画面の「時刻」か `time set`)
- 設定画面はタブ右端の「設定」を 1 秒長押し → PIN (**既定 `0000`。使い始めたら変える**)
- 版数・開発者・このリポジトリの URL (QR) と、依存ライブラリを含むライセンス全文は、設定画面の「このソフトについて」で見られる
- ToF を使うときは、引き出しを全部閉めて設定画面の「引き出しセンサ」で基準を取る
- 物品には 2〜15 文字 (空白・カンマ以外の印字可能 ASCII) の QR ラベルを貼る。名札は既定で数字 7 桁。テスト用の QR は `tools/test_qr.html`
- 顔が写る写真は個人情報。**撮影を使うなら、設置先で掲示と合意を取ってから** 設定画面の「写真」で入れる

## ホストテスト

`firmware/ToolCheck/src/core/` は Arduino に依存しないロジック (セッション・貸出の記録・電力状態・設定の検証など) で、
Visual Studio の MSVC でビルドして検査する (442 tests)。テストを先に書いて RED を確かめてから実装した記録は [docs/testing/core.tdd.md](docs/testing/core.tdd.md)。

```powershell
pwsh -NoProfile -File tests/run.ps1
```

## 文書

- [docs/設計.md](docs/設計.md) — 仕様・画面・永続化と電源断の整合・省電力・リスク
- [docs/開発記録.md](docs/開発記録.md) — マイルストーンごとの実装と、実機で見つけて直したこと (USB シリアルの行落ち・SD の抜け検知など)
- [spikes/README.md](spikes/README.md) — Tab5 / ESP32-P4 の実測 (CPU 周波数と画面・ライトスリープ・カメラ・SD・電流・RTC)

## ライセンス

[MIT](LICENSE)。依存ライブラリのライセンスは [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

記載の製品名は各社の商標または登録商標です。本リポジトリは M5Stack 社・STMicroelectronics 社・Pololu 社とは関係がなく、承認も受けていません。
