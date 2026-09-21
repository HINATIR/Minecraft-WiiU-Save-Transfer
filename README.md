# MCU Save Transfer

[English](README_en.md)

`MCU Save Transfer` は、ローカルネットワーク上のUDP通信を使用して、Wii UのワールドセーブデータをSwitchへ転送するwxWidgets製デスクトップアプリケーションです。

## 機能

- ローカルネットワーク上のSwitch自動探索
- SwitchのIPv4アドレス手動入力
- ワールドデータファイルと`.ext`ファイルの選択
- `.ext`のワールド名表示・編集
- `.ext`のアイコン表示・PNGインポート
- `.ext`未選択時の透明アイコンとデフォルトメタデータ
- 転送状態、進捗、完了、キャンセル、エラー理由の表示
- 通信確認用のデバッグログ

## 動作内容

アプリケーションはまずSwitchのLANセッションを探索します。SwitchとのPIAセッションを確立し、受信側の参加を待ってから、セーブデータを信頼性のある分割データとして送信します。各データチャンクの確認を受け取ってから、次のチャンクを送信します。

プログレスバーは、ワールドデータとアイコンデータを合わせた転送済み容量を表示します。

送信されるデータは次の構成です。

```text
ワールドデータ + .extのPNGペイロード
```

選択した`.ext`の先頭0x100バイトにあるバイナリヘッダはメタデータとして使用され、転送ペイロードには含まれません。

## `.ext`メタデータ

`.ext`を選択すると、ワールド名、シード、Host options、Extra data、保存メタデータを表示します。ワールド名の編集やアイコンPNGのインポートを行っても、元の`.ext`ファイルは変更されません。

`.ext`は省略できます。選択しない場合は、次のデフォルト値を使用します。

```text
4J_HOSTOPTIONS = 3e9c
4J_TEXTUREPACK = 0
4J_EXTRADATA   = 79900a8
4J_#LOADS      = 0
```

デフォルトアイコンは完全に透明なPNGです。負の`4J_SEED`にも対応しています。

### 互換性に関する注意

ワールドデータと`.ext`ファイルは、同じゲームアップデート世代のものを使用してください。

Seaアップデート後のワールドデータに、Seaアップデート前の`.ext`ファイルを使用した場合、ワールドを開けることがあります。しかし、ロード開始前にSwitch側がフリーズする場合があります。

`4J_EXTRADATA`は、セーブまたはゲームデータのバージョンを表している可能性が高い値です。ただし、この解釈はまだ完全には確認されていません。可能な限り、同じアップデート世代のファイルを組み合わせてください。

## 使用方法

1. PCとSwitchを同じローカルネットワークに接続し、`MCU_Save_Transfer`を起動します。
2. Switch探索方式を選択します。`Auto`では自動探索、`Manual IP`ではSwitchのIPv4アドレスを入力します。
3. ワールドデータファイルを選択します。
4. 対応する`.ext`ファイルを選択します。省略するとデフォルト値を使用します。
5. 必要に応じてワールド名を編集するか、アイコンPNGをインポートします。
6. `Send`を押し、状態ラベル、プログレスバー、デバッグログを確認します。

状態ラベルには、`Ready`、`Discovering`、`Connecting`、`Waiting for receiver`、`Sending`、`Completed`、`Cancelled`、または理由付きのエラーが表示されます。

## ビルド

このプロジェクトは、mbedTLSとwxWidgetsをGitサブモジュールとして使用しています。初回のみ、次のコマンドでサブモジュールを取得してください。

```sh
git submodule update --init --recursive
```

MinGWなどのGNU Make環境では次を実行します。

```sh
make
```

WindowsのVisual Studio C++ Build Toolsでは次を実行します。

```bat
build-msvc.bat
```

実行ファイルは次に生成されます。

```text
bin/MCU_Save_Transfer.exe
```

## プロジェクト構成

- `src/main.cpp` - wxWidgetsのUIとユーザー操作
- `src/transfer_worker.*` - 転送処理とバックグラウンド送信
- `src/pia_lan.*` - LAN探索とセッション確立
- `src/pia_packet.*` - 暗号化PIAパケット処理
- `src/save_transfer_metadata.*` - `.ext`メタデータとデフォルトペイロード処理
- `src/reliable_sliding_window.*` - 分割、確認応答、再送処理
- `third_party/mbedtls` - mbedTLSサブモジュール
- `third_party/wxWidgets` - wxWidgetsサブモジュール

## 外部ライブラリとライセンス

- [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) - mbedTLS 3.6.7。Apache-2.0またはGPL-2.0-or-laterのデュアルライセンスです。このプロジェクトではApache-2.0を使用します。全文は[`third_party/mbedtls/LICENSE`](third_party/mbedtls/LICENSE)にあります。
- [wxWidgets](https://github.com/wxWidgets/wxWidgets) - wxWidgets 3.2.8.1。wxWindows Library Licence 3.1です。このライセンスには、ライブラリを使用するアプリケーションのバイナリを独自条件で配布できる例外が含まれています。全文は[`third_party/wxWidgets/docs/licence.txt`](third_party/wxWidgets/docs/licence.txt)にあります。

wxWidgetsには、個別のライセンスを持つサードパーティコンポーネントも含まれています。アプリケーションを配布する場合は、両サブモジュールに含まれる関係するライセンスファイルと著作権表示を保持してください。
