---
displayed_sidebar: docs
description: "Estimates how much smaller a large text or JSON column would get under the zstd_compression_columns table property."
---

# zstd_compression_gain

Estimates how much smaller a large text or JSON column would get if it were listed
in the table property [`zstd_compression_columns`](../../sql-statements/table_bucket_part_index/CREATE_TABLE.md).

The estimate is produced by doing what the storage layer does. The column's values
are concatenated into a sample, the sample is cut into pages of the configured
`data_page_size`, and every page is compressed three ways: with LZ4, with ZSTD, and
with ZSTD against a dictionary taken from the first page. Comparing the three totals
tells you whether the property is worth setting on this column.

This function is supported from v4.2.

## Syntax

```Haskell
ZSTD_COMPRESSION_GAIN(<expr>)
```

## Arguments

`expr`: a column of CHAR, VARCHAR, STRING, or JSON type. NULL values are counted and
skipped.

## Returns

A VARCHAR holding a JSON object:

| Field | Meaning |
| --- | --- |
| `rows` / `null_rows` | Rows read, and how many of them were NULL. |
| `total_bytes` / `avg_row_bytes` | Raw size of every non-NULL value, and the average per row. |
| `sampled_rows` / `sampled_bytes` | How much of the column was actually measured. |
| `page_bytes` / `sampled_pages` | The page size used, and how many pages the sample filled. |
| `measured_pages` | How many of those pages the three totals below cover — every page except the first. |
| `lz4_bytes` | What the sample costs with LZ4, the default codec. |
| `zstd_bytes` | What it costs with ZSTD and no dictionary. |
| `zstd_with_dict_bytes` | What it costs with ZSTD and a dictionary — this is what the property produces. |
| `dict_bytes` | Size of the dictionary itself, stored once per column per segment. |
| `times_smaller_than_lz4` / `times_smaller_than_zstd` | `zstd_with_dict_bytes` against the two baselines. |
| `dictionary_kept` | Whether the writer would actually keep the dictionary at the default page size, applying the rule it uses (`zstd_compression_dict_min_gain`). |
| `suggested_page_size` | The page size to write after the column name in `zstd_compression_columns`. |
| `suggested_times_smaller_than_lz4` | What the column would cost at that page size, against LZ4. |
| `page_size_options` | One entry per candidate page size: how many rows a page holds (what a point lookup decompresses), the cost with and without a dictionary, and which the writer would choose. |
| `suggestion` | A plain reading of those numbers. |

Only the first 8 MB of the column is measured, however large the column is. That is
deliberate: the dictionary the writer builds also comes from one segment's worth of
data, not from the whole table.

`dict_bytes` is reported next to the totals rather than folded into them. A real
segment holds far more pages than this sample does, so charging the dictionary
against a handful of pages would understate the gain.

The head of the sample is reserved for the dictionary and left out of every
measurement, so that all candidate page sizes are compared over the same bytes --
leaving out one page of each size instead would exclude a different fraction of the
sample for each, and a larger page would look better for that reason alone. A sample
with nothing left after the reserved head yields no estimate, and says so.

`suggested_page_size` is the smallest candidate within 5% of the best, because the
page is what a point lookup decompresses: a few percent of ratio is not worth
multiplying the cost of reading a single row. Compare `rows_per_page` across
`page_size_options` to see that cost.

Two limits worth knowing. The estimate compresses the column's values as they are,
not the encoded page the writer builds, so on very short rows -- where the offset
array is a large part of a page -- its verdict on the dictionary can differ from the
writer's. And the writer decides from the first pages of each column in each
segment, while this reads a bounded sample of the whole column; on a column whose
content changes over time the two can disagree.

## Examples

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

Reading it: this column costs about 4.2x less with the property than it does today,
so it is worth setting.

```sql
ALTER TABLE spans SET ("zstd_compression_columns" = "input");
```

A column whose rows have little in common reads the other way:

```plaintext
mysql> SELECT zstd_compression_gain(request_id) FROM spans;
+-----------------------------------------------------------------+
| zstd_compression_gain(request_id)                                |
+-----------------------------------------------------------------+
| {..."times_smaller_than_lz4":1.03,"times_smaller_than_zstd":1.01,
|  "suggestion":"little to gain, leave the column as it is"}       |
+-----------------------------------------------------------------+
```

## Keywords

ZSTD_COMPRESSION_GAIN, COMPRESSION, ZSTD
