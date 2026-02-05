# TrussPhoto カラーマネジメント方針

## 現状の課題

LibRawでRAW現像すると、Lightroomと比べて色が合わない。

**LibRawが提供するもの：**
- デモザイク（Bayer → RGB）
- カラーマトリックス（センサー → sRGB 線形変換）
- ホワイトバランス

**LibRawにないもの：**
- トーンカーブ（S-curve、コントラスト）
- カメラメーカー固有の「ルック」
- カメラ内プリセット（Creative Style等）の再現

## 調査結果

### オープンソースの選択肢

| 方法 | 内容 | 問題点 |
|------|------|--------|
| DarkTable basecurve | トーンカーブのみ | 新しいカメラのデータがない（ILCE-7CM2なし） |
| RawTherapee camconst.json | カラーマトリックス | LibRawに内蔵済み |
| dcamprof | 自分でDCPプロファイル作成 | ColorCheckerが必要、手間がかかる |
| darktable-chart | DarkTable用プロファイル作成 | DarkTable専用、汎用性なし |

### 結論

**dcamprofでカメラプロファイルを自作する**のがベスト。

理由：
1. 汎用的なDCP形式で出力 → Lightroom, RawTherapee, TrussPhotoで使える
2. カメラ内プリセット（PT, VV, FL等）ごとにプロファイル作成可能
3. 新しいカメラでも対応可能（SIGMA BF等）
4. 高品質（デュアルイルミナント対応、滑らかなLUT生成）

---

## 実装計画

### Phase 1: dcamprofでプロファイル作成（手動作業）

**必要なもの：**
- X-Rite ColorChecker Passport Photo 2（約1万円）
- Argyll CMS（測色データ取得用、無料）
- dcamprof（プロファイル生成、無料）

**作業手順：**

1. ColorCheckerを各プリセットで撮影
   - Sony ILCE-7CM2: Standard, Portrait, Vivid, etc.
   - SIGMA BF: 同様

2. Argyll CMSで測色データ（.ti3）を取得

3. dcamprofでDCPプロファイル生成
   ```bash
   dcamprof make-profile -o profile.dcp measured.ti3
   ```

4. プロファイルをテスト・調整

**参考資料：**
- http://rawtherapee.com/mirror/dcamprof/camera-profiling.html

### Phase 2: TrussPhotoでDCP読み込み

**実装内容：**

1. `tcxDcp` アドオン作成（またはtcxLibRawに統合）
   - DCP（XML + バイナリLUT）のパース
   - カラーマトリックス抽出
   - トーンカーブ抽出
   - 3D LUT（HueSatMap）抽出

2. 適用方法の選択
   - **CPU適用**: LibRaw処理後にカラーマトリックス＋トーンカーブ適用
   - **GPU適用**: tcxLutで3D LUTとして適用（リアルタイム調整可能）

3. EXIFからプリセット判別
   - `CreativeStyle` タグを読み取り
   - 対応するプロファイルを自動選択

### Phase 3: プロファイル管理

**ディレクトリ構造：**
```
~/.trussc/profiles/
├── Sony_ILCE-7CM2/
│   ├── Standard.dcp
│   ├── Portrait.dcp
│   ├── Vivid.dcp
│   └── ...
├── SIGMA_BF/
│   ├── Standard.dcp
│   └── ...
└── ...
```

**機能：**
- カメラ＋プリセットに基づく自動プロファイル選択
- 手動オーバーライド
- プロファイルなしフォールバック（LibRawデフォルト）

---

## DCP形式について

DCP (DNG Camera Profile) はAdobeが策定したカメラプロファイル形式。

**構成要素：**
- ColorMatrix1/2 - XYZ変換マトリックス（標準光源A/D65）
- ForwardMatrix1/2 - 順方向マトリックス
- HueSatMap - 3D LUT（色相・彩度・明度の調整）
- ToneCurve - トーンカーブ
- ProfileLookTable - 追加の3D LUT

**仕様：**
- https://helpx.adobe.com/camera-raw/digital-negative.html
- XMLベース + バイナリデータ

---

## 代替案（保留）

### 埋め込みJPEGプレビュー

RAWファイルに埋め込まれたカメラ処理済みJPEGを使用。
- メリット: カメラのルックが完璧に再現
- デメリット: フル解像度ではない（通常1920x1280程度）

サムネイル表示には使えるかも。

### RAW+JPEGペアからのマッチング

darktable-chartの機能。ColorCheckerなしでカメラJPEGにマッチング。
- メリット: ColorChecker不要
- デメリット: 精度が落ちる、DarkTable専用

---

## TODO

- [ ] ColorChecker購入
- [ ] dcamprof環境構築
- [ ] Sony ILCE-7CM2のプロファイル作成（Standard, Portrait, Vivid）
- [ ] SIGMA BFのプロファイル作成
- [ ] tcxDcpアドオン実装（DCP読み込み）
- [ ] TrussPhotoでの適用テスト
- [ ] EXIF読み取り＋自動プロファイル選択
