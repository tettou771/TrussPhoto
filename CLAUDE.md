# TrussPhoto プロジェクト固有設定

## 概要

TrussC ベースの写真管理アプリ。クライアント（TrussPhoto）とサーバ（TrussPhotoServer）の2つのアプリで構成。

- **TrussPhoto**: デスクトップ写真ビューア + サーバ同期クライアント
- **TrussPhotoServer**: REST API サーバ（RAWインポート、サムネイル生成、メタデータ管理）

## ビルド方法

```bash
# クライアント
cd apps/TrussPhoto
cmake --preset macos
cmake --build build-macos

# サーバ
cd apps/TrussPhotoServer
cmake --preset macos
cmake --build build-macos
```

ソースファイルを追加/削除した場合は `cmake --preset macos` の再実行が必要（GLOB ベース）。

## アーキテクチャ

### クライアント (TrussPhoto)
```
src/
├── tcApp.h/cpp       # メインアプリ（起動フロー、イベント処理）
├── Settings.h        # 設定永続化（settings.json）
├── PhotoProvider.h   # 写真管理の中核（ローカル + サーバ抽象化）
├── PhotoGrid.h       # スクロール可能なサムネイルグリッドUI
├── PhotoItem.h       # 個々の写真アイテム（サムネ + ラベル）
├── AsyncImageLoader.h # バックグラウンドサムネイルローダー
└── UploadQueue.h     # バックグラウンドアップロード（リトライ付き）
```

### サーバ (TrussPhotoServer)
```
src/
├── tcApp.h/cpp       # Crow REST APIサーバ
├── Photo.h           # Photo構造体（NLOHMANN_DEFINE_TYPE_INTRUSIVE）
└── JsonStorage.h     # JSONファイルベースのストレージ
```

### データフロー
1. フォルダドロップ → `PhotoProvider::scanFolder()` → エントリ登録（即座） + ファイルコピー（バックグラウンド）
2. サムネイル表示 → `AsyncImageLoader` → `PhotoProvider::getThumbnail()` → ローカルキャッシュ → サーバ → RAWデコード
3. 自動アップロード → `UploadQueue` → `POST /api/import` → リトライ最大3回

### ローカル運用モード
**サーバなしでも完全に動作する。** `settings.json` の `serverUrl` が空なら：
- UploadQueue スレッドは起動しない
- 定期sync は走らない
- 全写真は `LocalOnly` 状態のまま
- サムネイルはローカルRAWからデコード → キャッシュ

サーバURLを後から設定すれば（MCPの `set_server` ツールまたは settings.json 編集）、
UploadQueue が起動し、sync + 自動アップロードが始まる。

### サーバ移行
**ローカルのRAWファイルとlibrary.jsonがあれば、サーバは完全に再構築できる。**

サーバ固有のデータ（`importedAt`, サーバ側サムネ, ファイルパス）は全てRAWから再生成可能。

移行手順：
1. 新サーバを起動（空の状態）
2. `settings.json` の `serverUrl` を新サーバに変更（またはMCP `set_server`）
3. アプリ起動 → `syncWithServer()` → 新サーバにない `Synced` 写真が自動的に `LocalOnly` に戻る
4. `enqueueLocalOnlyPhotos()` → 全写真が自動で新サーバにアップロードされる

### SyncState
| 状態 | 値 | 意味 | バッジ色 |
|------|-----|------|----------|
| LocalOnly | 0 | ローカルのみ（未アップロード） | オレンジ |
| Syncing | 1 | アップロード中 | 青 |
| Synced | 2 | ローカル + サーバ両方 | 緑 |
| ServerOnly | 3 | サーバのみ（ローカルRAWなし） | 紫 |

### ID形式
写真IDは `filename_filesize` 形式で統一（サーバ・クライアント共通）。
例: `DSC00001.ARW_42991616`

### 永続化ファイル（クライアント）
```
bin/data/
├── settings.json       # サーバURL、ライブラリフォルダ
├── library.json        # 全写真エントリ（SyncState含む）
└── thumbnail_cache/    # サムネイルJPEGキャッシュ
```

### 永続化ファイル（サーバ）
```
bin/data/
├── storage/library.json  # 全写真メタデータ
└── thumbnails/           # 生成サムネイル
```

## サーバAPI

| Method | Endpoint | 説明 |
|--------|----------|------|
| GET | /api/health | ヘルスチェック |
| GET | /api/photos | 写真一覧（id, filename, fileSize, camera, width, height） |
| GET | /api/photos/:id | 写真詳細 |
| GET | /api/photos/:id/thumbnail | サムネイルJPEG取得 |
| POST | /api/import | RAWインポート `{"path": "/path/to/file.ARW"}` |
| DELETE | /api/photos/:id | 写真削除 |

サーバポート: **8080**

## 使用アドオン

- **tcxCurl** - HTTPクライアント（libcurl / Emscripten Fetch）
- **tcxLibRaw** - RAW画像デコード（LibRaw、FetchContent）
- **tcxLut** - LUTカラーグレーディング（GPU シェーダー、header-only）
- **tcxExiv2** - EXIF/MakerNote メタデータ（libexiv2、brew前提）
- **tcxLensfun** - レンズ補正（lensfun、brew前提）
- **tcxCrow** - HTTPサーバ（Crow） ※サーバのみ

## 開発時の注意点・失敗談

### ポート番号の食い違い
サーバは8080で動いているのにクライアントのSettings.hデフォルトが18080だった。
settings.jsonに古い値がキャッシュされ、アプリ再起動しても直らなかった。
→ **Settings.hのデフォルト値とsettings.jsonの両方を確認すること**

### fs::copy_file によるビーチボール
`scanFolder()` でメインスレッド上で `fs::copy_file()` を呼んでいたため、
14枚のARW（各40MB、計560MB）のコピーでUIがフリーズした。
→ **ファイルコピーはバックグラウンドスレッドに移動**。scanFolder()はエントリ登録だけして即座に返り、コピーは裏で走る。`processCopyResults()` をupdate()で呼んでパスを更新。

### PhotoLibrary → PhotoProvider の移行
元々 `PhotoLibrary`（パスベース、index管理）を使っていたが、サーバ同期のために `PhotoProvider`（IDベース、unordered_map管理）に切り替えた。
`PhotoGrid::populate()` のインターフェースも `PhotoLibrary&` → `PhotoProvider&` に変更。
PhotoLibrary.h/cppは削除済み。CMake GLOBのため、削除後は `cmake --preset macos` 再実行が必要。

### std::map と tc::map の衝突
JsonStorage.h で `using namespace std;` を使うと `tc::map`（TrussCのマップ関数）と衝突する。
→ サーバ側のJsonStorage.hでは `std::map`, `std::string` 等を明示的に使用。

### Crow CATCHALL_ROUTE の POST body 問題
初期の tcxHttp ラッパーは `CATCHALL_ROUTE` で全ルートを動的に処理していたが、POST body が空になるバグがあった。
→ tcxHttp を tcxCrow にリネームし、Crow の `CROW_ROUTE` マクロを直接使う薄いヘルパーレイヤーに変更。

### MCP テスト方法
macOS の bash は 3.x で `coproc` 未対応。zsh の coproc で試したが、MCP初期化ハンドシェイク（`initialize` メソッド）が必要な可能性があり、単純な tool call だけでは動かなかった。
→ **MCP テストは GUI から直接操作するか、初期化シーケンスを含むスクリプトを用意する必要がある**（要調査）

### NLOHMANN_DEFINE_TYPE_INTRUSIVE のフィールド追加
Photo構造体にフィールドを追加した場合、INTRUSIVE マクロのリストに追加しないとJSONシリアライズされない。
また、既存のlibrary.jsonにそのフィールドがないとパースエラーになる。
→ **フィールド追加時は既存のlibrary.jsonを削除してやり直す**（開発フェーズ）

### UploadQueue の sleep() デッドロック
`UploadQueue::threadedFunction()` 内で `lock_guard<mutex>` のスコープ内に POSIX `sleep(1000)` を書いてしまい、
mutex を保持したまま1000秒スリープ。メインスレッドの `draw()` → `getPendingCount()` が同じ mutex を取ろうとしてデッドロック → ビーチボール。
macOS の `sample` コマンドでコールスタックを取得して発見。
→ **sleep は必ずロック外で実行。`std::this_thread::sleep_for(std::chrono::seconds(N))` を使う。POSIX `sleep()` は秒単位なので注意。**

### RAW サムネイルの黒帯（センサーダークピクセル）
LibRaw の出力にセンサー端のダークピクセル（暗画素）が含まれ、サムネイルの右端と下端に黒い帯が出た。
`half_size=1` を外しても解消せず（センサー固有の問題）。
→ **`cropBlackBorders()` で自動検出・クロップ。行/列の平均輝度が閾値以下なら除去。**

### Image vs Pixels のスレッド安全性
`Image` クラスはGPUテクスチャを含むためメインスレッドでのみ使用可能。
バックグラウンドスレッドでサムネイルを読み込む場合は `Pixels`（CPUのみ）を使う。
→ PhotoProvider の getThumbnail() では `Pixels::load()` を使用

### Creative Style はXMPではなくMakerNoteにある
Sony ARW の Creative Style（Portrait, Standard, Vivid等）は XMP パケットには入っていない。
LibRaw の `imgdata.idata.xmpdata` にある XMP には Rating と DocumentID しか入っていなかった。
Creative Style は Sony MakerNote（バイナリ形式の EXIF タグ）に格納されている。
→ **libexiv2 を使って `Exif.Sony2.CreativeStyle` から取得する**。LibRaw 単体では取れない。

### exiv2 0.28.x の API 変更
exiv2 0.27 → 0.28 で `toLong()` が `toInt64()` に変更された。
→ brew で入る 0.28.7 に合わせてコード側を `toInt64()` にする。

### アドオン内に .git があると cmake --preset が壊れる
tcxLensfun に `git init` したところ、TrussPhoto の `cmake --preset macos` が
tcxLensfun の .git を見つけてそちらの CMakePresets.json を要求してエラーになった。
→ **アドオンに独自の .git がある場合、ダミーの CMakePresets.json を置く必要がある**
（macos, windows, linux, web プリセットを定義）

## カメラプロファイル＋レンズ補正パイプライン

### 処理順序（フルビュー表示時）
1. **LibRaw**: RAW → sRGB ピクセル（CPU）
2. **lensfun**: レンズ補正 - 歪曲・周辺光量・色収差（CPU、ピクセル書き換え）
3. **GPU upload**: テクスチャ作成（Immutable）
4. **LutShader**: カメラプロファイルLUT適用（GPU、リアルタイム）

- レンズ補正はCPUでピクセル変形するため、GPU upload前に実行
- LUTはGPUシェーダーでリアルタイム適用（ブレンド調整可能、Pキーでトグル）
- サムネイルにはLUT/レンズ補正は適用しない

### カメラプロファイルディレクトリ構造
```
~/.trussc/profiles/
  SONY_ILCE-7CM2/           ← カメラモデル名（スペース→アンダースコア）
    Portrait.cube            ← Creative Style名.cube
    Standard.cube
    Vivid.cube
    _default.cube            ← スタイル不明時のフォールバック
```
- CameraProfileManager がスキャンして自動マッチ
- プロファイル（.cube）はまだ未作成。dcamprof等で後から生成する
- プロファイルがなくても現在の動作を維持（LibRawデフォルト）

### キーバインド（シングルビュー）
| キー | 機能 |
|------|------|
| ESC | グリッドに戻る |
| ←→ | 前後の写真 |
| スクロール | ズーム |
| ドラッグ | パン |
| 0 | ズームリセット |
| P | カメラプロファイル ON/OFF |
| [ / ] | プロファイルブレンド量 ±10% |
| L | レンズ補正 ON/OFF（画像再読み込み） |

### メタデータ表示（シングルビュー オーバーレイ）
```
7C201750.ARW [RAW]
7008x4672  Zoom: 100%
ILCE-7CM2 | ZEISS Batis 2/40 CF @40mm f/2.0 ISO1000 | Portrait
Profile: ON 100%
```

### 現状のアドオン設計に関するメモ
- **tcxExiv2**: CMakeLists.txt のみ。exiv2 のリンク設定だけ。ラッパーコードなし。
  exiv2 の API が十分使いやすいので素通しで直接 `#include <exiv2/exiv2.hpp>` して使う。
- **tcxLensfun**: ラッパーあり（LensCorrector クラス）。lensfun C API を隠蔽。
  ただし**実際のレンズ補正出力は未検証**。bilinear サンプリング（bicubic の方がよいかも）。
  `lf_modifier_initialize` のパラメータ（distance, scale等）が適切かも要確認。
- **tcxLut**: GPU依存（sokol シェーダー）。ヘッドレス/サーバサイドでは使えない。
  将来サーバでLUT適用済みサムネ生成をやるなら CPU ベースの LUT 適用関数が必要。
- **アドオン増加の懸念**: brew前提のアドオン（exiv2, lensfun）が増えると環境構築が面倒。
  FetchContent 化が難しいもの（glib2依存など）もある。方針要検討。

## テスト用データ

```
/Users/toru/Nextcloud/Make/TrussC/testPictures/
├── 7C201750.ARW  (41MB)
├── 7C201751.ARW  (41MB)
├── ...
└── 7C201765.ARW  (38MB)   # 14枚のSONY ILCE-7CM2 ARW
```

## 今後の予定

### カメラプロファイル関連
- dcamprof でカメラプロファイル（.cube）を作成する
- tcxLensfun の実動作検証（補正が正しくかかるか、画質は十分か）
- tcxLut に CPU ベースの LUT 適用関数を追加（サーバサイド用）

### アドオン構成の見直し
- brew前提アドオンが増えている（exiv2, lensfun）。環境構築の手間とのトレードオフ
- exiv2 は PhotoProvider 内で直接使っているだけなので、アドオンにする必要があるか要検討
- lensfun も同様。最終的に使うかどうかの判断が先

### その他
- MCP初期化シーケンスの調査と自動テスト
- マルチパートファイルアップロード（ネットワーク越しの同期対応）
- EXIF dateTimeOriginal をIDに含める
- Emscripten Fetch API の実装（tcxCurl WASM対応）
- サーバURL設定のGUI（現在はsettings.json手動編集 or MCP `set_server`）
