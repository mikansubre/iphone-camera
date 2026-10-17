# iPhone Camera

iPhone のカメラを、**アプリをインストールせずに Safari だけで** Windows の Web カメラにするツールです。
PC に表示された QR コードを iPhone で読み取って「開始」を押すだけで、Zoom / Teams / Discord / OBS /
Windows の「カメラ」アプリなどから普通のカメラとして選べるようになります。

iPhone は**何台でも**つなげられ、それぞれが別の仮想カメラ (`iPhone Camera 1`, `iPhone Camera 2`, …) になります。

```
iPhone A (Safari) ─┐                          ┌─▶ iPhone Camera 1 ─┐
iPhone B (Safari) ─┼─ WebRTC (H.264) ─▶ PC ───┼─▶ iPhone Camera 2 ─┼─▶ Zoom / OBS / Teams …
iPhone C (Safari) ─┘    同じ Wi-Fi           └─▶ iPhone Camera 3 ─┘
```

## 特長

- **iPhone 側はインストール不要** — Safari で QR を開くだけ
- **低遅延・高画質** — 1080p / 60fps、iPhone のカメラから PC のアプリまで 30ms 前後 (後述の計測値)
- **複数台対応** — つないだ台数だけ仮想カメラが増える (上限なし)。一度つないだ iPhone は次も同じカメラに戻る
- **Windows 11 ネイティブの仮想カメラ** — OBS などのドライバ不要。ツールの起動中だけ存在し、終了すると消える
- **PC の管理ページ** — カメラごとのプレビュー・受信状況・「アプリで使用中」表示、カメラの追加/削除、割り当てモード切替

## 目次

- [必要なもの](#必要なもの)
- [セットアップ](#セットアップ)
- [使い方](#使い方)
- [オプション](#オプション)
- [性能](#性能)
- [仕組み](#仕組み)
- [セキュリティ](#セキュリティ)
- [トラブルシューティング](#トラブルシューティング)
- [既知の制限](#既知の制限)
- [開発メモ](#開発メモ)
- [謝辞](#謝辞)

## 必要なもの

| | |
|---|---|
| PC | Windows 11 (build 22000 以降)。Windows 10 は未確認 (Windows 11 仮想カメラが使えないため、動いても OBS などで 1 台のみ) |
| Python | 3.10 以降 (3.14 で動作確認) |
| ビルド環境 | 複数台対応の仮想カメラ (`vcam/`) を使う場合のみ: [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/ja/downloads/) の「C++ によるデスクトップ開発」、CMake 3.24 以降、Git |
| iPhone | iOS 16.4 以降の Safari 推奨 (画面スリープ防止のため) |
| ネットワーク | PC と iPhone が同じ Wi-Fi / LAN。複数台なら 5GHz 帯推奨 |

## セットアップ

### 1. 取得

```bat
git clone https://github.com/mikansubre/iphone-camera.git
cd iphone-camera
```

### 2. 仮想カメラのインストール (初回だけ・管理者権限)

`vcam\install_vcam.bat` をダブルクリックし、UAC で「はい」を押します。

- 初回は `vcam\build\` に `iphonecam.dll` をビルドします (数十秒)
- DLL を `C:\Program Files\iPhoneCam\` にコピーして COM 登録します。
  Windows のカメラサービス (Frame Server) は **HKLM に登録されたもの**しか読み込まないため、管理者権限が必要です
- 仮想カメラの解像度 (既定 1920x1080、60fps / 30fps) を `HKLM\SOFTWARE\iPhoneCam` に書きます
- ログ用フォルダ `C:\ProgramData\iPhoneCam\logs\` を作ります

解像度を変える / アンインストールする:

```bat
vcam\install_vcam.bat -Width 1280 -Height 720
vcam\install_vcam.bat -Uninstall
```

> 仮想カメラをインストールしない場合は、[OBS Studio](https://obsproject.com/) の「OBS Virtual Camera」などを使って
> **1 台だけ**動かせます (OBS の起動は不要)。

### 3. 起動

`start.bat` をダブルクリックします。初回は `.venv` を作って依存パッケージ (`requirements.txt`) を入れます。

- Windows ファイアウォールの確認が出たら「プライベート ネットワーク」を許可してください (WebRTC は UDP も使います)
- PC のブラウザに管理ページ (`http://127.0.0.1:8080/`) が開きます

## 使い方

1. 管理ページの QR コードを iPhone のカメラで読み取る
2. Safari で「この接続ではプライバシーが保護されません」と出たら
   「詳細を表示」→「このWebサイトを閲覧」→「Webサイトを閲覧」
   (自己署名証明書のため。Safari がカメラを使うには HTTPS が必須です)
3. 「開始」を押してカメラを許可。画面上部に割り当てられたカメラ名 (例: `iPhone Camera 1 に配信中`) が出ます
4. 通話アプリのカメラ選択で **`iPhone Camera N (Windows 仮想カメラ)`** を選ぶ
   (末尾の「(Windows 仮想カメラ)」は Windows が自動で付けます)

起動直後から `iPhone Camera 1` が存在するので、iPhone をつなぐ前にアプリ側で選んでおけます。

### iPhone 側の画面

| 操作 | 内容 |
|---|---|
| ⟲ 背面 / 前面 | 使うカメラの切替 |
| 720p 30 / 720p 60 / 1080p 30 / 1080p 60 | 解像度と fps (既定 1080p 60) |
| 開始 / 停止 | 配信の開始と停止 |

配信中は **Safari を前面に出したまま**にしてください (iOS はバックグラウンドのページのカメラを止めます)。
画面左下に送信状況が出ます。

```
camera 1920x1080 60fps          ← カメラが実際に撮っている解像度/fps
send   1920x1080 60fps 12.3Mbps ← 実際に送っている解像度/fps/ビットレート
limit  bandwidth                ← 送信側が画質を落としている理由 (出ていなければ制限なし)
```

- `limit bandwidth` で解像度が下がる → `--bitrate` を上げる
- `limit cpu` → iPhone の処理が追いついていないので 720p か 30fps にする

### カメラの割り当て (管理ページ右上で切替)

| モード | QR | 割り当て |
|---|---|---|
| **自動割り当て** (既定) | 全員共通の 1 つ | 前回と同じ iPhone → 前回のカメラ。それ以外 → 空いているカメラ (未使用のもの → 配信が終わってから長いもの の順)。空きが無ければ新しく作る |
| **カメラごとの QR** | カメラごとに 1 つ | 読んだ QR のカメラに入る。そのカメラに別の iPhone がいれば入れ替わる。どの iPhone をどのカメラにするか決めたいとき向け |

カメラの数は「同時につないだ iPhone の最大数」まで増えます。iPhone の見分けは Safari の localStorage に保存した
端末 ID で行うため、プライベートブラウズでは毎回別の iPhone として扱われます。

### PC の管理ページ

- カメラごとのカード: プレビュー (仮想カメラに送っているのと同じ映像)、接続中の iPhone、
  受信解像度 / fps / ビットレート、PC の処理時間、パケット欠落の回数、「アプリで使用中」
- ＋ でカメラを追加、× で削除 (配信中の iPhone は切断されます)

## オプション

`start.bat` (= `python server.py`) に渡せます。

| オプション | 既定 | 説明 |
|---|---|---|
| `--assign auto\|fixed` | `auto` | 起動時の割り当てモード (管理ページでも切替可) |
| `--bitrate MBPS` | `16` | iPhone 1 台あたりの送信ビットレート |
| `--mode fit\|fill` | `fit` | 縦持ち映像を黒帯付きで全体表示 / 切り抜いて全面表示 |
| `--backend auto\|mf\|obs\|unitycapture` | `auto` | 仮想カメラ。`auto` は Windows 11 仮想カメラがあればそれ、なければ OBS など |
| `--width` / `--height` / `--fps` | `1920` / `1080` / `60` | OBS などを使うときの出力 (Windows 11 仮想カメラはインストール時に決める) |
| `--host IP` | 自動検出 | QR に載せる PC の IP (VPN 使用時など自動検出が外れるとき) |
| `--port` / `--ui-port` | `8443` / `8080` | iPhone 向け HTTPS / 管理ページのポート |
| `--no-browser` | | 起動時に管理ページを開かない |

複数台つなぐときは Wi-Fi の帯域 (1 台あたり `--bitrate`) と PC の CPU (1 台ごとに H.264 デコード) に注意してください。
5GHz の Wi-Fi なら 1080p60 で 3〜4 台が目安です。

## 性能

Edge で iPhone 用ページを動かし (カメラの代わりに時刻を描いた canvas)、仮想カメラを別のアプリとして開いて
映像に写った時刻との差を測った値です。iPhone のカメラ自体の遅延と Wi-Fi 区間は含みません。

| 項目 | 値 |
|---|---|
| 遅延 (送信側エンコード → アプリが受け取るまで) | 中央値 25〜30ms / 90 パーセンタイル 33〜36ms |
| アプリに届く新しいフレーム | 受信 55fps に対して 55fps (取りこぼしなし) |
| PC の処理時間 (1080p, 1 台) | 約 2.7ms / フレーム |
| 仮想カメラ部分のみ (2 台同時) | 各 57fps、遅延 中央値 19〜22ms |

改善の経緯 (同じ条件での計測):

| | 初期版 (aiortc 既定 + OBS) | 現在 |
|---|---|---|
| 720p30 を送ったときの受信 | **320x180 / 7fps** (帯域推定が絞り込んでいた) | 1280x720 / 30fps |
| 遅延 | 中央値 310ms / 90% 688ms | 中央値 25〜30ms |

## 仕組み

### 全体

```
iPhone Safari                    PC: server.py (Python / aiortc)                Windows
─────────────                    ───────────────────────────────                ───────
getUserMedia ─▶ RTCPeerConnection ══ WebRTC ══▶ Connection ─▶ CameraOutput ──▶ iphonecam.dll ─▶ Frame Server ─▶ アプリ
 (H.264 HW)      ▲                   (UDP)      (1 台ごと)     (専用スレッド)     共有メモリ        (MF 仮想カメラ)
                 └── /offer (HTTPS, QR の URL に含めたトークンで認証)
```

### ファイル構成

| パス | 内容 |
|---|---|
| `server.py` | 本体。HTTPS サーバー、WebRTC 受信、カメラの割り当て (`Hub` / `Slot` / `Connection`)、映像変換と送出 (`CameraOutput`)、管理ページ API |
| `rtc_tuning.py` | aiortc の受信設定を LAN 内の高画質・低遅延向けに調整 |
| `mfcam.py` | `iphonecam.dll` を ctypes で呼んで仮想カメラを作成・削除し、フレームを書き込む |
| `vcam/` | Windows 11 仮想カメラのメディアソース (C++ / WRL / WIL) とインストーラ |
| `static/phone.html` | iPhone 用ページ (getUserMedia + WebRTC、画面スリープ防止、送信状況表示) |
| `static/host.html` | PC 用管理ページ |
| `certs/` | 起動時に LAN の IP 用の自己署名証明書を自動生成 (git 管理外) |

### WebRTC の受信 (`rtc_tuning.py`)

aiortc の既定値はインターネット越しの汎用的な通話向けで、そのまま iPhone の 1080p60 を受けると画質と遅延が大きく悪化します。
次の調整を `RTCPeerConnection` を作る前に適用しています。

| 問題 | 対策 |
|---|---|
| 受信側帯域推定 (REMB) が Python のイベントループの揺らぎを回線混雑と誤認し、送信側のビットレートを絞る → iPhone が解像度・fps を落とす | REMB と abs-send-time を無効化。SDP に `x-google-start/min/max-bitrate` と `b=AS` を入れ、最初から指定ビットレートで送らせる (下げるのはパケットロス時のみ) |
| H.264 を Constrained Baseline / Level 3.1 (720p30 相当) としか宣言しない | High / Constrained High も受け付け、Level 5.2 を宣言 |
| ジッタバッファが「次のフレームの到着」を待ってから出力 (常に 1 フレーム遅れる) | フレームの最終パケット (marker bit) が揃った時点で出力 |
| ジッタバッファが 128 パケットしかなく 1080p のキーフレームが収まらない → 破棄と再要求を繰り返す | 2048 パケットに拡大 |
| 再送でも埋まらない欠落でバッファが詰まる | 0.25 秒で諦めてキーフレームを要求 |

デコーダには `low_delay` フラグを付けています。

### 映像の変換と送出 (`CameraOutput`)

- カメラごとに専用スレッドを持ち、フレームが届いた時点で出力サイズに合わせて送出します
  (変換が追いつかないときは古いフレームを捨て、遅延を溜めません)
- 仮想カメラのネイティブ形式 (Windows 11 仮想カメラ / OBS は NV12、Unity Capture は RGBA) へ PyAV (swscale) で
  直接変換します。H.264 のデコード結果 (yuv420p) から NV12 は並べ替えだけで済みます
- iPhone が切断されると 2 秒後に待機画面 (`iPhone Camera N - Waiting for iPhone...`) に切り替えます
- 管理ページのプレビューは同じフレームを縮小して MJPEG で流しています

### 接続の管理

- Safari のタブを閉じても WebRTC の切断検知には数十秒かかるため、**映像の途絶**で判断します。
  3 秒届かなければそのカメラを他の iPhone に譲れる状態にし、20 秒届かなければ接続を閉じます
- 同じカメラに新しい接続が来たら古い接続を閉じます (ページの再読み込みや、カメラごとの QR での入れ替え)

### Windows 11 仮想カメラ (`vcam/`)

`MFCreateVirtualCamera` (Windows 11 の Media Foundation API) で作る仮想カメラです。ドライバではなく、
Windows のカメラサービス (Frame Server) が読み込むメディアソース (COM の DLL) として動きます。

```
server.py のプロセス                                Frame Server (svchost, session 0, LocalService)
───────────────────                                ───────────────────────────────────────────────
mfcam.VirtualCamera("iPhone Camera 1")             Activator ─▶ MediaSource ─▶ MediaStream ─▶ アプリへ
  └ ICam_Create: MFCreateVirtualCamera                                            │
  └ ICam_Send:  NV12 を書き込み ──▶ 共有メモリ Global\iPhoneCam.<名前のハッシュ> ◀┘ (3 枚のリング + イベント)
```

- **カメラの識別**: 1 つの DLL (CLSID) から台数無制限にカメラを作るため、メディアソースは自分がどのカメラかを
  Frame Server が渡す属性 `MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME` (作成時に付けた名前) から知り、
  共有メモリの名前を決めます。デバイスのプロパティに情報を持たせる `IMFVirtualCamera::AddProperty` は
  管理者権限が必要なため使っていません
- **共有メモリ**: `Global\` 名前空間への作成には特権が要るため、**Frame Server 側が作り**、server.py は開くだけです。
  アプリがカメラを開いている間だけ存在するので、使われていないカメラには書き込みを省けます。
  アクセス権は SYSTEM / LocalService / Administrators / 対話ログオンユーザーのみ
- **フレームの受け渡し**: 新しいフレームが書かれた瞬間 (イベント) に、Frame Server からの要求 (`RequestSample`) に応えます。
  新しいフレームが来ない間も 100ms ごとに同じ画を送り直し、アプリのタイムアウトを防ぎます
- **公開する形式**: NV12、インストール時の解像度で 60fps と 30fps
- **寿命**: `MFVirtualCameraLifetime_Session` なので、server.py が終了すると Windows が自動で削除します
- **ログ**: `C:\ProgramData\iPhoneCam\logs\<プロセス名>.log`
  (`svchost.exe.log` が Frame Server 側、`python.exe.log` が server.py 側)

#### DLL のエクスポート (`mfcam.py` から使用)

| 関数 | 内容 |
|---|---|
| `ICam_GetFormat(&w, &h)` | インストール時に決めた解像度 |
| `ICam_Create(name, &handle)` | 仮想カメラを作って開始 |
| `ICam_Send(handle, nv12, size)` | フレームを書き込む。`S_OK` = アプリが使用中、`S_FALSE` = 未使用 |
| `ICam_InUse(handle)` | アプリが使用中か (直近 1 秒にフレームを渡したか) |
| `ICam_Remove(handle)` | 仮想カメラを削除 |

## セキュリティ

- iPhone 向けの URL には起動ごとにランダムなトークンを含めており、トークンを知らない端末からは接続できません
- iPhone 向けページは LAN に公開されます (HTTPS、自己署名証明書)。証明書は LAN の IP 用に自動生成され、`certs/` に保存されます
- 管理ページ (カメラの追加・削除、プレビュー) は `127.0.0.1` だけで待ち受け、Host / Origin ヘッダーを確認して
  他のサイトからの操作 (CSRF / DNS リバインディング) を拒否します
- 仮想カメラの DLL は `C:\Program Files\iPhoneCam\` (管理者のみ書き込み可) に置かれ、Frame Server (LocalService) に読み込まれます

## トラブルシューティング

| 症状 | 対処 |
|---|---|
| iPhone でページが開けない | PC と同じネットワークか、ファイアウォールで Python が許可されているか確認。IP が違う場合は `--host` で指定 |
| 「接続中」のまま進まない | WebRTC は UDP を使うため、ファイアウォールで Python の UDP 通信も許可が必要 |
| 管理ページに「1 台のみ」と出る | `vcam\install_vcam.bat` で Windows 11 仮想カメラを入れる |
| アプリでカメラが真っ黒 | `C:\ProgramData\iPhoneCam\logs\svchost.exe.log` を確認 |
| アプリにカメラが出てこない | server.py が起動しているか確認 (仮想カメラは起動中だけ存在する)。`vcam\install_vcam.bat` をやり直す |
| 「すでに起動しています」と出る | 前に開いた `start.bat` のウィンドウが残っている。そちらがそのまま使える。終了はそのウィンドウで Ctrl+C |
| カクつく・止まる | 管理ページの「欠落」が増えていれば Wi-Fi でパケットが失われている。`--bitrate` を下げるか 5GHz 帯に接続 |
| 画質が粗い・解像度が下がる | iPhone 画面左下の `limit` を確認 (`bandwidth` なら `--bitrate` を上げる) |
| IP が変わるたびに証明書の警告が出る | IP に合わせて証明書を作り直すため。ルーターで PC の IP を固定すると避けられる |
| `start.bat` がすぐ閉じる | PowerShell で `start.bat` を直接実行してエラーを確認 |

## 既知の制限

- **Teams**: Windows 11 の仮想カメラ (`MFCreateVirtualCamera`) について「プレビューには映るが相手に送られない」という報告があります
  ([VCamSample の issue](https://github.com/smourier/VCamSample))。その場合は `--backend obs` で OBS Virtual Camera を使ってください
- 実機の iPhone (Safari) では 1 台・OBS Virtual Camera 経由での動作のみ確認しています。複数台と Windows 11 仮想カメラ経由は
  PC 上のテスト ([開発メモ](#テストのしかた)) でのみ確認しています
- 前面カメラが 1080p60 に対応していない機種では 30fps になります (iPhone 画面左下の `camera` 行で確認できます)
- iPhone を見分ける端末 ID は Safari の localStorage に保存するため、プライベートブラウズでは毎回別の iPhone として扱われます
- 音声は送りません
- Windows 11 仮想カメラの解像度はインストール時に固定されます (全カメラ共通)

## 開発メモ

### DLL のビルド

```bat
cmake -S vcam -B vcam\build -G "Visual Studio 17 2022" -A x64
cmake --build vcam\build --config Release
vcam\install_vcam.bat
```

WIL は CMake の FetchContent で取得します。CRT は静的リンクなので、Frame Server 側で VC++ ランタイムは不要です。
DLL を作り直したら `install_vcam.bat` で入れ直してください (Frame Server を止めて古い DLL を差し替えます)。

### テストのしかた

実機がなくても、Playwright + Edge で次のように確認できます (開発時に使用、リポジトリには含めていません)。

- iPhone 用ページの `navigator.mediaDevices.getUserMedia` を、`Date.now()` を白黒のブロックで描く canvas の
  `captureStream()` に差し替えて配信する
- 仮想カメラを別のページから `getUserMedia({ video: { deviceId } })` で開き、`requestVideoFrameCallback` ごとに
  ブロックを読み取って、新しいフレームの fps と遅延を測る
- ブラウザのコンテキストを分けると別の iPhone (別の端末 ID) として扱われるので、複数台の割り当てを確認できる

## 謝辞

- [aiortc](https://github.com/aiortc/aiortc) — Python の WebRTC 実装
- [PyAV](https://github.com/PyAV-Org/PyAV) — H.264 のデコードと映像変換
- [pyvirtualcam](https://github.com/letmaik/pyvirtualcam) — OBS Virtual Camera などへの出力
- [VCamSample](https://github.com/smourier/VCamSample) (MIT) — `MFCreateVirtualCamera` のメディアソース実装の参考
- [WIL](https://github.com/microsoft/wil) — Windows Implementation Libraries
