# TrussPhoto プロジェクト固有設定

## 概要

TrussC ベースの写真管理・RAW現像アプリ。**1バイナリ2モード**設計。

- **GUIモード**（デフォルト）: デスクトップ写真ビューア + サーバ同期クライアント
- **サーバモード**（`--server`）: ヘッドレス REST API サーバ（Crow HTTP）

旧 TrussPhotoServer は別リポジトリに残存するが、機能は TrussPhoto に統合済み。

## ビルド方法

```bash
cd apps/TrussPhoto
cmake --preset macos
cmake --build build-macos
```

ソースファイルを追加/削除した場合は `cmake --preset macos` の再実行が必要（GLOB ベース）。

## 起動モード

```bash
# GUI モード（デフォルト）
./bin/TrussPhoto.app/Contents/MacOS/TrussPhoto

# サーバモード（ヘッドレス）
./bin/TrussPhoto.app/Contents/MacOS/TrussPhoto --server

# サーバモード + オプション
./bin/TrussPhoto.app/Contents/MacOS/TrussPhoto --server --port 8080 --library-dir /path/to/photos
```

### コマンドライン引数
| フラグ | 説明 | デフォルト |
|--------|------|-----------|
| `--server` | ヘッドレスサーバモード | GUI モード |
| `--port N` | サーバポート | 18730 |
| `--library-dir PATH` | ライブラリフォルダ上書き | settings.json の値 |

## アーキテクチャ

### ソース構成
```
src/
├── main.cpp           # エントリポイント（GUI / --server 分岐）
├── tcApp.h/cpp        # メインアプリ（起動フロー、イベント処理、モード分岐）
├── AppConfig.h        # コマンドライン引数パース（--server, --port, --library-dir）
├── AppPaths.h         # OS標準パス定義 + bin/data/ からの自動マイグレーション
├── Settings.h         # 設定永続化（settings.json → tpDataPath）
├── ServerConfig.h     # APIキー生成・永続化（サーバモード用）
├── PhotoServer.h      # Crow HTTPサーバ（PhotoProvider をREST公開）
├── PhotoEntry.h       # PhotoEntry構造体 + JSON シリアライズ
├── Database.h         # SQLite3 薄い RAII ラッパー
├── PhotoDatabase.h    # photos テーブル CRUD、スキーマ管理、JSON マイグレーション
├── PhotoProvider.h    # 写真管理の中核（ローカル + サーバ抽象化、SQLite永続化）
├── PhotoGrid.h        # スクロール可能なサムネイルグリッドUI
├── PhotoItem.h        # 個々の写真アイテム（サムネ + ラベル）
├── AsyncImageLoader.h # バックグラウンドサムネイルローダー
└── UploadQueue.h      # バックグラウンドアップロード（リトライ付き）
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

### 永続化ファイル

```
tpDataPath/                          ← ~/Library/Application Support/TrussPhoto/ (macOS)
├── settings.json                    # サーバURL、ライブラリフォルダ
├── library.db                       # SQLite - 全写真エントリ（即時書き込み）
└── server_config.json               # APIキー、ポート（サーバモード用）

tpCachePath/                         ← ~/Library/Caches/TrussPhoto/ (macOS)
└── thumbnail_cache/                 # サムネイル JPEG キャッシュ

bin/data/                            ← バンドルリソース（読み取り専用）
└── lensfun/                         # レンズ補正データベース

libraryFolder                        ← ユーザ設定（例: ~/Pictures/TrussPhoto/）
└── 2026/01/15/                      # RAW オリジナル（日付別）
    └── DSC00001.ARW
```

| OS | tpDataPath | tpCachePath |
|----|-----------|-------------|
| macOS | `~/Library/Application Support/TrussPhoto/` | `~/Library/Caches/TrussPhoto/` |
| Linux | `~/.local/share/TrussPhoto/` | `~/.cache/TrussPhoto/` |
| Windows | `%APPDATA%/TrussPhoto/` | `%LOCALAPPDATA%/TrussPhoto/` |

**bin/data/ からの自動マイグレーション**: 起動時に tpDataPath に library.db がなく bin/data/ にある場合、自動コピー。settings.json、thumbnail_cache も同様。元ファイルは安全のため残す。

## サーバAPI

| Method | Endpoint | 説明 |
|--------|----------|------|
| GET | /api/health | ヘルスチェック |
| GET | /api/photos | 写真一覧（id, filename, fileSize, camera, width, height） |
| GET | /api/photos/:id | 写真詳細 |
| GET | /api/photos/:id/thumbnail | サムネイルJPEG取得 |
| PATCH | /api/photos/:id/metadata | メタデータ更新（rating, colorLabel, flag, memo, tags） |
| POST | /api/import | RAWインポート `{"path": "/path/to/file.ARW"}` |
| DELETE | /api/photos/:id | 写真削除 |

サーバポート: **18730**（デフォルト、`--port` で変更可能）
認証: Bearer トークン（`/api/health` 以外すべて）。APIキーは `server_config.json` に自動生成。

## 使用アドオン

addons.make に記載（すべて統合バイナリに含まれる）:
- **tcxCurl** - HTTPクライアント（libcurl / Emscripten Fetch）
- **tcxCrow** - HTTPサーバ（Crow、--server モード用）
- **tcxLibRaw** - RAW画像デコード（LibRaw、FetchContent、OpenMP並列デモザイク）
- **tcxLut** - LUTカラーグレーディング（GPU シェーダー、header-only）

アドオン不使用（直接リンク）:
- **exiv2** - EXIF/MakerNote メタデータ（brew の dylib を local.cmake でリンク）
- **lensfun** - レンズ補正は自前実装（`LensCorrector.h`）。lensfun の XML DB のみ使用

### アドオンの Git 管理

アドオン（`TrussC/addons/tcx*`）は TrussC 本体リポジトリとは**別の独立した Git リポジトリ**で管理されている。
TrussC の `.gitignore` がアドオンディレクトリ配下の `.git` を無視するため、各アドオンは個別にコミット・プッシュが必要。

```
TrussC/                    ← TrussC 本体 (git repo)
├── trussc/                ← コアライブラリ
├── addons/
│   ├── tcxCurl/           ← 独立 git repo
│   ├── tcxLibRaw/         ← 独立 git repo
│   ├── tcxLut/            ← 独立 git repo
│   ├── tcxCrow/           ← 独立 git repo
│   └── ...               （他にも tcxBox2d, tcxOsc 等あり）
├── apps/
│   ├── TrussPhoto/        ← 独立 git repo (このプロジェクト)
│   └── TrussPhotoServer/  ← 独立 git repo
└── ...
```

**コミット時の注意:**
- アドオンを変更した場合、そのアドオンのディレクトリに `cd` してからコミットする
- TrussPhoto / TrussPhotoServer / 各アドオンのコミットは別々に行う
- TrussC 本体の `apps/` は `.gitignore` で除外されている

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

### PhotoEntry へのフィールド追加
PhotoEntry にフィールドを追加した場合:
1. `PhotoEntry.h` の構造体と `to_json`/`from_json` に追加
2. `PhotoDatabase.h` の CREATE TABLE、bindEntry()、loadAll() に追加
3. `PhotoDatabase::SCHEMA_VERSION` をインクリメントし、ALTER TABLE マイグレーションを追加
4. 開発中は `library.db` を削除して再作成するのが手っ取り早い

### リッチメタデータ（v2 スキーマ）
- `rating` (0-5), `colorLabel`, `flag` (-1/0/1), `memo`, `tags` (JSON配列文字列)
- 各フィールドに `xxxUpdatedAt` (ms epoch) を持つ（フィールド単位の同期用）
- setter は `PhotoProvider::setRating()` 等 → メモリ + DB + XMP サイドカー一括更新
- XMP サイドカー: インポート時に `.xmp` を読み、メタデータ変更時に書き出し
- サイドカー形式: Lightroom 互換（`DSC00001.xmp`）、darktable 形式（`.ARW.xmp`）も読み込み対応

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

## イベント駆動レンダリング（redraw）

### 省電力モード
`setIndependentFps(VSYNC, 0)` で update のみ回し、draw は `redraw()` 呼び出し時のみ実行。
バッテリー消費を大幅に削減できる。

### redraw() が必要な箇所
表示内容が変わる全ての箇所で `redraw()` を呼ぶ必要がある:

| 箇所 | トリガー |
|------|----------|
| `keyPressed()` | 全てのキー入力後 |
| `mouseReleased()` | ドラッグ終了時 |
| `mouseDragged()` | パン操作中 |
| `mouseScrolled()` | ズーム操作 |
| `windowResized()` | ウィンドウサイズ変更 |
| `filesDropped()` | フォルダドロップ後 |
| `showFullImage()` | 画像ロード完了時 |
| `exitFullImage()` | グリッドに戻る時 |
| `update()` 内 sync 完了 | グリッド再構築時 |
| `update()` 内 syncState 変更 | バッジ色更新時（変更があった場合のみ） |
| `update()` 内 RAW ロード完了 | フルサイズ表示切替時 |
| `repairLibrary()` | グリッド再構築後 |

### 変更検知パターン
無駄な redraw を避けるため、変更があった場合のみ redraw する:
```cpp
// PhotoGrid::updateSyncStates() は変更があれば true を返す
if (grid_ && grid_->updateSyncStates(provider_)) {
    redraw();
}
```

## カメラプロファイル＋レンズ補正パイプライン

### 処理順序（フルビュー表示時）

**プログレッシブロード方式を採用:**
1. **即時表示（~100ms）**: `half_size=1` でプレビューをロード → `previewTexture_` に表示
2. **バックグラウンド**: フルサイズRAWをデコード + レンズ補正 → `rawPixels_` / `fullPixels_`
3. **完了時**: `fullTexture_` に差し替え、`previewTexture_` を破棄

**フルサイズパイプライン:**
1. **LibRaw**: RAW → sRGB ピクセル（CPU、OpenMP並列デモザイク）
2. **lensfun**: レンズ補正 - 歪曲・周辺光量・色収差（CPU、ピクセル書き換え）
3. **GPU upload**: テクスチャ作成（Immutable）
4. **LutShader**: カメラプロファイルLUT適用（GPU、リアルタイム）

**設計ポイント:**
- `rawLoadTargetIndex_` で「どの画像のロードか」を追跡。ユーザーが別画像に移動したら結果を破棄
- レンズ補正はフルサイズのみ。プレビューには適用しない（速度優先）
- LUTはGPUシェーダーなのでプレビュー/フルサイズ両方に適用される
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
| Z | ズームリセット |
| 0-5 | レーティング設定（0=解除, 1-5=★） |
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

### ビルド依存関係の整理
- **アドオン（addons.make）**: tcxCurl, tcxCrow, tcxLibRaw, tcxLut
- **local.cmake**: exiv2 のみ。brew 前提の唯一の外部依存
- **src/LensCorrector.h**: lensfun ライブラリ非依存の自前実装。
  lensfun の XML データベース（`bin/data/lensfun/`）を pugixml でパースし、
  PTLens歪曲補正・PA周辺光量補正・poly3色収差補正を自前で実装。
  **実際のレンズ補正出力は未検証**（要テスト）。bilinear サンプリング（bicubic の方がよいかも）。
- **tcxLut**: GPU依存（sokol シェーダー）。ヘッドレス/サーバサイドでは使えない。
  将来サーバでLUT適用済みサムネ生成をやるなら CPU ベースの LUT 適用関数が必要。
- **WASM非対応**: exiv2, LibRaw のファイルI/O依存、ローカルファイルシステム前提の設計により、
  TrussPhoto はデスクトップ専用。WASM版は別アーキテクチャが必要。

### exiv2 の配布時の課題
- 現在 exiv2 は **動的リンク**（`/opt/homebrew/opt/exiv2/lib/libexiv2.28.dylib`）
- 配布先にも brew の exiv2 が必要。配布時の対策：
  - `install_name_tool` で rpath を書き換えて `.app/Contents/Frameworks/` に dylib 同梱
  - または exiv2 をソースから静的ビルド（FetchContent 化）
  - 開発中は brew の dylib で問題なし。配布フェーズで対応する

## テスト用データ

```
/Users/toru/Nextcloud/Make/TrussC/testPictures/
├── 7C201750.ARW  (41MB)
├── 7C201751.ARW  (41MB)
├── ...
└── 7C201765.ARW  (38MB)   # 14枚のSONY ILCE-7CM2 ARW
```

## 今後の予定

### レンズ補正（要検証）
- **自前実装済み**だが、実際の補正出力は未検証
- テスト方法: アプリ起動 → RAW ドロップ → シングルビュー → L キーで ON/OFF 比較
- 確認ポイント: 歪曲が補正されているか、周辺光量落ちが改善されるか、色収差が減るか
- 問題があれば: 座標正規化（rNorm）、PTLens 数式の解釈、補間ロジックを疑う

### カメラプロファイル関連
- dcamprof でカメラプロファイル（.cube）を作成する
- tcxLut に CPU ベースの LUT 適用関数を追加（サーバサイド用）

### その他
- MCP初期化シーケンスの調査と自動テスト
- マルチパートファイルアップロード（ネットワーク越しの同期対応）
- EXIF dateTimeOriginal をIDに含める
- サーバURL設定のGUI（現在はsettings.json手動編集 or MCP `set_server`）
