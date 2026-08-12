---
displayed_sidebar: docs
description: "zstd_compression_gain 估算某个大文本列或 JSON 列启用 zstd_compression_columns 后能小多少。"
---

# zstd_compression_gain

估算某个大文本列或 JSON 列在被写入表属性
[`zstd_compression_columns`](../../sql-statements/table_bucket_part_index/CREATE_TABLE.md)
之后能小多少。

估算的做法就是把存储层做的事重做一遍：把该列的值拼成一份样本，按配置的 `data_page_size`
切成页，然后对每一页分别用三种方式压缩——LZ4、ZSTD、以及以第一页为字典的 ZSTD。对比这三个
总量，就能看出这一列值不值得开这个属性。

该函数从 4.2 版开始支持。

## 语法

```Haskell
ZSTD_COMPRESSION_GAIN(<expr>)
```

## 参数说明

`expr`：CHAR、VARCHAR、STRING 或 JSON 类型的列。NULL 值会被计数并跳过。

## 返回值说明

返回一个 VARCHAR，内容是一个 JSON 对象：

| 字段 | 含义 |
| --- | --- |
| `rows` / `null_rows` | 读到的行数，以及其中 NULL 的行数。 |
| `total_bytes` / `avg_row_bytes` | 所有非 NULL 值的原始字节数，以及平均每行字节数。 |
| `sampled_rows` / `sampled_bytes` | 实际参与估算的行数和字节数。 |
| `page_bytes` / `sampled_pages` | 使用的页大小，以及样本占了多少页。 |
| `measured_pages` | 下面三个总量覆盖了其中多少页——除第一页以外的所有页。 |
| `lz4_bytes` | 样本在 LZ4（默认压缩算法）下的大小。 |
| `zstd_bytes` | 样本在不带字典的 ZSTD 下的大小。 |
| `zstd_with_dict_bytes` | 样本在带字典的 ZSTD 下的大小——这就是开启该属性后的结果。 |
| `dict_bytes` | 字典本身的大小，每列每个 Segment 存一份。 |
| `times_smaller_than_lz4` / `times_smaller_than_zstd` | `zstd_with_dict_bytes` 相对两个基准分别小多少倍。 |
| `dictionary_kept` | 在默认页大小下，写入端是否真的会保留字典（用它自己那条规则 `zstd_compression_dict_min_gain`）。 |
| `suggested_page_size` | 建议写在 `zstd_compression_columns` 里列名后面的页大小。 |
| `suggested_times_smaller_than_lz4` | 采用该页大小后，相对 LZ4 小多少倍。 |
| `page_size_options` | 每个候选页大小一项：一页装多少行（即一次点查要解压多少）、带与不带字典各多大、写入端会选哪个。 |
| `suggestion` | 对上述数字的直白解读。 |

无论列有多大，只估算前 8 MB。这是有意为之：写入端构建字典用的也是一个 Segment 量级的数据，
而不是整张表。

`dict_bytes` 单独列出而不计入总量。真实 Segment 的页数远多于这份样本，把字典摊到这么几页上
会低估收益。

样本开头有一段被留作字典来源，并从所有测量中排除，这样各候选页大小才是在**同一批字节**上比较——
若改成「各自排除一个页」，每种页大小排除掉的比例都不同，大页会仅因这一点就显得更好。排除之后
无剩余内容的样本不给出估算，并直接说明原因。

`suggested_page_size` 取的是「距最优 5% 以内的最小候选」，因为页正是一次点查要解压的单位：
几个百分点的压缩率，不值得把读一行的代价成倍放大。各选项里的 `rows_per_page` 就是这个代价。

两条局限需要知道。一是本估算压缩的是列的**原始值**，而不是写入端真正构建的编码页；因此在行非常短
（偏移数组占一页相当比例）的列上，它对字典的判断可能与写入端不同。二是写入端按「每列每个 Segment
的头几个页」判定，而这里读的是整列的有界样本；如果列的内容随时间变化，两者可能给出不同结论。

## 示例

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

这个结果的含义是：这一列开启该属性后大约能小 4.2 倍，值得开。

```sql
ALTER TABLE spans SET ("zstd_compression_columns" = "input");
```

而行与行之间没什么共同内容的列，结论则相反：

```plaintext
mysql> SELECT zstd_compression_gain(request_id) FROM spans;
+-----------------------------------------------------------------+
| zstd_compression_gain(request_id)                                |
+-----------------------------------------------------------------+
| {..."times_smaller_than_lz4":1.03,"times_smaller_than_zstd":1.01,
|  "suggestion":"little to gain, leave the column as it is"}       |
+-----------------------------------------------------------------+
```

## 关键字

ZSTD_COMPRESSION_GAIN, COMPRESSION, ZSTD
