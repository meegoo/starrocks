---
displayed_sidebar: docs
sidebar_position: 50
---

# ユニークキーテーブル

テーブル作成時にユニークキーを定義する必要があります。同じユニークキーを持つ複数のデータ行がある場合、値カラムの値は置き換えられます。クエリ時には、同じユニークキーを持つデータグループの最新データが返されます。さらに、ソートキーを別途定義することもできます。クエリのフィルター条件にソートキーが含まれている場合、StarRocks はデータを迅速にフィルタリングし、クエリ効率を向上させます。

ユニークキーテーブルは、リアルタイムかつ頻繁なデータ更新をサポートできます。しかし、徐々に[主キーテーブル](./primary_key_table.md)に置き換えられつつあります。

## シナリオ

ユニークキーテーブルは、データをリアルタイムで頻繁に更新する必要があるビジネスシナリオに適しています。例えば、e コマースのシナリオでは、1 日に数億件の注文が行われ、注文のステータスが頻繁に変更されます。

## 原理

ユニークキーテーブルは、同じユニークキーを持つレコードグループの中で最新のレコードを返すために、値カラムに REPLACE 集計関数が指定された特別な集計テーブルと考えることができます。

ユニークキーテーブルにデータをロードする際、データは複数のバッチに分割されます。各バッチにはバージョン番号が割り当てられます。したがって、同じユニークキーを持つレコードは複数のバージョンに含まれることがあります。クエリには最新バージョンのデータ（つまり、最大のバージョン番号を持つレコード）が返されます。

次の表に示すように、`ID` はユニークキー、`value` は値カラム、`_version` は StarRocks 内で生成されたデータバージョン番号を保持します。この例では、`ID` が 1 のレコードはバージョン番号が `1` と `2` の 2 つのバッチによってロードされ、`ID` が `2` のレコードはバージョン番号が `3`、`4`、`5` の 3 つのバッチによってロードされます。

| ID   | value | _version |
| ---- | ----- | -------- |
| 1    | 100   | 1        |
| 1    | 101   | 2        |
| 2    | 100   | 3        |
| 2    | 101   | 4        |
| 2    | 102   | 5        |

`ID` が `1` のレコードをクエリすると、最大のバージョン番号を持つ最新のレコード、つまりこの場合は `2` が返されます。`ID` が `2` のレコードをクエリすると、最大のバージョン番号を持つ最新のレコード、つまりこの場合は `5` が返されます。次の表は、2 つのクエリによって返されるレコードを示しています。

| ID   | value |
| ---- | ----- |
| 1    | 101   |
| 2    | 102   |

## テーブルの作成

e コマースのシナリオでは、注文のステータスを日付ごとに収集して分析する必要があります。この例では、注文を保持するために `orders` という名前のテーブルを作成し、注文をフィルタリングする条件として頻繁に使用される `create_time` と `order_id` をユニークキーカラムとして定義し、他の 2 つのカラム `order_state` と `total_price` を値カラムとして定義します。これにより、注文のステータスが変わるたびにリアルタイムで更新され、クエリを加速するために迅速にフィルタリングできます。

テーブル作成のステートメントは次のとおりです。

```SQL
CREATE TABLE orders (
    create_time DATE NOT NULL COMMENT "create time of an order",
    order_id BIGINT NOT NULL COMMENT "id of an order",
    order_state INT COMMENT "state of an order",
    total_price BIGINT COMMENT "price of an order"
)
UNIQUE KEY(create_time, order_id)
DISTRIBUTED BY HASH(order_id);
```

> **注意**
>
> - テーブルを作成する際には、`DISTRIBUTED BY HASH` 句を使用してバケット化カラムを指定する必要があります。詳細情報は [bucketing](../data_distribution/Data_distribution.md#bucketing) を参照してください。
> - バージョン 2.5.7 以降、StarRocks はテーブルを作成する際やパーティションを追加する際に、バケット数 (BUCKETS) を自動的に設定できます。バケット数を手動で設定する必要はありません。詳細情報は [set the number of buckets](../data_distribution/Data_distribution.md#set-the-number-of-buckets) を参照してください。

## 使用上の注意

- **ユニークキー**:
  - CREATE TABLE ステートメントでは、ユニークキーは他のカラムの前に定義する必要があります。
  - ユニークキーは `UNIQUE KEY` を使用して明示的に定義する必要があります。
  - ユニークキーには一意性制約があります。

- **ソートキー**:

  - バージョン 3.3.0 以降、ソートキーはユニークキーテーブルのユニークキーから分離されています。ユニークキーテーブルは `ORDER BY` を使用してソートキーを指定し、`UNIQUE KEY` を使用してユニークキーを指定することをサポートしています。ソートキーとユニークキーのカラムは同じである必要がありますが、カラムの順序は同じである必要はありません。

  - クエリ時には、集計前にソートキーに基づいてデータをフィルタリングできます。ただし、マルチバージョン集計後に値カラムに基づいてデータをフィルタリングできます。したがって、集計前にデータをフィルタリングしてクエリパフォーマンスを向上させるために、頻繁にフィルタリングされるフィールドをソートキーとして使用することをお勧めします。

- テーブルを作成する際には、テーブルのキー列に対してのみビットマップインデックスまたはブルームフィルターインデックスを作成できます。

## 次のステップ

テーブルが作成された後、さまざまなデータ取り込み方法を使用して StarRocks にデータをロードできます。StarRocks がサポートするデータ取り込み方法については、[Loading options](../../loading/Loading_intro.md) を参照してください。

:::note

- ユニークキーテーブルを使用するテーブルにデータをロードする際には、テーブルのすべてのカラムを更新する必要があります。例えば、前述の `orders` テーブルを更新する際には、`create_time`、`order_id`、`order_state`、`total_price` のすべてのカラムを更新する必要があります。
- ユニークキーテーブルを使用するテーブルからデータをクエリする際には、StarRocks は複数のデータバージョンのレコードを集計する必要があります。この状況では、多数のデータバージョンがクエリパフォーマンスを低下させます。したがって、リアルタイムデータ分析の要件を満たしつつ、多数のデータバージョンを防ぐために、テーブルにデータをロードする適切な頻度を指定することをお勧めします。分単位のデータが必要な場合は、1 秒のロード頻度ではなく、1 分のロード頻度を指定できます。

:::