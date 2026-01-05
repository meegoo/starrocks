-- Case: 复现 date_trunc 分区函数在分区名不匹配但范围重叠时的报错行为
-- 目的: 验证当自动计算出的分区名不存在（因为被重命名了），但对应的 Range 已经存在时，插入是否会失败。
-- 预期: 修复前会报错 "Range intersected"; 修复后应该成功插入。

CREATE DATABASE IF NOT EXISTS test_db;
USE test_db;

DROP TABLE IF EXISTS t_date_trunc_issue;

CREATE TABLE t_date_trunc_issue (
    k1 DATETIME,
    v1 INT
)
ENGINE=OLAP
DUPLICATE KEY(k1)
PARTITION BY date_trunc('day', k1)
DISTRIBUTED BY HASH(k1) BUCKETS 1
PROPERTIES (
    "replication_num" = "1"
);

-- 1. 插入数据，自动创建分区 [2024-01-01, 2024-01-02)
--    默认生成的日分区名为 p20240101
INSERT INTO t_date_trunc_issue VALUES ('2024-01-01 10:00:00', 1);

-- 2. 重命名分区，模拟"分区名不存在但Range已存在"的场景
--    这模拟了并发场景下或者其他导致名字不一致场景下的状态
ALTER TABLE t_date_trunc_issue RENAME PARTITION p20240101 p_renamed;

-- 3. 再次插入相同时间范围的数据
--    BE/FE 会尝试自动创建标准分区名 p20240101
--    FE 检查 p20240101 不存在 -> 尝试创建 Range
--    FE 发现 Range [2024-01-01, 2024-01-02) 与 p_renamed 重叠
--    结果：报错 DdlException: Range intersected
INSERT INTO t_date_trunc_issue VALUES ('2024-01-01 11:00:00', 2);

SELECT * FROM t_date_trunc_issue ORDER BY k1;
