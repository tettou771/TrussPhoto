# TrussPhoto 同期アーキテクチャ設計

> **Status (2026-07):** 設計確定。実装は Phase A → B → C の順。
> このドキュメントが同期まわりの実装仕様の正。VISION.md の「サーバ連携」「マルチクライアント同期」節を具体化・置換する。
> 旧構想（ARCHITECTURE_CLOUD.md のシンクライアント型・縮小DNG・編集ロック）は本設計で置き換え済み。

## 基本原則: ローカルファースト + レプリケーション

LR CC の「クライアント＝サーバの薄いキャッシュ」モデルは採らない。

> **クライアントは常にフルのローカルカタログエンジンとして動く。
> サーバ接続は UI の関心事ではなく、バックグラウンドのレプリケーションでしかない。**

- UI は常に自分の `library.db` だけを読み書きする（高速、完全オフライン動作）
- `SyncEngine` が裏で「ローカルカタログ ⇔ サーバカタログ」の差分を運ぶ
- ローカル専用モード ＝ レプリケーション先が未設定、という退化ケース
- サーバも同じカタログ構造（library.db + originals + SP + サムネ）を持つ対等なノード。
  違いはポリシーだけ（originals の権威を持つ / 派生データ生成を担う / 常時稼働）

これにより「ローカルモードとサーバモードをシームレスに繋ぐ」問題は消える。
モードの切替ではなく、同期先があるかないかの差でしかない。

## ペイロードの階層モデル

**ローカルファーストの対象はカタログ（library.db）であって、画像ペイロードではない。**
カタログは全ノードがフルコピーを持つ（110k枚でも数百MB）。
画像ペイロードは3階層それぞれが独立に「ローカルにあるか／サーバにあるか」の状態を持つ。

| 階層 | 形式 | 性質 | ローカルから消せる条件 |
|------|------|------|----------------------|
| サムネイル | 512px JPEG | 純粋なキャッシュ | いつでも（SP/originalから再生成可能） |
| スマートプレビュー | 2560px JPEG XL (XYB f16) | ほぼキャッシュ | ほぼいつでも（originalがどこかにあれば再生成可能） |
| original | RAW/JPEG | **権威データ。再生成不可能** | サーバ保有をチェックサムで確認できた時のみ |

- サムネ/SP は LRU + 容量上限で雑に管理してよい
- **originals の「退避」は削除ではなく、権威の所在をローカル→サーバに移す操作**。
  退避後のローカルコピーはキャッシュ扱いに格下げされる

### 解決チェーン

```
グリッド表示:     ローカルサムネ → サーバサムネDL → SPからデコード → originalからデコード
シングルビュー/現像: ローカルSP → サーバSPをDL&キャッシュ → ローカルoriginal → サーバoriginal
書き出し(フル品質):  ローカルoriginal → サーバからフェッチ（明示的、進捗UI必須）
```

既存の `getThumbnail()`（ローカル → サーバ → RAWデコード）の一般化。

## 同期プロトコル

### 変更フィード（cursor 同期）

全件比較（現行の `GET /api/photos` 方式）は 110k 枚ではスケールしない。

- サーバは単調増加の **seq**（変更シーケンス番号）を持つ
- 写真行が変更されるたびに、その行の `serverSeq` を採番し直す
- クライアントは `GET /api/changes?since=<seq>` で差分だけを取得し、
  受信した最大 seq をカーソルとして永続化する
- oplog は持たない。**state-based**（変更行を丸ごと送り、受信側でフィールド単位マージ）

### フィールド単位 last-write-wins

スキーマにある `xxxUpdatedAt`（ms epoch）をそのまま使う。

- 人間編集フィールド（rating, colorLabel, flag, memo, tags）: フィールドごとに
  updatedAt を比較し、新しい方を採用。別フィールドの並行編集は両方生き残る
- **現像パラメータは丸ごと1フィールド**（develop snapshot 全体に1つの updatedAt）。
  スライダー単位でマージすると意図しない合成結果になるため、編集セッション単位の後勝ち

### tombstone（削除の伝播）

存在ベースの同期には「オフライン中に削除した写真が次回syncで復活する」構造的バグがある。

- `photos` テーブルに `deletedAt`（ms epoch, 0=生存）を追加
- 削除は行の物理削除ではなく tombstone 化。変更フィードに乗って全ノードに伝播
- tombstone を受信したノードはローカルのペイロード（original含む）を削除してよい
  （originalの物理削除は「サーバに残す/完全削除」のユーザー意図を尊重すること）
- 物理削除（DB上のGC）は全クライアントの同期が十分進んだ後の別課題。当面残しっぱなしでよい

### 同期フロー

```
1. push: ローカルの dirty 行（前回push以降に updatedAt が進んだ行）をサーバへ送信
2. サーバ: フィールド単位LWWでマージ、変更行に新しい serverSeq を採番
3. pull: GET /api/changes?since=<cursor> で差分取得
4. クライアント: フィールド単位LWWでローカルにマージ、カーソル更新
```

push が先。サーバ側マージ結果（自分の変更＋他ノードの変更の合成）を pull で受け取る形。

## データ権威と派生データ

| データ種別 | 扱い |
|-----------|------|
| 人間編集（rating, memo, tags, 現像パラメータ…） | フィールド単位LWW。事実上クライアント権威 |
| original ファイル | 内容不変（追加/削除のみ）。所在だけが状態 |
| 機械生成（サムネ, SP, embedding, 顔, レンズ補正params） | **冪等データ。権威という概念を使わない** |

### 派生データの生成ポリシー

クライアントもサーバも同じ C++ パイプライン（1バイナリ設計）を持つため、
**どのノードが生成しても結果は同一**。したがって:

- 「入力（original）と暇なリソースを持つノードが生成し、生成物は同期で全ノードに配布」
- (photo, model/pipeline-version) をキーとして冪等。競合しても同じ結果なので壊れない
- 二重生成の防止は最適化にすぎない（生成キューが「他ノード生成済み」を見てスキップ）
- クライアント生成 ＝ インポート直後に即見たい、のためのレイテンシ最適化
- サーバ生成 ＝ サーバ直インポート・非力クライアント・将来のWebギャラリーのため
- **サーバにGPUは不要**。SP/サムネ生成はCPUパイプライン（LibRawデコード + レンズ補正 +
  リサイズ + JXLエンコード + CPU develop）で完結する

### 編集反映済みサムネの生成責任

編集したノードが再生成し、派生データとして配布する（現行のFBO readback方式を維持）。
サーバのCPU現像でも同一結果を出せるが、当面は「編集ノードが焼く」で統一。

## originals 退避（Phase C）

1. **退避条件**: syncState == Synced かつ、サーバ側チェックサム一致の確認後のみ。
   退避したら ServerOnly に遷移
2. **チェックサム**: アップロード時に xxhash を計算し DB に保存。退避時に照合
3. **ポリシー**（ユーザー設定）: 全部ローカル保持（デフォルト）/ 直近Nヶ月のみ /
   空き容量ターゲット。レーティング等によるピン留めは将来
4. **ローカル専用モードでは退避は存在しない**（退避先がない）
5. オンデマンド取得: 書き出し時・等倍確認時・クロップ率が閾値を超えた時に original を自動フェッチ

### オフライン時の劣化（設計目標）

- サムネキャッシュあり → グリッド閲覧可
- SPキャッシュあり → 現像編集可（パラメータはlibrary.db書き込みのみ、復帰時sync）
- originalなし & オフライン → フル品質書き出しのみ不可

## スコープ決定

- **WASM/ブラウザ版フル機能クライアントは作らない**。唯一「薄いクライアント」という
  第2のアーキテクチャを強制する存在であり、コスパが悪い
- 「出先でブラウザから見たい」は将来、**読み取り専用の共有ギャラリー**（サーバがJPEGを
  焼いて返すだけ）で満たす。共有リンク機能と同一のもの
- **iOS版は同一アーキテクチャ**で成立する（TrussC/sokol/LibRaw/SQLite/ONNXすべてiOSで動く）。
  「originalsをデフォルトで持たない積極退避ポリシー + SP中心現像」の設定違いにすぎない。
  Phase C の退避機構がそのまま活きる
- 認証は当面 API キー（Bearer）のまま。インターネット公開は Tailscale/VPN 越し前提とし、
  JWT等の認証基盤は作らない
- API はクライアント非依存に保つ（変更フィード / ペイロード取得 / 書き出しリクエスト）。
  将来のWebクライアントの余地はAPIが勝手に残す

## スキーマ変更（Phase A、互換性破壊OK）

マイグレーション不要（LRカタログをまっさらから再インポートする前提）。CREATE し直してよい。

```sql
-- photos テーブルへの追加
deletedAt      INTEGER DEFAULT 0,   -- tombstone (ms epoch, 0=alive)
serverSeq      INTEGER DEFAULT 0,   -- server-assigned change sequence
checksum       TEXT DEFAULT '',     -- xxhash64 of original file (hex)
developParams  TEXT DEFAULT '',     -- develop snapshot JSON
developUpdatedAt INTEGER DEFAULT 0,

-- sync 状態（クライアント側）
CREATE TABLE sync_state (
    key   TEXT PRIMARY KEY,   -- 'cursor', 'lastPushAt', ...
    value TEXT
);
```

サーバ側の seq 採番: `UPDATE photos SET serverSeq = (SELECT COALESCE(MAX(serverSeq),0)+1 FROM photos) WHERE id = ?`
（単一writer前提で十分。将来問題になれば専用カウンタテーブルに変更）

## API 変更（Phase A/B）

```
GET  /api/changes?since=<seq>          # 変更フィード（tombstone含む、ページング付き）
POST /api/sync/push                    # dirty行の一括push（フィールド単位LWWマージ）
GET  /api/photos/:id/preview           # スマートプレビュー (JPEG XL)
PUT  /api/photos/:id/preview           # SP アップロード（クライアント生成分の配布）
PUT  /api/photos/:id/thumbnail         # サムネアップロード（編集反映サムネの配布）
GET  /api/photos/:id/original          # original ダウンロード（Phase C）
```

既存の `/api/photos`, `/api/import`, `/api/photos/:id/metadata` 等は維持
（/api/photos はブートストラップ・デバッグ用途に残す）。

## 実装フェーズ

- **Phase A**: スキーマ刷新 + SyncEngine を PhotoProvider から分離 + 変更フィード +
  フィールド単位LWW同期 + tombstone。→ 2台フルカタログ運用が成立
- **Phase B**: SP/サムネの双方向配布 + ServerOnly 写真の閲覧・編集 + サーバ側SP/サムネ生成。
  → 新マシンのセットアップが「serverUrl 設定するだけ」になる
- **Phase C**: originals 退避 + オンデマンドフェッチ + 退避ポリシーUI。
  → LR CC 相当のストレージモデル完成。iOS版の前提が整う
