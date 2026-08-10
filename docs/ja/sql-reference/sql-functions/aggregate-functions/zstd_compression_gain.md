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
| `suggestion` | 上記の数値をそのまま読んだ結果。 |

列がどれだけ大きくても、測定されるのは先頭 8 MB だけです。これは意図的なものです。書き込み側が
ディクショナリを構築するときに使うのも、テーブル全体ではなく 1 つの Segment 分のデータだからです。

`dict_bytes` は合計に含めず、別に報告します。実際の Segment はこのサンプルよりはるかに多くの
ページを持つため、ディクショナリを数ページ分に負担させると効果を過小評価することになります。

3 つの合計はいずれも最初のページを含みません。そのページはディクショナリの取得元であり、自分自身に
対して圧縮されてしまうためです。実際の Segment でも同じことは起きますが、そこでは数百ページのうちの
1 ページに過ぎません。ここではそれが測定結果の大半を占めてしまいます。したがってサンプルが 2 ページ目に
届かない場合、この関数は見積もりを出さず、その理由を返します。

## 例

```plaintext
mysql> SELECT zstd_compression_gain(input) FROM spans;
+---------------------------------------------------------------------------+
| zstd_compression_gain(input)                                               |
+---------------------------------------------------------------------------+
| {"rows":120000,"null_rows":0,"total_bytes":6153420800,"avg_row_bytes":51278,
|  "sampled_rows":163,"sampled_bytes":8388096,"page_bytes":65536,"sampled_pages":128,"measured_pages":127,
|  "lz4_bytes":4193280,"zstd_bytes":2096640,"zstd_with_dict_bytes":1006387,
|  "dict_bytes":65536,"times_smaller_than_lz4":4.17,"times_smaller_than_zstd":2.08,
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
