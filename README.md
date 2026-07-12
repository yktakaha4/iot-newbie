# iot-newbie

IoT デバイス向けの Arduino CLI プロジェクト集です。コーディングエージェントは `arduino-cli` が利用可能である前提で作業してください。

## 前提

- `arduino-cli` が `PATH` から実行できること
- 対象デバイスが USB 接続されていること
- ボードとライブラリの依存は各スケッチ配下の `sketch.yaml` で管理すること
- Arduino CLI のローカル状態は [arduino-cli.yaml](arduino-cli.yaml) に従ってリポジトリ配下へ置くこと
- Wi-Fi などの秘密情報は `.env` から生成し、ソースコードへ直接書かないこと

## 構成

```text
.
├── arduino-cli.yaml
├── apps/
│   ├── HelloWorldM5Paper/
│   │   ├── HelloWorldM5Paper.ino
│   │   └── sketch.yaml
│   └── TRHCheckerM5Paper/
│       ├── .env.example
│       ├── TRHCheckerM5Paper.ino
│       └── sketch.yaml
├── scripts/
│   └── generate_secrets.sh
├── Makefile
└── .gitignore
```

- `apps/`: Arduino アプリケーションプロジェクトを置くディレクトリ。`apps/*/sketch.yaml` を持つディレクトリをアプリとして扱う
- `apps/HelloWorldM5Paper/HelloWorldM5Paper.ino`: M5Paper に `Hello World` を表示するサンプルスケッチ
- `apps/HelloWorldM5Paper/sketch.yaml`: `HelloWorldM5Paper` 用の FQBN、platform、ライブラリのバージョン定義
- `apps/TRHCheckerM5Paper/.env.example`: `TRHCheckerM5Paper` 用のローカル秘密情報テンプレート
- `apps/TRHCheckerM5Paper/TRHCheckerM5Paper.ino`: M5Paper 内蔵 SHT30 の温度・湿度を表示するスケッチ
- `apps/TRHCheckerM5Paper/sketch.yaml`: `TRHCheckerM5Paper` 用の FQBN、platform、ライブラリのバージョン定義
- `scripts/generate_secrets.sh`: 対象スケッチ配下の `.env` から `secrets.h` を生成するスクリプト
- `Makefile`: アプリの secrets 生成、ビルド、書き込みの入口
- `arduino-cli.yaml`: ローカル sketchbook と build cache の配置
- `.arduino/`: ローカル sketchbook、依存ライブラリ、build cache の配置先。コミットしない
- `.env`, `*/.env`, `*/secrets.h`: ローカル秘密情報。コミットしない

## 依存管理

Arduino CLI の sketch profile を使います。依存を追加・更新する場合は、ライブラリ本体をコミットせず、対象スケッチの `sketch.yaml` を更新してください。

`HelloWorldM5Paper` の現在のデフォルトプロファイル:

```yaml
profiles:
  default:
    fqbn: esp32:esp32:m5stack_paper
    platforms:
      - platform: esp32:esp32 (3.3.10)
        platform_index_url: https://espressif.github.io/arduino-esp32/package_esp32_index.json
    libraries:
      - M5Unified (0.2.18)
      - M5GFX (0.2.25)
```

デバイス固有のライブラリを使う場合は、可能な限りメーカー公式または Arduino Library Manager で配布されているものを選んでください。`HelloWorldM5Paper` では M5Stack 公式の `M5Unified` / `M5GFX` を使っています。

## ポート確認

USB 接続後、ポートを確認します。

```sh
arduino-cli board list
```

USB シリアルデバイスは例として次のように見えることがあります。

```text
/dev/cu.usbserial-02142314
```

## ビルド

秘密情報が必要なスケッチでは、先に対象スケッチ配下の `.env` を用意します。

```sh
cp apps/TRHCheckerM5Paper/.env.example apps/TRHCheckerM5Paper/.env
# apps/TRHCheckerM5Paper/.env の WIFI_SSID / WIFI_PASSWORD をローカル値に編集する
```

アプリ名は `APP` に指定します。`apps/$(APP)` が対象アプリです。

```sh
make compile APP=TRHCheckerM5Paper
```

`.env.example` があるアプリでは、`make compile` が先に `secrets.h` を生成します。`PORT` を指定すると、ビルド後にデバイスへ書き込みます。
`.env` の `KEY=VALUE` は `secrets.h` に `#define KEY "VALUE"` として出力されます。空行と `#` で始まるコメント行は無視されます。

```sh
make compile APP=TRHCheckerM5Paper PORT=/dev/cu.usbserial-02142314
```

アプリ一覧は次で確認できます。

```sh
make list-apps
```

## 直接コマンド

通常は `make` を使ってください。Arduino CLI を直接使う場合は、対象スケッチの profile 名を確認してから `arduino-cli --config-file arduino-cli.yaml compile --profile PROFILE apps/APP_NAME` を実行します。

## エージェント向け注意

- `.arduino/` は生成物なので編集・コミットしない
- `.env` と `secrets.h` は秘密情報なので編集してもコミットしない
- ライブラリの追加は `arduino-cli lib install` だけで終わらせず、`sketch.yaml` にバージョンを反映する
- ボード定義の変更は `fqbn` と `platforms` をセットで見直す
- 新しいデバイスやスケッチを追加するときは、`apps/` 配下に専用ディレクトリと `sketch.yaml` を作る
- シリアルポートへの書き込みは環境によって権限が必要になる
- デバイス固有の制約は、対象スケッチの README またはコメントに閉じ込める
