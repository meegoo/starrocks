---
displayed_sidebar: docs
description: "zstd_compression_gain は、大きなテキスト列や JSON 列を zstd_compression_columns に指定した場合にどれだけ小さくなるかを見積もります。"
---

# zstd_compression_gain

大きなテキスト列や JSON 列をテーブルプロパティ
[`zstd_compression_columns`](../../sql-statements/table_bucket_part_index/CREATE_TABLE.md)
に指定した場合、どれだけ小さくなるかを見積もります。

見積もりはストレージ層と同じことを行って求めます。列の値を連結してサンプルを作り、設定された
`data_page_size` の大きさでページに切り、各ページを 3 通り — LZ4、ZSTD、そして最初のページを
ディクショナリとして使う ZSTD — で圧縮します。この 3 つの合計を比べれば、その列にプロパティを
設定する価値があるかどうかが分かります。

この関数は v4.2 からサポートされています。

## 構文

```Haskell
ZSTD_COMPRESSION_GAIN(<expr>)
```

## パラメータ

`expr`: CHAR、VARCHAR、STRING、JSON 型の列。NULL 値はカウントされ、スキップされます。

## 戻り値

JSON オブジェクトを含む VARCHAR を返します:

| フィールド | 意味 |
| --- | --- |
| `rows` / `null_rows` | 読み取った行数と、そのうち NULL だった行数。 |
| `total_bytes` / `avg_row_bytes` | NULL 以外のすべての値の生バイト数と、1 行あたりの平均。 |
| `sampled_rows` / `sampled_bytes` | 実際に測定された行数とバイト数。 |
| `page_bytes` / `sampled_pages` | 使用したページサイズと、サンプルが占めたページ数。 |
| `measured_pages` | 下の 3 つの合計が対象としたページ数 — 最初の 1 ページを除いたすべて。 |
| `lz4_bytes` | 既定の圧縮方式である LZ4 でのサンプルのサイズ。 |
| `zstd_bytes` | ディクショナリなしの ZSTD でのサイズ。 |
| `zstd_with_dict_bytes` | ディクショナリありの ZSTD でのサイズ。プロパティを設定した場合の結果です。 |
| `dict_bytes` | ディクショナリ自体のサイズ。列ごと、Segment ごとに 1 つ保存されます。 |
| `times_smaller_than_lz4` / `times_smaller_than_zstd` | 2 つの基準に対して `zstd_with_dict_bytes` が何倍小さいか。 |
| `dictionary_kept` | 既定のページサイズで書き込み側が実際にディクショナリを保持するかどうか (書き込み側と同じ規則 `zstd_compression_dict_min_gain` を適用)。 |
| `suggested_page_size` | `zstd_compression_columns` の列名の後ろに書くとよいページサイズ。 |
| `suggested_times_smaller_than_lz4` | そのページサイズでの LZ4 比の縮小倍率。 |
| `page_size_options` | 候補ページサイズごとの内訳: 1 ページの行数 (ポイントルックアップでの解凍量)、ディクショナリあり・なしのサイズ、書き込み側の選択。 |
| `suggestion` | 上記の数値をそのまま読んだ結果。 |

列がどれだけ大きくても、測定されるのは先頭 8 MB だけです。これは意図的なものです。書き込み側が
ディクショナリを構築するときに使うのも、テーブル全体ではなく 1 つの Segment 分のデータだからです。

`dict_bytes` は合計に含めず、別に報告します。実際の Segment はこのサンプルよりはるかに多くの
ページを持つため、ディクショナリを数ページ分に負担させると効果を過小評価することになります。

サンプルの先頭はディクショナリ用に確保され、すべての測定から除外されます。こうすることで、候補となる
ページサイズがすべて同じバイト列で比較されます。サイズごとに 1 ページを除外する方式では、除外される
割合がサイズごとに異なり、大きなページはその理由だけで有利に見えてしまいます。確保分を除いた後に
何も残らないサンプルでは見積もりを行わず、その理由を返します。

`suggested_page_size` は「最良から 5% 以内で最小の候補」です。ページはポイントルックアップで解凍
される単位であり、数パーセントの圧縮率のために 1 行の読み取りコストを何倍にもする価値はありません。
各選択肢の `rows_per_page` がそのコストです。

2 つの制限があります。1 つ目は、この見積もりが列の**生の値**を圧縮しており、書き込み側が実際に構築
するエンコード済みページではないことです。そのため 1 行が非常に短い (オフセット配列がページの大きな
割合を占める) 列では、ディクショナリに関する判断が書き込み側と異なる場合があります。2 つ目は、
書き込み側が列ごと Segment ごとの先頭数ページで判断する一方、こちらは列全体の上限付きサンプルを
読むことです。内容が時間とともに変化する列では両者が一致しないことがあります。

## 例

```plaintext
mysql> SELECT zstd_compression_gain(input) FROM spans;
+---------------------------------------------------------------------------+
| zstd_compression_gain(input)                                               |
+---------------------------------------------------------------------------+
| {"rows":120000,"null_rows":0,"total_bytes":6153420800,"avg_row_bytes":51278,
|  "sampled_rows":163,"sampled_bytes":8388096,"page_bytes":65536,"sampled_pages":128,"measured_pages":127,
|  "lz4_bytes":4193280,"zstd_bytes":2096640,"zstd_with_dict_bytes":1006387,
|  "dict_bytes":65536,"dictionary_kept":true,
|  "times_smaller_than_lz4":4.17,"times_smaller_than_zstd":2.08,
|  "suggested_page_size":65536,"suggested_times_smaller_than_lz4":4.17,
|  "page_size_options":[{"page_bytes":65536,"pages":112,"rows_per_page":1.3,...}],
|  "suggestion":"enable zstd_compression_columns on this column"}             |
+---------------------------------------------------------------------------+
```

読み方: この列はプロパティを設定すると現状より約 4.2 倍小さくなるため、設定する価値があります。

```sql
ALTER TABLE spans SET ("zstd_compression_columns" = "input");
```

行同士に共通点がほとんどない列では、逆の結果になります:

```plaintext
mysql> SELECT zstd_compression_gain(request_id) FROM spans;
+-----------------------------------------------------------------+
| zstd_compression_gain(request_id)                                |
+-----------------------------------------------------------------+
| {..."times_smaller_than_lz4":1.03,"times_smaller_than_zstd":1.01,
|  "suggestion":"little to gain, leave the column as it is"}       |
+-----------------------------------------------------------------+
```

## キーワード

ZSTD_COMPRESSION_GAIN, COMPRESSION, ZSTD
