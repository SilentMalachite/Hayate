# CLAUDE.md（Opus 5）

Claude Code で **Hayate** を扱うときの指示。
`AGENTS.md` は Sol 向けで**写しではない**。規約を変えるときは両方を直す。

正本: `docs/SPEC.md`。図は `docs/ER.md` / `docs/UML.md`。
チャットの合意はファイルに落とすまで存在しない。

コード探索は Serena（clangd LSP、`.serena/`）。型・所有・経路は Graphify（`graphify-out/`、質問は `graphify query`）。grep / ファイル全読みの前に両方を使う。graph が無ければ先に `/graphify`。

## 相手

- 状態・手順・決定は会話ではなくファイルに置く。
- 構造・対比・図から入る。短く、密度高く。易しくしない。
- 会話は日本語。コード・識別子・コミットは英語。
- セッションは予告なく途切れる。いつ止まっても再開できる状態を残す。
- 1応答は画面1枚を目安。音声に頼らない。

## 応答

- 質問は1メッセージに1つ。候補は番号付きの短い表。
- 結論 → 根拠 → 詳細。最初の1文で結果を言う。
- 前置き・免責・繰り返し要約・褒めは書かない。
- 図は ASCII か Mermaid。色や動きに意味を持たせない。
- 進捗: 着手前に1文、方針が変わったときだけ、終わりに結果1文＋変更ファイル。

## このリポジトリ

C++20 の HTTP フレームワーク兼サーバー。namespace `hayate`。
ハンドラはコルーチン。公開 API は Fluent + concepts。マクロでルートを登録しない。
土台は Asio + Boost.Beast。自前イベントループ・自前 HTTP パーサは書かない。

作業順（この順以外で実装しない）:

1. SPEC を読む / 足りなければ該当節だけ直す
2. 公開ヘッダのシグネチャ
3. 失敗するテスト
4. 最小実装
5. 全テスト
6. 停止

Phase を飛ばさない。今の受け入れは Phase 1 と CORS と静的ファイルとレート制限と静的ファイルのストリーミング送出。multipart / WS / SSE / gzip は実装しない。

## 制約

- C++20 厳守。`std::expected`・C++ Modules・C++23/26 必須機能は使わない
- `hayate::Result<T>` は薄い自前か Boost.Outcome の一方
- JSON は 1 本（優先 glaze、ダメなら nlohmann）。混在禁止
- `new` / `delete` / `malloc`、生配列禁止
- Request 配下は非所有 view。寿命は Request。view を App や Response に保存しない
- 例外はハンドラ / MW 境界を出ない。Beast/Asio は `Error` に変換
- 共有可変グローバル禁止。状態は App か Request の Extension
- 実装が 2 つになるまで抽象・ファクトリを切らない
- 既存フレームワーク（Drogon / Crow / Oat++ / Cinatra / userver）のコードをコピーしない
- 公開ヘッダは `include/hayate/`。hello は公開ヘッダだけに依存
- テストは GoogleTest。ループバック + エフェメラルポート。スリープ同期しない
- debug preset で ASan。新規リーク・UAF を残さない
- clang-format を通す。コメントは「なぜ」だけ

コマンド（揃えたらこの名前）:

```bash
cmake --preset debug
cmake --build --preset debug
cmake --build --preset test && ctest --preset test --output-on-failure
```

## 聞いてからやれ

- 新しい依存
- 公開 API の破壊的変更
- Phase を跨ぐ機能
- Windows 専用コード
- ベンチマーク数字の主張
- ライセンス変更

## やらないこと

- HTTP/2, HTTP/3, gRPC, GraphQL、テンプレート、ORM
- 頼んでいないサンプル・「将来のため」の層
- UML / ER に無い型（Service / Context / ApplicationBuilder）
- 追加の検証ステップやサブエージェントによる再確認
- ユーザーに「前回何を話したか」を思い出させる質問
- `AGENTS.md` をこのファイルからコピーして上書きすること


