---
displayed_sidebar: docs
---

# seconds_diff

2 つの日付式（`expr1` − `expr2`）の秒単位の差を返します。秒単位で正確です。

## Syntax

```Haskell
BIGINT seconds_diff(DATETIME expr1,DATETIME expr2);
```

## Parameters

- `expr1`: 終了時刻。DATETIME 型でなければなりません。

- `expr2`: 開始時刻。DATETIME 型でなければなりません。

## Return value

BIGINT 値を返します。

日付が存在しない場合、例えば 2022-02-29 の場合は、NULL が返されます。

## Examples

```Plain
select seconds_diff('2010-11-30 23:59:59', '2010-11-30 20:59:59');
+------------------------------------------------------------+
| seconds_diff('2010-11-30 23:59:59', '2010-11-30 20:59:59') |
+------------------------------------------------------------+
|                                                      10800 |
+------------------------------------------------------------+
```