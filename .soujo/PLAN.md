# PLAN — Phase 1

完了条件（全部満たしたら停止。Phase 2 に進まない）:

- [x] `cmake --preset debug` が通る
- [x] `cmake --build --preset test && ctest --preset test --output-on-failure` が失敗ゼロ
- [x] `examples/hello` の `GET /` が 200
- [x] `:param` と JSON POST がテストにある
- [x] MW onion（入り A→B、戻り B→A、next なし短絡）がテストにある
- [x] メソッド違いは 405 + Allow、パス無しは 404
- [x] 過大 body / timeout / shutdown（in-flight 完了・新規拒否）がテストにある
- [x] TLS / HTTP/2 / ORM / テンプレートがリポジトリに無い
- [x] hello が公開ヘッダだけに依存する
- [x] debug + ASan で新規リーク・UAF が無い
- [x] 頼んでいないファイルが diff に無い
