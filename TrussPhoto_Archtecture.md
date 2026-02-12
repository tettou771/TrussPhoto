# TrussPhoto アーキテクチャ設計書

> **Note (2026-02):** これは初期の全体設計構想。WebDAV連携、JPEG XLプレビュー、
> Lightroom連携、tcxRivet 等の構想を含むが、実装の過程で方針が変更された部分がある。
> 現在の方向性は [VISION.md](VISION.md) を参照。
> 実装済みのクライアント・サーバアーキテクチャは [ARCHITECTURE_CLOUD.md](ARCHITECTURE_CLOUD.md) を参照。

## 概要

TrussPhotoは、TrussCフレームワーク上に構築する写真管理・RAW現像アプリケーション。
Lightroom Classicとの併用を前提とし、WebDAVベースのストレージ連携によりNextcloudを含む様々なサービスに対応する。

## 技術スタック

| 要素 | 技術 | 備考 |
|------|------|------|
| フレームワーク | TrussC (sokol) | 自作フレームワーク |
| UIライブラリ | tcxRivet | TrussC用UIアドオン（新規作成） |
| RAW読み込み | LibRaw | C API |
| レンズ補正 | Lensfun | C API、主要レンズカバー |
| メタデータ | Exiftool or libexif | EXIF/XMP読み取り |
| 顔検出/認識 | InsightFace (ONNX Runtime) | embedding + クラスタリング |
| AIタグ | CLIP (ONNX Runtime) | シーン・被写体分類 |
| プレビュー形式 | JPEG XL (libjxl) | 12bit、高階調 |
| DB | SQLite | ローカルカタログ |
| ストレージ連携 | WebDAV (libcurl) | Nextcloud等 |

---

## ファイル構成

### ストレージ上のフォルダ構造

```
[WebDAVルート]
├── raw/                          ← RAWルート（ユーザーが指定）
│   └── YYYY/
│       └── MM/
│           └── DD/
│               ├── DSC00123.ARW
│               ├── DSC00124.ARW
│               └── ...
│
└── preview/                      ← プレビュールート（ユーザーが指定）
    └── YYYY/
        └── MM/
            └── DD/
                ├── DSC00123_preview_2560.jxl
                ├── DSC00123_thumb_256.jxl
                ├── DSC00124_preview_2560.jxl
                ├── DSC00124_thumb_256.jxl
                └── ...
```

### 命名規則

RAWファイル `{name}.{ext}` に対して：

- スマートプレビュー: `{name}_preview_2560.jxl`（長辺2560px、12bit JPEG XL）
- サムネイル: `{name}_thumb_256.jxl`（長辺256px、12bit JPEG XL）

### RAWとプレビューの分離理由

- RAWフォルダはLightroom Classicと共有可能（LRの管理下にあるフォルダをそのまま指定）
- プレビューフォルダはTrussPhoto専用
- LRのカタログやファイルに一切干渉しない

---

## プレビュー体系

### 3段階のファイルアクセス

| 段階 | 用途 | 解像度 | 形式 | ビット深度 | 1枚サイズ | 10万枚 |
|------|------|--------|------|-----------|----------|--------|
| サムネイル | ブラウズ、グリッド表示 | 256px | JPEG XL | 12bit | ~20-40KB | ~3-4GB |
| スマートプレビュー | 単一表示、軽い確認 | 2560px | JPEG XL | 12bit | ~1-2MB (d=2.0) | ~100-200GB |
| RAWオリジナル | 本格編集 | フル | ARW等 | 14bit | ~30-40MB | ~4TB |

### プレビューの管理ポリシー

- **サムネイル**
  - 常にローカルにミラーする（自動ダウンロード）
  - 削除しない
  - 全体で数GB程度なのでストレージ負担なし

- **スマートプレビュー**
  - デフォルトではサーバー上に保持
  - 必要に応じてローカルにダウンロード
  - 明示的に削除可能（サーバーからもローカルからも）
  - ローカルキャッシュは容量上限を設定可能

- **RAWオリジナル**
  - 常にサーバー上に保持
  - 編集時のみオンデマンドでダウンロード
  - 編集完了後はローカルから削除可能

### プレビュー生成

- **ローカル生成**：RAWをダウンロード → プレビュー生成 → サーバーにアップロード → ローカルのRAW削除
- **サーバー生成（将来対応）**：サーバー側で自動生成、クライアントはダウンロードのみ
- **競合回避**：一時ファイル名で書き込み → 完了時にMOVE（WebDAVのMOVEはアトミック）

```
書き込み中: DSC00123_preview_2560.jxl.tmp
完了時:     MOVE → DSC00123_preview_2560.jxl
```

### JPEG XL設定

- ビット深度: 12bit
- 色空間: Display P3 or Rec.2020（広色域で保存）
- 品質: distance=2.0（バランス重視）
- スマートプレビュー解像度は将来的に可変対応（当面2560px固定）
- WBはニュートラル寄りで現像して保存（後から±1000K程度の微調整は可能）

---

## Lightroom Classic 連携

### 基本方針

- LRカタログ (.lrcat) は**読み取り専用**
- LRのファイル・カタログに一切書き込まない
- TrussPhotoのメタデータはすべて独自DB (tp.db) に保存

### カタログからの読み取り情報

```
[カタログ (.lrcat) から取得]
├── コレクション/アルバム構造
├── ピックフラグ
├── スタック情報
├── 顔の名前紐付け
├── ファイルパスとの紐付け
└── 最終更新日時（差分同期用）

[XMPサイドカーから取得]
├── レーティング（★1-5）
├── 色ラベル
├── キーワード/タグ
├── 顔の位置（矩形座標）
├── 著作権情報
└── タイトル、キャプション

[EXIFから取得]
├── 撮影日時
├── カメラ・レンズ情報
├── GPS座標
└── ISO、シャッタースピード、絞り値
```

### 自動認識フロー

```
TrussPhoto起動
    ↓
LRカタログ参照（前回同期日時以降の追加分をクエリ）
    ↓
新規RAWファイル検出
    ↓
メタデータ抽出 → tp.db に登録
    ↓
サムネイル生成キューに追加
    ↓
バックグラウンドでプレビュー生成
```

### LR現像パラメータについて

- 現像パラメータの移行は行わない（エンジンが異なるため結果が一致しない）
- 部分補正（マスク）のデータも移行しない
- 過去の現像済み写真はLRで管理を継続
- TrussPhotoでは新規撮影分から使い始める運用を想定

---

## ストレージ連携

### WebDAV API（libcurl）

```
PROPFIND  → フォルダ内ファイル一覧取得
GET       → ファイルダウンロード
PUT       → ファイルアップロード
MKCOL     → フォルダ作成
MOVE      → リネーム（アトミック操作）
DELETE    → ファイル削除
```

### 設定

ユーザーが指定する項目：

- WebDAV URL（例: `https://nextcloud.example.com/remote.php/dav/files/user/`）
- 認証情報
- RAWルートフォルダパス（WebDAV内の相対パス）
- プレビュールートフォルダパス（WebDAV内の相対パス）

### アルバム共有

- 共有時に一時フォルダを生成し、JPEG書き出し版を配置
- NextcloudのOCS Share APIで共有リンク発行（期限付き）
- 期限切れで自動アクセス不能

### 対応可能なサービス

WebDAVプロトコルのみに依存するため：
Nextcloud, ownCloud, Synology NAS, QNAP NAS, Box, 自前WebDAVサーバー等に対応可能。

---

## ローカルデータベース (tp.db)

### 主要テーブル

```sql
-- 写真
photos (
    id INTEGER PRIMARY KEY,
    filename TEXT,
    raw_path TEXT,              -- RAWの相対パス
    preview_path TEXT,          -- プレビューの相対パス
    thumb_path TEXT,            -- サムネイルの相対パス
    lr_catalog_id INTEGER,      -- LRカタログ上のID（あれば）
    date_taken DATETIME,
    camera TEXT,
    lens TEXT,
    iso INTEGER,
    shutter_speed TEXT,
    aperture REAL,
    focal_length REAL,
    gps_lat REAL,
    gps_lon REAL,
    rating INTEGER,             -- ★1-5
    label TEXT,                 -- 色ラベル
    flag INTEGER,               -- ピック/不採用
    last_synced DATETIME,
    created_at DATETIME,
    updated_at DATETIME
)

-- キーワード
keywords (
    id INTEGER PRIMARY KEY,
    name TEXT UNIQUE
)

photo_keywords (
    photo_id INTEGER,
    keyword_id INTEGER
)

-- アルバム
albums (
    id INTEGER PRIMARY KEY,
    name TEXT,
    type TEXT,                  -- 'manual' / 'smart'
    smart_query TEXT,           -- スマートアルバムの条件（JSON）
    lr_collection_id INTEGER
)

photo_albums (
    photo_id INTEGER,
    album_id INTEGER,
    sort_order INTEGER
)

-- 顔
faces (
    id INTEGER PRIMARY KEY,
    photo_id INTEGER,
    person_id INTEGER,
    rect_x REAL,
    rect_y REAL,
    rect_w REAL,
    rect_h REAL,
    embedding BLOB              -- 顔ベクトル
)

persons (
    id INTEGER PRIMARY KEY,
    name TEXT
)

-- AIタグ
ai_tags (
    id INTEGER PRIMARY KEY,
    photo_id INTEGER,
    tag TEXT,
    confidence REAL
)

-- 現像パラメータ
edits (
    id INTEGER PRIMARY KEY,
    photo_id INTEGER,
    params JSON,                -- 現像パラメータ全体
    created_at DATETIME
)

-- プレビュー管理
preview_status (
    photo_id INTEGER PRIMARY KEY,
    thumb_generated BOOLEAN,
    thumb_local BOOLEAN,        -- ローカルにある
    preview_generated BOOLEAN,
    preview_local BOOLEAN,      -- ローカルにある
    generated_at DATETIME
)
```

---

## アプリ画面構成

```
┌──────────────────────────────────────────────────────┐
│ [ライブラリ] [現像] [マップ] [アルバム]     モード切替 │
├──────────┬───────────────────────────────────────────┤
│          │                                           │
│ フォルダ  │         メインビュー                       │
│ ツリー    │    （グリッド / 単一写真 / 比較）           │
│          │                                           │
│ ──────── │                                           │
│          │                                           │
│ コレク   │                                           │
│ ション   ├───────────────────────────────────────────┤
│          │     フィルムストリップ / サムネ一覧         │
└──────────┴───────────────────────────────────────────┘

現像モード時は右側にパラメータパネル追加
```

---

## 外部ライブラリ一覧

| ライブラリ | 用途 | 言語 | ライセンス |
|-----------|------|------|-----------|
| LibRaw | RAW読み込み | C/C++ | LGPL/CDDL |
| Lensfun | レンズ補正プロファイル | C | LGPL 3.0 |
| libjxl | JPEG XL読み書き | C/C++ | BSD 3-Clause |
| libcurl | WebDAV通信 | C | MIT/X |
| SQLite | ローカルDB | C | Public Domain |
| ONNX Runtime | AI推論（顔、タグ） | C/C++ | MIT |
| libexif or exiv2 | EXIF/XMP読み取り | C/C++ | LGPL |

すべてC/C++ APIがあり、TrussCから直接呼び出し可能。

---

## 開発フェーズ（案）

### Phase 1: 基盤
- tcxRivet レイアウトシステム
- 基本ウィジェット（ボタン、スライダー、スクロール、分割ペイン）
- 非同期画像ローダー + テクスチャキャッシュ

### Phase 2: ビューア
- フォルダ指定 → サムネグリッド表示
- 単一画像表示（パン、ズーム）
- EXIFメタデータ表示
- LRカタログ読み取り + メタデータインポート

### Phase 3: プレビューシステム
- JPEG XLプレビュー生成パイプライン
- WebDAV連携（ファイル一覧、ダウンロード、アップロード）
- サムネのローカルミラー
- プレビュー管理（生成、ダウンロード、削除）

### Phase 4: 現像エンジン
- LibRawでRAW読み込み
- sokolシェーダーで基本現像（WB、露出、トーンカーブ、HSL）
- Lensfunでレンズ補正
- 非破壊編集パラメータの保存/復元

### Phase 5: 管理機能
- アルバム管理（手動 + スマート）
- レーティング、ラベル、フラグ
- GPXファイルからのGPSタグ付け
- マップ表示

### Phase 6: AI機能
- CLIP によるAIタグ付け
- InsightFace による顔検出・認識
- カメラ特化ノイズ除去

### Phase 7: 共有
- アルバム単位のJPEG書き出し
- WebDAV上に共有フォルダ生成
- 共有リンク発行（Nextcloud OCS API）
