-- name: test_position_v6_insert_multi_group_id
-- Reproduce: INSERT into table partitioned by varchar(group_id) with multiple distinct group_id values
-- Bug: "The row is out of partition ranges" when inserting rows with different group_id in one batch
DROP DATABASE IF EXISTS test_position_v6_${uuid0};
CREATE DATABASE test_position_v6_${uuid0};
use test_position_v6_${uuid0};

CREATE TABLE `position_v6` (
  `group_id` varchar(100) NOT NULL,
  `position_id` bigint(20) NOT NULL,
  `location_country` varchar(100) NULL,
  `job_function` varchar(100) NULL,
  `business_unit` varchar(100) NULL,
  `position_type` varchar(50) NULL,
  `skills_json` json NULL,
  `attributes_json` json NULL
) ENGINE=OLAP
PRIMARY KEY(`group_id`, `position_id`)
PARTITION BY (`group_id`)
DISTRIBUTED BY HASH(`position_id`) BUCKETS 2
ORDER BY(`location_country`, `job_function`, `business_unit`)
PROPERTIES (
  "replication_num" = "1"
);

INSERT INTO position_v6 (
  group_id, position_id, position_type, location_country, job_function, business_unit, skills_json, attributes_json
) VALUES
  ('eightfolddemo-hiring-test.com', 1001, 'role', 'US', 'eng', 'hr', '[{"name":"java","skill_group":"eng"}]', '{"role_approval_status":["approved"]}'),
  ('eightfolddemo-hiringtest.com', 1002, 'role', 'US', 'eng', 'hr', '[{"name":"java","skill_group":"eng"}]', '{"role_approval_status":["approved"]}'),
  ('eightfolddemo-hiringtest.com', 1003, 'role', 'US', 'eng', 'hr', '[{"name":"java","skill_group":"eng"}]', '{"role_approval_status":["approved"]}'),
  ('eightfolddemo-hiring-test.com', 1004, 'role', 'US', 'eng', 'hr', '[{"name":"java","skill_group":"eng"}]', '{"role_approval_status":["approved"]}');

[ORDER]SELECT group_id, position_id, location_country, job_function, business_unit, position_type
FROM position_v6 ORDER BY group_id, position_id;
-- result:
eightfolddemo-hiring-test.com	1001	US	eng	hr	role
eightfolddemo-hiring-test.com	1004	US	eng	hr	role
eightfolddemo-hiringtest.com	1002	US	eng	hr	role
eightfolddemo-hiringtest.com	1003	US	eng	hr	role
-- !result

DROP DATABASE test_position_v6_${uuid0};
