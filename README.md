# iot-newbie

Arduino CLI で IoT デバイス向けスケッチを管理するリポジトリです。

## 前提

- `arduino-cli` が `PATH` から実行できること
- 対象デバイスが USB 接続されていること
- ボード、platform、ライブラリの依存は各アプリの `sketch.yaml` で管理すること

## アプリ一覧

```sh
make list-apps
```

アプリは `apps/` 配下に置きます。各アプリディレクトリには `sketch.yaml` が必要です。

## ビルド

```sh
make compile APP=HelloWorldM5Paper
```

秘密情報が必要なアプリでは、先に `.env.example` から `.env` を作ります。

```sh
cp apps/TRHCheckerM5Paper/.env.example apps/TRHCheckerM5Paper/.env
```

`.env` がある場合、`make compile` は `secrets.h` を自動生成してからビルドします。

## 書き込み

ポートを確認します。

```sh
arduino-cli board list
```

`PORT` を指定して書き込みます。

```sh
make upload APP=TRHCheckerM5Paper PORT=/dev/cu.usbserial-02142314
```

## 注意点

- 通常は `make` を使い、Arduino CLI を直接実行しない
- `sketch.yaml` の profile 名は基本的に `default` にする
- `.arduino/`、`.env`、`secrets.h` はコミットしない
- ライブラリを追加・更新したら、対象アプリの `sketch.yaml` にバージョンを反映する
- ボードを変える場合は、`fqbn` と `platforms` をセットで見直す
- 秘密情報はソースコードへ直接書かない
