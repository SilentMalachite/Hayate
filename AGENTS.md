# AGENTS.md（Sol）

Sol で **Hayate** を扱うときの指示。
Claude Code 用の `CLAUDE.md` より短いのは意図的。写しにしない。規約を変えるときは両方を直す。

正本: `docs/SPEC.md`。図: `docs/ER.md` / `docs/UML.md`。
コード探索は Serena（`.serena/`）。構造・関係は Graphify（`graphify-out/`）。grep の前に両方。

## 相手

- 状態はファイル。会話は覚えていない前提。
- 表・図・対比から。短く。日本語。コードとコミットは英語。
- 止まる前に、未完了なら分かる印をファイルに残す。

## 応答

- 質問は1つ。番号付き候補。
- 最初の1文で結果。前置き・免責・要約の繰り返しは不要。
- 終わりに結果1文＋変更ファイル。

## 完了

- 頼まれた範囲だけ。広げない。別案は1文、実装は頼まれた通り。
- 作業順: SPEC → 公開ヘッダ → 失敗するテスト → 最小実装 → 全テスト → 停止。
- 今の受け入れは Phase 1 と Phase 2（CORS / 静的ファイル / レート制限）と Phase 3（TLS / JWT 検証 / OpenAPI 生成 / 最小 metrics / 静的ファイルのストリーミング送出）。multipart / WS / SSE / gzip は跨がない。
- 読み方で成果物が割れるときだけ1問。それ以外は決めて進める。
- レビューは見つけたものを全部表で出す。絞り込みはしない。

## Hayate

- C++20。namespace `hayate`。Asio + Beast。コルーチンハンドラ。マクロルート禁止。
- `Result<T>` は 1 実装。`std::expected` 禁止。
- JSON は nlohmann/json 1 本（glaze は C++23 必須なので採らない）。混在禁止。
- Request の view を Request より長く持たない。
- 例外は境界を出ない。`new`/`delete`/`malloc`・生配列・共有可変グローバル禁止。C++23/26 必須機能と C++ Modules は使わない。神オブジェクト禁止。
- Beast/Asio の失敗は、応答を書けるなら `Error` にして応答（413 / 431 / 500）。書けないなら閉じる。受け手のない `Error` は作らない。
- Drogon / Crow / Oat++ / Cinatra / userver をコピーしない。
- 図に無い型を足さない。

```bash
cmake --preset debug
cmake --build --preset test && ctest --preset test --output-on-failure
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan --output-on-failure
```

## 聞くもの

依存追加、公開 API 破壊、Phase 跨ぎ、Windows 専用、ベンチ数字、ライセンス。

秘匿情報（トークン、セッション URL、個人情報、ホームの絶対パス）をコード・ログ・コミットに書かない。
