# xv6 Reader

MIT 6.1810 Fall 2026 の Labs と公式 xv6 book を、ローカルで読みやすく閲覧するための静的サイトです。UI は隣の `../sudoku` にある Markdown reader と同じ設計思想（Bricolage Grotesque + Geist、静かなインディゴ、罫線中心の一覧）に揃えています。

## 起動

Node.js 22.12 以降を用意して、リポジトリのルートで実行します。

```sh
npm install
npm run dev
```

表示された `http://localhost:4321` を開いてください。Lab と Book を横断する検索は `⌘K` または `Ctrl+K` でも開けます。

各Lab／章は、公式本文を日本語化したローカル版を表示します。UI文言は英語のまま維持し、課題文の構造、コード本体、コマンド、識別子、リンク、図版は変更しません。コード中のコメントだけは日本語化しています。翻訳版は `content/book-ja/` と `data/labs/processed-ja/` に分離し、原文の取得データも残しています。

本番相当の静的出力は次のコマンドで作成します。

```sh
npm run build
npm run preview
```

## コンテンツの更新

### Labs

```sh
npm run sync:labs
```

対象は公式 Fall 2026 スケジュールに掲載される次の9件だけです。

`util`, `syscall`, `pgtbl`, `traps`, `cow`, `net`, `lock`, `fs`, `mmap`

- `robots.txt` と公式スケジュールを先に確認します。
- ネットワークアクセスは逐次実行し、各リクエストの間を3秒以上空けます。
- 初回取得HTMLは `data/labs/raw/` に保存し、通常実行では再取得しません。
- `--refresh` では `ETag` / `Last-Modified` を使った条件付き取得を行います。
- 403、429、5xx、チャレンジページ、接続リセットを検知した場合は中止します。
- `data/labs/metadata.json` に取得元・日時・レスポンス情報を記録します。

特定の1件だけを確認する場合:

```sh
npm run sync:labs -- --only util
```

### Book

Book の再生成には Git、Python 3、[Quarto](https://quarto.org/)、および公式 book の整形処理が使う `groff` が必要です。

```sh
npm run sync:book
```

この処理は `mit-pdos/xv6-riscv-book` を `data/book/source/` に clone し、公式 `convert-quarto.py` を先に実行します。その QMD を Markdown に正規化し、数式・脚注・引用・コード・図・相互参照を維持します。コミットSHAは `data/book/source-version.json` と各章の frontmatter に記録されます。

TikZ 由来でリポジトリに生成物が含まれない5図だけは、同じ公式リポジトリの公開ビルドからSVGを取得します。取得元とファイル名も `source-version.json` に記録します。

公式資料を更新した場合は、Google翻訳を開いた `agent-browser` セッションを指定して `AGENT_BROWSER_SESSION=... npm run translate:ja` を実行すると、日本語版も再生成できます。

## 検証

```sh
npm test
npm run check
npm run build
```

検証内容:

- Lab の許可リストが9件から増減していないこと
- 見出し、段落、コード、リンク、リスト、表、図の件数が変換後に減っていないこと
- Book が14章揃い、図版とローカルリンクが解決できること
- 引用キーが未解決で残っていないこと
- 検索索引が Labs と Book の両方を含むこと
- 全9 Lab／14章の本文が日本語化されていること
- 静的出力内の内部リンクと画像・スクリプト・CSSがすべて解決できること

## 主な構成

- `src/pages/` — Astro の各ページ
- `src/components/` — ヘッダー、章ナビ、ページ内目次、出典表示
- `content/book/` — 公式 QMD から生成した Markdown 14章
- `data/book/conversion-stats.json` — Book変換前後の見出し・コード・図・表・脚注数
- `data/labs/raw/` — 取得した公式 Lab HTML（無加工）
- `data/labs/processed/` — 表示用HTMLと検証統計
- `content/book-ja/` — 日本語化した Book 14章
- `data/labs/processed-ja/` — 日本語化した Lab 表示用HTML
- `scripts/sync-labs.ts` — 低頻度・キャッシュ付き Lab 取得
- `scripts/sync-book.ts` — 公式リポジトリからの Book 変換
- `scripts/translate-ja.ts` — キャッシュ済み本文の日本語版生成（Google翻訳を生成時のみ使用）
- `scripts/verify-content.ts` — 欠落・リンク検査
- `public/search-index.json` — ローカル横断検索索引

コンテンツの著作権は各原著者に帰属します。各ページ下部に公式出典と、Lab は取得日、Book はコミットSHAを表示します。
