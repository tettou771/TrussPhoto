# TrussPhoto クラウドアーキテクチャ設計

> **Note (2026-02):** このドキュメントの Phase 1（サーバ基盤、基本API、JsonStorage）は実装済み。
> Phase 2 以降の計画（縮小DNG、編集パラメータ同期、書き出し等）は一部方針変更あり。
> 今後の方向性は [VISION.md](VISION.md) を参照。

## 次にやること (2026-02-03)

1. **TrussPhoto Server プロジェクト作成**
   - `apps/TrussPhotoServer/` を作成
   - addons.make: tcxHttp, tcxLibRaw, tcxLut
   - projectGenerator --update で生成

2. **基本API実装**
   - GET /api/photos - 一覧（まずはダミーデータ）
   - GET /api/photos/:id/thumbnail - サムネイル返却
   - curl でテスト

3. **JsonStorage 実装**
   - library.json の読み書き
   - Photo 構造体定義

---

## コンセプト

**クラウドファースト**のRAW写真管理システム。

- サーバーがマスター、クライアントはキャッシュ
- 縮小DNGで編集、書き出し時のみフル解像度
- ローカルストレージ節約 + 複数デバイス対応
- 完全に独立したシステム（外部サービス非依存）

---

## システム構成

```
┌─────────────────────────────────────────────────────────┐
│               TrussPhoto Server                          │
│                                                          │
│  ┌───────────────────────────────────────────────────┐  │
│  │  ストレージ                                        │  │
│  │  /data/photos/                                    │  │
│  │  ├── originals/     ← オリジナルRAW               │  │
│  │  ├── previews/      ← 縮小DNG (~2560px)          │  │
│  │  ├── thumbnails/    ← サムネイル (~512px)         │  │
│  │  └── exports/       ← 書き出し済みファイル         │  │
│  └───────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────┐  │
│  │  コア機能                                          │  │
│  │  - フォルダ監視・自動インポート                     │  │
│  │  - 縮小DNG/サムネイル生成                          │  │
│  │  - メタデータ管理 (インメモリ + JSON永続化)         │  │
│  │  - カメラプロファイル管理                          │  │
│  │  - RAW現像エンジン                                │  │
│  │  - 書き出し処理                                   │  │
│  └───────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────┐  │
│  │  API                                              │  │
│  │  - REST API (HTTP/2)                             │  │
│  │  - WebSocket (リアルタイム通知)                    │  │
│  │  - gRPC (オプション、高速転送用)                   │  │
│  └───────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
                          ↑
                   高速API (HTTP/2)
                          ↓
┌─────────────────────────────────────────────────────────┐
│               TrussPhoto Client                          │
│                                                          │
│  macOS / iOS / Web                                      │
│  - サムネイルグリッド表示                                │
│  - 縮小DNGで編集（非破壊）                               │
│  - 編集パラメータはサーバーに同期                         │
│  - 書き出しリクエスト                                    │
│                                                          │
│  ローカルキャッシュ:                                     │
│  ~/Library/Caches/TrussPhoto/                           │
│  ├── thumbnails/    ← 閲覧した画像                      │
│  └── previews/      ← 編集中の画像                      │
└─────────────────────────────────────────────────────────┘
```

---

## データフロー

### 1. インポート

```
RAWファイルをサーバーにアップロード（または監視フォルダに配置）
    ↓
メタデータ抽出（EXIF: カメラ, レンズ, 日時, GPS, CreativeStyle等）
    ↓
サムネイル生成（512px JPEG, ~50KB）
    ↓
縮小DNG生成（2560px, Lossy DNG, ~2-3MB）
    ↓
DB登録
    ↓
クライアントに通知（WebSocket）
```

### 2. ブラウズ

```
グリッド表示リクエスト
    ↓
サーバーからサムネイル一覧取得（ページネーション）
    ↓
ローカルキャッシュに保存
    ↓
表示
```

### 3. 編集

```
画像選択
    ↓
縮小DNGをダウンロード（未キャッシュの場合、~2-3MB）
    ↓
縮小DNGで編集（露出, WB, トーンカーブ, カラー等）
    ↓
編集パラメータをサーバーに同期
    ↓
[オプション] 「フルRAWで確認」→ オリジナルRAWダウンロード
```

### 4. 書き出し

```
書き出しリクエスト（クライアント → サーバー）
    ↓
サーバーがフルRAWを読み込み
    ↓
カメラプロファイル + 編集パラメータを適用
    ↓
JPEG/TIFF/PNG生成
    ↓
クライアントにダウンロード or サーバー上に保存
```

---

## 縮小DNG

### 仕様

| 項目 | 値 |
|------|-----|
| 解像度 | 長辺 2560px |
| 形式 | DNG (Lossy) |
| サイズ | 約 2-3MB/枚 |
| RAWデータ | 含む（非破壊編集可能） |

### 生成方法

**方法1: Adobe DNG Converter CLI**
```bash
Adobe\ DNG\ Converter -lossy -side 2560 -d output/ input.ARW
```

**方法2: LibRaw + 自前DNG書き出し**
- Adobe SDK不要
- より柔軟な制御が可能
- 実装コストあり

### 制限

- フル解像度よりダイナミックレンジが狭い
- 大きなクロップで画質劣化
- 最終品質にはオリジナルRAW必須

---

## API設計

### エンドポイント

```
# 認証
POST /api/v1/auth/login
POST /api/v1/auth/refresh

# ライブラリ
GET    /api/v1/photos                    # 一覧（フィルタ、ソート、ページネーション）
GET    /api/v1/photos/:id                # 詳細
POST   /api/v1/photos                    # アップロード
DELETE /api/v1/photos/:id                # 削除

# アセット
GET    /api/v1/photos/:id/thumbnail      # サムネイル (JPEG)
GET    /api/v1/photos/:id/preview        # 縮小DNG
GET    /api/v1/photos/:id/original       # フルRAW

# 編集
GET    /api/v1/photos/:id/edits          # 編集パラメータ取得
PUT    /api/v1/photos/:id/edits          # 編集パラメータ保存
POST   /api/v1/photos/:id/edits/reset    # 編集リセット

# 書き出し
POST   /api/v1/photos/:id/export         # 書き出しリクエスト
GET    /api/v1/exports/:id               # 書き出しステータス
GET    /api/v1/exports/:id/download      # 書き出しダウンロード

# アルバム/コレクション
GET    /api/v1/albums
POST   /api/v1/albums
PUT    /api/v1/albums/:id
DELETE /api/v1/albums/:id

# カメラプロファイル
GET    /api/v1/profiles                  # プロファイル一覧
POST   /api/v1/profiles                  # プロファイル追加

# リアルタイム
WS     /api/v1/events                    # イベントストリーム
```

### 認証

| 環境 | 方式 |
|------|------|
| ローカル | APIキー（シンプル） |
| インターネット | JWT + リフレッシュトークン |

---

## 共有機能（将来）

### Phase 1: リンク共有
- 写真/アルバムに対して共有リンク生成
- オプションでパスワード保護
- 有効期限設定可能

```
GET /share/:token              # 共有ページ
GET /share/:token/download     # ダウンロード
```

### Phase 2: マルチユーザー（必要になったら）
- ユーザー登録/管理
- アクセス権限（閲覧のみ / 編集可）
- 共同アルバム

---

## 技術スタック

### 統一アーキテクチャ: TrussCベース

**サーバーもクライアントもTrussC**で統一する。

```
┌─────────────────────────────────────────────────────────┐
│                    共有 addons                           │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────────────┐│
│  │ tcxLibRaw   │ │ tcxLut      │ │ tcxPhotoCore        ││
│  │ RAW読み込み  │ │ LUT適用     │ │ 現像ロジック、DB等   ││
│  └─────────────┘ └─────────────┘ └─────────────────────┘│
└─────────────────────────────────────────────────────────┘
         ↑                                    ↑
         │                                    │
┌────────┴────────┐                ┌─────────┴─────────┐
│ TrussPhoto      │                │ TrussPhoto        │
│ Server          │                │ Client            │
│ (headless)      │                │ (GUI)             │
│                 │                │                   │
│ + tcxHttp       │                │ + UI              │
│   HTTPサーバー   │                │                   │
└─────────────────┘                └───────────────────┘
```

### サーバー

| コンポーネント | 技術 |
|--------------|------|
| ベース | TrussC (ヘッドレスモード) |
| HTTP | tcxHttp addon (Crow or Drogon統合) |
| データ | インメモリ + JSON永続化 |
| RAW処理 | tcxLibRaw (既存) |
| 色補正 | tcxLut (既存) |
| 共通ロジック | tcxPhotoCore (新規) |

### クライアント

| プラットフォーム | 技術 |
|----------------|------|
| macOS / Windows / Linux | TrussC (ネイティブ) |
| iOS | TrussC + SwiftUI |
| Web | TrussC (WASM) |

### メリット

- **同じビルドシステム**: CMake + addons.make
- **コード共有**: 現像ロジック、DB操作、モデル定義
- **学習コストゼロ**: TrussCの知識がそのまま活きる
- **一貫性**: サーバーとクライアントで動作が完全一致

### tcxHttp addon 構想

```cpp
#include <tcxHttp.h>

// サーバー定義
http::Server server;
server.port(8080);

// エンドポイント
server.get("/api/photos", [&](http::Request& req, http::Response& res) {
    auto photos = photoCore.listPhotos(req.query("limit", 100));
    res.json(photos);
});

server.get("/api/photos/:id/thumbnail", [&](http::Request& req, http::Response& res) {
    auto id = req.param("id");
    auto path = photoCore.getThumbnailPath(id);
    res.sendFile(path, "image/jpeg");
});

server.put("/api/photos/:id/edits", [&](http::Request& req, http::Response& res) {
    auto id = req.param("id");
    auto edits = req.json<EditParams>();
    photoCore.saveEdits(id, edits);
    res.ok();
});

// 起動（ヘッドレスなのでブロック）
server.run();
```

---

## ストレージ設計

### ディレクトリ構造

```
/data/trusphoto/
├── originals/
│   ├── 2024/
│   │   ├── 01/
│   │   │   ├── 15/
│   │   │   │   ├── {uuid}.arw
│   │   │   │   └── ...
│   │   │   └── ...
│   │   └── ...
│   └── ...
├── previews/
│   └── {uuid}.dng
├── thumbnails/
│   └── {uuid}.jpg
├── exports/
│   └── {uuid}/
│       └── {filename}.jpg
├── profiles/
│   └── Sony_ILCE-7CM2_Portrait.dcp
├── library.json          ← メインデータ
└── library.json.backup   ← バックアップ
```

### データストレージ: インメモリ + JSON永続化

**方針: シンプルに始める。問題が出たら対処する。**

```
起動時:
  library.json 読み込み → vector<Photo> に展開（メモリ）

変更時:
  メモリ更新 → 非同期で library.json に保存

定期保存:
  30秒ごと or 変更10件ごと（クラッシュ対策）

シャットダウン時:
  最終保存
```

**データサイズ見積もり:**
```
1枚あたり: ~3KB（パス、EXIF、編集パラメータ等）
1万枚: 30MB
10万枚: 300MB
→ 現代のマシンなら余裕でメモリに載る
```

**メリット:**
- 超シンプル、依存ゼロ
- デバッグ楽（JSONファイル見ればわかる）
- バックアップ楽（ファイルコピー）
- 検索も速い（メモリ上）

**抽象化で将来に備える:**

```cpp
// ストレージ抽象化
class PhotoStorage {
public:
    virtual ~PhotoStorage() = default;
    virtual vector<Photo> loadAll() = 0;
    virtual void save(const Photo& photo) = 0;
    virtual void saveAll() = 0;
    virtual vector<Photo> query(const PhotoQuery& q) = 0;
};

// 最初はこれで十分
class JsonStorage : public PhotoStorage {
    vector<Photo> photos_;  // メモリ上
    fs::path filePath_;

    vector<Photo> loadAll() override {
        // JSONファイル読み込み → photos_ に展開
    }

    void saveAll() override {
        // photos_ → JSONファイル書き出し
    }

    vector<Photo> query(const PhotoQuery& q) override {
        // メモリ上でフィルタリング（STL algorithm）
    }
};

// 必要になったら追加（同じインターフェース）
class SqliteStorage : public PhotoStorage { ... };
```

### ファイルストレージ（将来の拡張）

```cpp
// ファイル（RAW, サムネイル等）のストレージ抽象化
class FileStorage {
public:
    virtual void put(const string& key, const vector<uint8_t>& data) = 0;
    virtual vector<uint8_t> get(const string& key) = 0;
    virtual void remove(const string& key) = 0;
    virtual vector<string> list(const string& prefix) = 0;
};

// 実装
class LocalFileStorage : public FileStorage { ... };  // ローカルFS
class S3Storage : public FileStorage { ... };         // S3互換（将来）
```

---

## 開発フェーズ

### Phase 0: 検証 ✅ 完了
- [x] LibRaw統合（tcxLibRaw）
- [x] tcxHttp addon 作成・動作確認（Crow + ASIO）
- [ ] 縮小DNG生成の検証

### Phase 1: サーバー基盤 ← 次はここから
- [ ] **TrussPhoto Server プロジェクト作成**
  - apps/TrussPhotoServer/ を作成
  - addons: tcxHttp, tcxLibRaw, tcxLut
  - ヘッドレス or デバッグ用GUI両対応
- [ ] 基本API実装
  - GET /api/photos - 写真一覧
  - GET /api/photos/:id/thumbnail - サムネイル
  - POST /api/photos - アップロード
- [ ] PhotoStorage (JsonStorage) 実装
- [ ] サムネイル生成（LibRaw使用）
- [ ] ファイル監視・自動インポート

### Phase 1.5: クライアント連携
- [ ] TrussPhoto Client をサーバー対応に改修
- [ ] PhotoLoader 実装（URL/ID主体）
- [ ] サムネイルキャッシュ

### Phase 2: クライアント統合
- [ ] TrussPhoto Client → Server API接続
- [ ] サムネイルグリッド（サーバー経由）
- [ ] 縮小DNGダウンロード・キャッシュ

### Phase 3: 編集機能
- [ ] 編集パラメータフォーマット定義
- [ ] 縮小DNGでの編集プレビュー
- [ ] パラメータ同期
- [ ] フルRAWダウンロード

### Phase 4: カメラプロファイル
- [ ] DCP読み込み実装
- [ ] EXIF → プロファイル自動選択
- [ ] プロファイル適用

### Phase 5: 書き出し
- [ ] サーバーサイドRAW現像
- [ ] JPEG/TIFF書き出し
- [ ] バッチ書き出し

### Phase 6: 共有
- [ ] リンク共有
- [ ] パスワード保護
- [ ] 有効期限

### Phase 7: マルチプラットフォーム
- [ ] iOS クライアント
- [ ] Web クライアント
- [ ] 編集競合解決

---

## オープンな疑問

1. **編集パラメータ形式**
   - XMP（Adobe互換、他ツールで読める）
   - 独自JSON（シンプル、完全制御）
   - → まずはJSONで、XMPエクスポートは後から？

2. **オフライン編集の競合**
   - 縮小DNGがあれば編集可能
   - オンライン復帰時に同期
   - → ロック機構で基本回避、競合時は後勝ち or 選択

3. **バックアップ**
   - サーバーのバックアップは別途考える
   - rsync / rclone / borg 等
   - → library.json + originals/ をバックアップすれば復元可能

4. **モバイルからのインポート**
   - iPhone/Androidで撮影 → 直接アップロード?
   - 別アプリ（写真アプリ）からの共有?
   - → Web版経由でアップロード？専用アプリ？

5. **スケーラビリティ**
   - 10万枚超えたらどうする？
   - → JsonStorage → SqliteStorage に切り替え可能な設計

---

## 開発方針: WASM-First

### 概要

**Web版 (WASM) を主な開発対象**とし、ネイティブ版は「おまけ」として自動的に完成させる。

```
TrussPhoto Client (C++ / TrussC)
     │
     ├── Emscripten ──→ WASM (Web版) ← 主な開発対象
     │                   - ブラウザで動作確認
     │                   - DevToolsでデバッグ
     │                   - リロードが速い
     │
     └── clang/gcc ──→ ネイティブ版 ← ビルドするだけで完成
                        - macOS / iOS / Windows / Linux
                        - 追加のRAW処理能力
                        - ローカルキャッシュが永続的
```

### Web版 vs ネイティブ版の棲み分け

| | Web版 (WASM) | ネイティブ版 |
|---|---|---|
| インストール | 不要 | 必要 |
| キャッシュ | 一時的 (IndexedDB) | 永続的 |
| RAW処理 | サーバー依存 | ローカル可能 |
| インポート | オミット or サーバー直送 | フル対応（ローカル先行） |
| オフライン | 閲覧のみ（キャッシュ分） | フル対応 |
| 向いてる用途 | 出先で確認、軽い編集 | メイン作業環境 |
| 開発時 | コア部分の開発 | フル機能版 |

**方針:** ネイティブ版がメイン。WASM版は閲覧・編集に特化し、重い処理はオミット可。

---

## リソース管理: URL/ID主体

### 設計思想

ファイルパスではなく、**URL/ID主体**でリソースを管理する。

```
旧: ファイルパス主体
    /Users/toru/Photos/DSC00001.ARW

新: ID主体
    photos/abc123/thumbnail
    photos/abc123/preview
    photos/abc123/original
```

ローカルキャッシュは「URLのキャッシュ」として透過的に扱う。

### メリット

- **WASM互換**: fetch APIで並列読み込み可能、スレッド不要
- **ネイティブ/WASM共通コード**: 同じAPIで両方動く
- **キャッシュ透過**: ローカル/リモートを意識しない

### PhotoLoader API

```cpp
// 統一されたフォトID（ファイルパスではない）
using PhotoId = string; // "abc123" or "local-550e8400..."

class PhotoLoader {
    // キャッシュにあれば即返却、なければfetch
    void loadThumbnail(PhotoId id, function<void(Image&)> onLoaded);
    void loadPreview(PhotoId id, function<void(Pixels&)> onLoaded);
    void loadOriginal(PhotoId id, function<void(Pixels&)> onLoaded);

    // 複数同時（並列fetch）
    void loadThumbnails(vector<PhotoId> ids, function<void(PhotoId, Image&)> onEach);
};
```

### ローカルにしかないファイルの扱い

インポート直後、まだサーバーにアップロードされていない写真の管理：

```
インポートフロー:
1. ローカルで仮ID (UUID) を生成 → "local-550e8400-e29b-..."
2. ローカルでサムネイル生成 → キャッシュに保存
3. UIに即表示（仮IDで参照）
4. バックグラウンドでアップロード開始
5. サーバーが正式ID発行 → "abc123"
6. マッピング更新: local-550e8400... → abc123
7. 状態が ready に変更
```

### 写真の状態管理

```
pending_upload   ローカルのみ、アップロード待ち（仮ID）
                 → 閲覧可能、編集不可

uploading        アップロード中
                 → 閲覧可能、編集不可

processing       サーバーで縮小DNG生成中
                 → 閲覧可能、編集不可

ready            完全同期済み（正式ID）
                 → 閲覧可能、編集可能

upload_failed    アップロード失敗（リトライ可）
                 → 閲覧可能、編集不可
```

仮IDでも正式IDでも `PhotoLoader` が内部で解決するため、
呼び出し側は気にする必要がない。

---

## サムネイル転送の効率化

### 問題

Web版で毎回サムネイルをダウンロードすると遅い（特に大量の写真）

### 解決策

**案1: LZ4圧縮バンドル（推奨）**
```
GET /api/v1/photos/thumbnails?ids=1,2,3,...,100

Response: thumbnails.tar.lz4
├── 1.jpg
├── 2.jpg
└── ...
```
- 100枚まとめて圧縮転送
- クライアントで展開してIndexedDBにキャッシュ
- 実装シンプル

**案2: スプライトシート**
```
GET /api/v1/photos/thumbnails/sprite?ids=1,2,3,...,100

Response:
├── sprite.jpg (1枚の大きな画像)
└── coordinates.json (各サムネイルの座標)
```
- 1リクエストで完了
- JPEGの圧縮効率が良い
- CSS/Canvasで切り出し

**案3: HTTP/2多重化**
- 普通に100リクエスト並列
- HTTP/2なら多重化される
- 実装が最もシンプル
- 小規模なら十分

---

## インポートフロー

### ネイティブ版: ローカル先行処理

```
1. ユーザーがRAWファイルを選択/ドロップ
       ↓
2. ローカルでサムネイル生成（LibRaw）
       ↓
3. 即座にUIに表示 ← ユーザー体験が速い
       ↓
4. バックグラウンドでサーバーにアップロード
       ↓
5. サーバーが縮小DNG生成
       ↓
6. 完了通知（編集可能に）
```

**メリット:**
- 即座に結果が見える
- オフラインでも写真確認可能
- アップロードは裏で進む

### Web版: サーバー主体

```
1. ユーザーがRAWファイルを選択
       ↓
2. サーバーにアップロード開始
       ↓
3. 「アップロード中... 30%」表示
       ↓
4. サーバーがサムネイル/縮小DNG生成
       ↓
5. サムネイルをダウンロード
       ↓
6. UIに表示
```

**理由:**
- ブラウザでLibRaw (WASM) は動くが遅い
- 50MBのRAWをメモリに展開するのは厳しい
- サーバーに任せた方が現実的

### 整合性管理

```
写真の状態:
- pending_upload: ローカルのみ、アップロード待ち
- uploading: アップロード中
- processing: サーバーで処理中
- ready: 完全同期済み
- upload_failed: アップロード失敗（リトライ可）
```

ネイティブ版では `pending_upload` 状態でもサムネイル表示・閲覧可能。
編集は `ready` になってから（縮小DNGが必要なため）。

---

## 編集パラメータの同期

### 基本フロー

```
マシンA: 写真を編集
    ↓
編集パラメータをサーバーに保存（自動 or 手動）
    ↓
マシンB: 同じ写真を開く
    ↓
サーバーから編集パラメータを取得
    ↓
続きを編集
```

### 保存タイミング

- **自動保存**: 編集操作後、数秒のデバウンス
- **手動保存**: 明示的な「保存」操作
- **終了時保存**: アプリ終了/写真切り替え時

### 競合解決: 編集ロック

同時に複数デバイスで編集した場合の対策。

```
┌─────────┐         ┌─────────┐         ┌─────────┐
│ Client A│         │ Server  │         │ Client B│
└────┬────┘         └────┬────┘         └────┬────┘
     │                   │                   │
     │ 編集開始           │                   │
     │ ──────────────→   │                   │
     │   lock(photo_id)  │                   │
     │ ←──────────────   │                   │
     │   OK, lock取得    │                   │
     │                   │                   │
     │                   │   編集開始         │
     │                   │ ←────────────────│
     │                   │   lock(photo_id)  │
     │                   │ ────────────────→│
     │                   │   LOCKED by A     │
     │                   │                   │
     │                   │   「他で編集中」表示│
     │                   │   閲覧は可能       │
```

### ロックの仕様

```
Lock情報:
- photo_id: 対象写真
- device_id: ロックを持つデバイス
- user_agent: デバイス名（"MacBook Pro" 等）
- acquired_at: 取得時刻
- expires_at: 有効期限（30分等、自動延長）
```

**ロック中に他のデバイスができること:**
- 閲覧: 可能
- 編集: 警告表示、「強制編集」で奪取可能
- 強制編集: ロックを奪う（元のデバイスには通知）

**ロックの自動解除:**
- 有効期限切れ（ハートビート途絶）
- 明示的な編集終了
- アプリ終了時

### 編集パラメータ形式

```json
{
  "version": 1,
  "photo_id": "abc123",
  "updated_at": "2024-01-15T10:30:00Z",
  "params": {
    "exposure": 0.5,
    "contrast": 10,
    "highlights": -20,
    "shadows": 30,
    "whites": 0,
    "blacks": -5,
    "temperature": 5500,
    "tint": 0,
    "vibrance": 15,
    "saturation": 0,
    "tone_curve": [[0,0], [64,60], [192,200], [255,255]],
    "crop": {"x": 0, "y": 0, "w": 1, "h": 1, "angle": 0},
    "profile_id": "Sony_ILCE-7CM2_Portrait"
  }
}
```

XMP互換も検討可能だが、まずは独自JSONでシンプルに。

---

## カメラプロファイル管理

### 配置場所

プロファイルは**サーバーがマスター**として管理。

```
/data/trusphoto/profiles/
├── Sony_ILCE-7CM2_Standard.dcp
├── Sony_ILCE-7CM2_Portrait.dcp
├── Sony_ILCE-7CM2_Vivid.dcp
├── SIGMA_BF_Standard.dcp
└── ...
```

### 更新フロー

```
1. プロファイル作成（dcamprof + ColorChecker）
     ↓
2. サーバーの profiles/ にファイル配置
   - 手動コピー
   - または git push → サーバーで git pull
     ↓
3. サーバーにリロード指示
   POST /api/profiles/reload
   （または自動検知でホットリロード）
     ↓
4. クライアントがプロファイル一覧を再取得
   GET /api/profiles
     ↓
5. 新しいプロファイルが選択可能に
```

**アプリ再ビルド不要。** プロファイルはデータとして動的にロード。

### クライアントでのプロファイル利用

```
写真を開く / プロファイル選択時:
  1. EXIF から推奨プロファイルを判定
     (カメラ: ILCE-7CM2, CreativeStyle: Portrait)
       → Sony_ILCE-7CM2_Portrait.dcp
  2. ローカルキャッシュにある？
     - あれば使用
     - なければサーバーからダウンロード → キャッシュ
  3. プロファイルを適用してプレビュー表示
```

### プロファイルキャッシュ

```
~/Library/Caches/TrussPhoto/profiles/
├── Sony_ILCE-7CM2_Standard.dcp
├── Sony_ILCE-7CM2_Portrait.dcp
└── ...
```

- サーバーのバージョン（ハッシュ or 更新日時）と比較
- 古ければ再ダウンロード
- 「プロファイル更新」ボタンで強制更新も可能

### API

```
GET  /api/profiles                    # 一覧（名前、カメラ、更新日時）
GET  /api/profiles/:name              # ダウンロード
POST /api/profiles/reload             # サーバー側リロード
```

---

## 参考

- Adobe DNG Specification: https://helpx.adobe.com/camera-raw/digital-negative.html
- LibRaw: https://www.libraw.org/
- Crow (C++ HTTP Framework): https://crowcpp.org/
- Drogon (C++ HTTP Framework): https://github.com/drogonframework/drogon
- dcamprof (Camera Profiling): http://rawtherapee.com/mirror/dcamprof/camera-profiling.html
- LZ4: https://github.com/lz4/lz4
