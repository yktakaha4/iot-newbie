# iot-newbie

IoT デバイス向けの Arduino CLI プロジェクト集です。コーディングエージェントは `arduino-cli` が利用可能である前提で作業してください。

## 前提

- `arduino-cli` が `PATH` から実行できること
- 対象デバイスが USB 接続されていること
- ボードとライブラリの依存は各スケッチ配下の `sketch.yaml` で管理すること
- Arduino CLI のローカル状態は [arduino-cli.yaml](arduino-cli.yaml) に従ってリポジトリ配下へ置くこと

## 構成

```text
.
├── arduino-cli.yaml
├── HelloWorldM5Paper/
│   ├── HelloWorldM5Paper.ino
│   └── sketch.yaml
└── .gitignore
```

- `HelloWorldM5Paper/HelloWorldM5Paper.ino`: M5Paper に `Hello World` を表示するサンプルスケッチ
- `HelloWorldM5Paper/sketch.yaml`: `HelloWorldM5Paper` 用の FQBN、platform、ライブラリのバージョン定義
- `arduino-cli.yaml`: ローカル sketchbook と build cache の配置
- `Arduino/`, `.arduino/`: ローカル生成物。コミットしない

## 依存管理

Arduino CLI の sketch profile を使います。依存を追加・更新する場合は、ライブラリ本体をコミットせず、対象スケッチの `sketch.yaml` を更新してください。

`HelloWorldM5Paper` の現在のプロファイル:

```yaml
profiles:
  m5stack_paper:
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

対象スケッチの profile 名を確認してからビルドします。`HelloWorldM5Paper` の場合は次の通りです。

```sh
arduino-cli --config-file arduino-cli.yaml compile --profile m5stack_paper HelloWorldM5Paper
```

このコマンドは `HelloWorldM5Paper/sketch.yaml` に従って、必要な platform と library を復元してビルドします。

## 書き込み

`PORT` は `arduino-cli board list` で確認した USB シリアルポートに置き換えてください。

```sh
arduino-cli --config-file arduino-cli.yaml compile --profile m5stack_paper -p PORT -u HelloWorldM5Paper
```

例:

```sh
arduino-cli --config-file arduino-cli.yaml compile --profile m5stack_paper -p /dev/cu.usbserial-02142314 -u HelloWorldM5Paper
```

## エージェント向け注意

- `Arduino/` と `.arduino/` は生成物なので編集・コミットしない
- ライブラリの追加は `arduino-cli lib install` だけで終わらせず、`sketch.yaml` にバージョンを反映する
- ボード定義の変更は `fqbn` と `platforms` をセットで見直す
- 新しいデバイスやスケッチを追加するときは、スケッチごとに専用ディレクトリと `sketch.yaml` を作る
- シリアルポートへの書き込みは環境によって権限が必要になる
- デバイス固有の制約は、対象スケッチの README またはコメントに閉じ込める
