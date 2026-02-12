# TrussPhoto

TrussC ベースの写真管理・RAW現像アプリケーション。
セルフホストサーバとデスクトップクライアントで構成される個人向け写真システム。

## 特徴

- **RAW対応**: LibRaw によるRAWデコード、プログレッシブ表示（半サイズ即時 → フルサイズバックグラウンド）
- **カメラプロファイル**: Creative Style別の LUT (.cube) をGPUシェーダーで適用
- **レンズ補正**: lensfun DB ベースの歪曲・周辺光量・色収差補正（自前実装）
- **セルフホストサーバ**: REST API サーバとの同期・自動アップロード
- **MCP対応**: AI エージェントからの操作（フォルダ読み込み、サーバ設定、ライブラリ修復）
- **省電力**: イベント駆動レンダリング（変更時のみ描画）

## 構成

```
apps/
├── TrussPhoto/          ← デスクトップクライアント（このリポジトリ）
└── TrussPhotoServer/    ← REST API サーバ（別リポジトリ）
```

## ビルド

```bash
cmake --preset macos
cmake --build build-macos
```

詳しくは [HOW_TO_BUILD.md](HOW_TO_BUILD.md) を参照。

## ドキュメント

| ファイル | 内容 |
|---------|------|
| [VISION.md](VISION.md) | プロダクトビジョンと将来ロードマップ。今後の方向性はここを参照 |
| [ARCHITECTURE_CLOUD.md](ARCHITECTURE_CLOUD.md) | クライアント・サーバアーキテクチャの設計メモ。Phase 1（サーバ基盤）は実装済み |
| [TrussPhoto_Archtecture.md](TrussPhoto_Archtecture.md) | 初期の全体設計構想。WebDAV連携やLightroom連携など、一部は方針変更済み |
| [HOW_TO_BUILD.md](HOW_TO_BUILD.md) | ビルド手順、依存関係 |
| [SECURITY.md](SECURITY.md) | API Key 認証の仕組み |
| [COLOR_MANAGEMENT.md](COLOR_MANAGEMENT.md) | カラーマネジメント方針、dcamprof によるプロファイル作成計画 |
| [LICENSES.md](LICENSES.md) | ライセンス情報 |
| [CLAUDE.md](CLAUDE.md) | AI アシスタント向けプロジェクトコンテキスト |

## 技術スタック

| 領域 | 技術 |
|------|------|
| フレームワーク | TrussC (sokol) |
| RAW デコード | LibRaw (tcxLibRaw アドオン) |
| HTTP クライアント | libcurl (tcxCurl アドオン) |
| カラーグレーディング | GPU LUT シェーダー (tcxLut アドオン) |
| メタデータ | exiv2 (EXIF / MakerNote) |
| レンズ補正 | lensfun XML + 自前実装 |
| サーバ | Crow (tcxCrow アドオン) |

## ライセンス

MIT License. 詳細は [LICENSES.md](LICENSES.md) を参照。
