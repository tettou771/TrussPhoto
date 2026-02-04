# TrussPhoto Security: API Key Authentication

## 認証方式

TrussPhoto サーバは **Bearer トークン方式** の API Key 認証を採用している。

- サーバが初回起動時にランダムな API Key を生成し、`server_config.json` に保存
- クライアントは `settings.json` に同じキーを設定し、全 HTTP リクエストの `Authorization` ヘッダーに付与する

```
Authorization: Bearer <64-char-hex-key>
```

`/api/health` エンドポイントのみ認証不要（監視・疎通確認用）。
それ以外のエンドポイントはキーが一致しなければ `401 Unauthorized` を返す。

### 認証フロー

```
Client                              Server
  |                                   |
  |  GET /api/health                  |   (認証不要)
  |---------------------------------->|
  |  200 OK                           |
  |<----------------------------------|
  |                                   |
  |  GET /api/photos                  |   (認証必要)
  |  Authorization: Bearer <key>      |
  |---------------------------------->|
  |  200 OK + JSON                    |
  |<----------------------------------|
  |                                   |
  |  GET /api/photos                  |   (キーなし/不一致)
  |---------------------------------->|
  |  401 Unauthorized                 |
  |<----------------------------------|
```

### キーの配布

現時点では手動コピー。サーバ初回起動時にログと画面に API Key が表示されるので、
それをクライアントの `settings.json` に貼る、または MCP `set_server` ツールで渡す。

```bash
# サーバ側: bin/data/server_config.json に自動生成
{"apiKey": "a1b2c3...64文字hex...", "port": 8080}

# クライアント側: bin/data/settings.json に手動追加
{"serverUrl": "http://localhost:8080", "apiKey": "a1b2c3...同じキー..."}
```

---

## アーキテクチャと採用理由

### なぜ Bearer Token か

| 方式 | 検討結果 |
|------|---------|
| **Bearer Token (採用)** | シンプル、ステートレス、curl でもすぐ試せる。ローカルネットワーク用途に十分 |
| Basic 認証 | ユーザー/パスワードの管理が不要な用途には過剰 |
| OAuth 2.0 | サードパーティ連携がないので不要 |
| mTLS | 証明書管理が煩雑。LAN内の個人用途にはオーバー |

TrussPhoto はローカルネットワーク内の個人利用が前提で、
インターネット公開は想定していない。
この前提では Bearer Token で十分なセキュリティを確保できる。

### ミドルウェア方式

Crow の **ミドルウェア機構** (`crow::App<AuthMiddleware>`) を使い、
全ルートを横断的に保護している。

- ルートごとに認証チェックを書く必要がない
- エンドポイント追加時に認証漏れが起きない
- health チェックだけホワイトリストで除外

### キー生成

- `std::random_device` + `mt19937_64` で暗号学的にランダムな値を取得
- 64 文字の hex 文字列（256 bit エントロピー）
- ブルートフォースは現実的に不可能

### セキュリティ上の制約・注意点

- **HTTP 通信**: 現状 TLS なし。LAN 外で使う場合はリバースプロキシ (nginx, Caddy 等) で HTTPS 化が必要
- **キーローテーション**: 手動で `server_config.json` のキーを書き換えて再起動すれば可能。自動ローテーションの仕組みはない
- **キーの画面表示**: サーバの `draw()` で API Key を画面に表示している。共有ディスプレイ環境では注意

---

## 実装概要

### サーバ側

| ファイル | 役割 |
|---------|------|
| `ServerConfig.h` | API Key の生成・永続化 (`server_config.json`) |
| `tcApp.h` | `AuthMiddleware` 構造体の定義、`crow::App<AuthMiddleware>` への切り替え |
| `tcApp.cpp` | 起動時にキー読み込み → ミドルウェアに注入、画面にキー表示 |

`AuthMiddleware::before_handle()` で `Authorization` ヘッダーを検証し、
不一致なら `res.code = 401` で即座にレスポンスを返す。

### クライアント側

| ファイル | 役割 |
|---------|------|
| `tcxCurl.h` (アドオン) | `addHeader()`, `setBearerToken()` を HttpClient に追加 |
| `Settings.h` | `apiKey` フィールドの永続化 |
| `PhotoProvider.h` | `setApiKey()` → 内部 HttpClient にトークンをセット |
| `UploadQueue.h` | アップロードスレッド用 HttpClient にもトークンをセット |
| `tcApp.cpp` | 起動時に各コンポーネントへキーを伝播、MCP `set_server` にオプショナル `apiKey` 引数を追加 |

### 検証コマンド

```bash
# キーなし → 401
curl http://localhost:8080/api/photos

# キーあり → 200
curl -H "Authorization: Bearer <key>" http://localhost:8080/api/photos

# health は認証不要 → 200
curl http://localhost:8080/api/health
```
