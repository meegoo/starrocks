# CLAUDE.md - AI Assistant Guide for StarRocks

This document provides comprehensive guidance for AI assistants working on the StarRocks codebase. It covers architecture, development workflows, conventions, and best practices.

---

## Table of Contents

1. [Project Overview](#project-overview)
2. [Architecture](#architecture)
3. [Directory Structure](#directory-structure)
4. [Development Workflows](#development-workflows)
5. [Code Conventions](#code-conventions)
6. [Testing Practices](#testing-practices)
7. [Common Tasks](#common-tasks)
8. [Important Considerations](#important-considerations)
9. [Resources](#resources)

---

## Project Overview

**StarRocks** is an open-source, high-performance analytical database system designed for sub-second, real-time analytics on both data lakehouses and traditional data warehouses. It is a Linux Foundation project.

### Key Characteristics

- **Languages**: C++ (Backend), Java (Frontend), Python (Testing)
- **Architecture**: Two-tier system with Frontend (FE) and Backend (BE)
- **Performance**: 3x faster than alternatives with vectorized query execution
- **Scale**: Production deployments at Airbnb, Pinterest, Tencent, Alibaba, and many others
- **Build System**: CMake (BE), Maven/Gradle (FE), Custom shell scripts
- **Testing**: Google Test (BE), JUnit (FE), Custom Python framework (Integration)

### Version Information

- Current version uses git tags/branches for versioning
- Supports versions 3.3, 3.4, 3.5, 4.0 for backporting
- Version 3.0+ supports shared-data architecture

---

## Architecture

### High-Level Design

StarRocks uses a **streamlined two-tier architecture**:

```
┌─────────────────────────────────────────┐
│          Frontend (FE) - Java           │
│  SQL Parser │ Optimizer │ Meta Manager  │
│  Planner    │ Scheduler │ Catalog       │
└──────────────────┬──────────────────────┘
                   │ RPC (Thrift)
┌──────────────────▼──────────────────────┐
│         Backend (BE) - C++              │
│  Execution │ Storage │ Connectors       │
│  Pipeline  │ Rowsets │ Format Readers   │
└─────────────────────────────────────────┘
```

### Frontend (FE) - Java Components

**Purpose**: SQL processing, query optimization, and metadata management

**Key Responsibilities**:
- **SQL Processing**: ANTLR-based parser → AST → Semantic analyzer → Type checker
- **Query Optimization**: Cost-based optimizer (CBO) with rule transformations
- **Metadata Management**: Catalog, schema, partitions, tablets
- **Query Planning**: Generate physical execution plans
- **Scheduling**: Coordinate query execution across BE nodes
- **Authentication**: User management, permissions, RBAC

**Location**: `/fe/`

**Sub-modules**:
- `fe-core/` - Core business logic
- `fe-grammar/` - SQL grammar (.g4 ANTLR files)
- `fe-parser/` - Parser and AST generation
- `fe-type/` - Type system
- `fe-spi/` - Service Provider Interface for plugins
- `fe-utils/` - Utility libraries

### Backend (BE) - C++ Components

**Purpose**: High-performance query execution and data storage

**Key Responsibilities**:
- **Query Execution**: Vectorized pipeline execution engine
- **Storage Management**: Rowset-based storage with indexes
- **Data Loading**: Stream load, broker load, routine load
- **Format Support**: Parquet, ORC, Avro, CSV, JSON readers
- **External Connectors**: Hive, Iceberg, Hudi, Delta Lake, Kudu
- **Resource Management**: Memory, CPU, I/O management

**Location**: `/be/`

**Key Directories**:
- `be/src/exec/` - Execution engine (pipeline, operators)
- `be/src/storage/` - Storage layer (rowsets, indexes, tablets)
- `be/src/formats/` - File format parsers
- `be/src/connector/` - External data source connectors
- `be/src/exprs/` - Expression evaluation
- `be/src/runtime/` - Runtime components (memory, batch processing)
- `be/src/service/` - Core services (HTTP, RPC)

### Communication Layer

**FE ↔ BE Communication**:
- **Protocol**: Thrift RPC (primary), BRPC (high-performance scenarios)
- **Definitions**: `/gensrc/thrift/` contains service definitions
- **Data Structures**: Protocol Buffers in `/gensrc/proto/`

**JNI Bridge** (Java ↔ C++):
- `/be/src/udf/java/` - Java UDF support in BE
- `/java-extensions/jni-connector/` - Data exchange between FE and BE

### Shared-Data Architecture (v3.0+)

- **StarOS/Lake**: Shared storage layer
- **Compute Nodes (CN)**: Stateless compute nodes for elastic scaling
- **Format SDK**: `/format-sdk/` for data interchange

---

## Directory Structure

### Root Level Overview

```
starrocks/
├── be/                      # Backend (C++) - Execution engine & storage
├── fe/                      # Frontend (Java) - SQL processing & optimization
├── java-extensions/         # Java-based connectors & extensions
├── test/                    # Integration tests (Python-based)
├── gensrc/                  # Generated code (Thrift, Proto)
├── thirdparty/              # Third-party dependencies
├── conf/                    # Configuration templates
├── bin/                     # Deployment scripts
├── docs/                    # Documentation (Docusaurus)
├── tools/                   # Diagnostic & benchmark tools
├── build.sh                 # Main build script
├── run-be-ut.sh             # Backend unit test runner
├── run-fe-ut.sh             # Frontend unit test runner
└── CLAUDE.md                # This file
```

### Backend Structure (`/be/`)

```
be/
├── src/
│   ├── exec/               # Execution engine
│   │   ├── pipeline/       # Pipeline execution framework
│   │   ├── vectorized/     # Vectorized operators
│   │   └── sorting/        # Sort algorithms
│   ├── storage/            # Storage layer
│   │   ├── rowset/         # Rowset management
│   │   ├── lake/           # Shared-data storage
│   │   ├── index/          # Indexing structures
│   │   └── compaction/     # Compaction strategies
│   ├── formats/            # File format readers
│   │   ├── parquet/        # Apache Parquet
│   │   ├── orc/            # Apache ORC
│   │   ├── csv/            # CSV parser
│   │   └── json/           # JSON parser
│   ├── connector/          # External connectors
│   ├── exprs/              # Expression evaluation
│   ├── runtime/            # Runtime components
│   ├── http/               # HTTP service layer
│   ├── service/            # Core services
│   ├── common/             # Common utilities
│   ├── util/               # Utility functions
│   └── gutil/              # Google utilities
├── test/                   # Unit tests (mirrors src/)
└── CMakeLists.txt          # CMake build configuration
```

### Frontend Structure (`/fe/`)

```
fe/
├── fe-core/
│   └── src/main/java/com/starrocks/
│       ├── sql/                    # SQL processing
│       │   ├── parser/             # Parser classes
│       │   ├── analyzer/           # Semantic analysis
│       │   ├── optimizer/          # Query optimizer (CBO)
│       │   ├── ast/                # Abstract syntax tree
│       │   └── plan/               # Physical plan nodes
│       ├── planner/                # Query planner
│       ├── qe/                     # Query execution coordinator
│       ├── catalog/                # Metadata catalog
│       ├── connector/              # External connectors
│       ├── mv/                     # Materialized views
│       ├── server/                 # Server components
│       ├── authentication/         # Auth & security
│       ├── privilege/              # Permission management
│       ├── common/                 # Common utilities
│       └── statistic/              # Statistics collection
├── fe-grammar/                     # ANTLR SQL grammar
├── fe-parser/                      # Parser implementation
├── fe-type/                        # Type system
├── fe-spi/                         # Plugin interfaces
└── pom.xml                         # Maven configuration
```

### Java Extensions (`/java-extensions/`)

```
java-extensions/
├── hive-reader/                # Hive table reader
├── iceberg-metadata-reader/    # Apache Iceberg
├── hudi-reader/                # Apache Hudi
├── paimon-reader/              # Apache Paimon
├── jdbc-bridge/                # JDBC connectivity
├── kudu-reader/                # Apache Kudu
├── odps-reader/                # Alibaba MaxCompute
├── udf-extensions/             # User-defined functions
├── hadoop-ext/                 # Hadoop integration
├── jni-connector/              # JNI bridge
└── pom.xml                     # Maven parent POM
```

### Test Structure (`/test/`)

```
test/
├── sql/                        # SQL test cases
│   ├── test_agg/              # Aggregation tests
│   ├── test_join/             # Join tests
│   ├── test_mv/               # Materialized view tests
│   ├── test_external/         # External table tests
│   └── [195+ test suites]
├── common/                     # Test utilities
├── lib/                        # Test libraries
└── run.py                      # Test runner
```

Each test suite contains:
- `T/` - Test SQL scripts
- `R/` - Expected results
- `data/` - Test data files

---

## Development Workflows

### Initial Setup

1. **Clone Repository**:
   ```bash
   git clone https://github.com/StarRocks/starrocks.git
   cd starrocks
   ```

2. **Environment Setup**:
   ```bash
   source env.sh  # Sets up STARROCKS_HOME and other variables
   ```

3. **Build Third-party Dependencies** (first time only):
   ```bash
   cd thirdparty
   ./build-thirdparty.sh  # Takes significant time!
   ```

### Building the Project

#### ⚠️ CRITICAL: Build System Warning

**DO NOT build unless explicitly required!** The build process is:
- **Time-intensive**: Full build can take 1-2 hours
- **Resource-intensive**: Requires 16GB+ RAM, multiple CPU cores
- **Disk-intensive**: Requires 50GB+ free space

#### Build Commands

```bash
# Build everything (NOT RECOMMENDED - very slow)
./build.sh

# Build Backend only
./build.sh --be

# Build Frontend only
./build.sh --fe

# Build with specific type
BUILD_TYPE=Debug ./build.sh --be    # Debug build
BUILD_TYPE=Release ./build.sh --be  # Release build (default)
BUILD_TYPE=Asan ./build.sh --be     # Address sanitizer build

# Clean build
./build.sh --fe --be --clean

# Build with shared-data support
./build.sh --be --enable-shared-data

# Build with code coverage
./build.sh --be --with-gcov
```

#### Build Options Reference

- `--be` - Build Backend only
- `--fe` - Build Frontend only
- `--format-lib` - Build format library (shared-data mode)
- `--spark-dpp` - Build Spark DPP application
- `--hive-udf` - Build Hive UDF support
- `--clean` - Clean before building
- `--enable-shared-data` - Enable shared-data features
- `--with-gcov` - Enable code coverage
- `--without-java-ext` - Skip Java extensions
- `--without-starcache` - Skip StarCache

### Running Tests

#### Backend Unit Tests

```bash
# Run all BE unit tests (NOT RECOMMENDED - very slow)
./run-be-ut.sh

# Run specific test
./run-be-ut.sh --test storage_tablet_test

# Run with pattern
./run-be-ut.sh --gtest_filter="TabletTest.*"

# Run with code coverage
./run-be-ut.sh --with-gcov

# Run in parallel
./run-be-ut.sh -j 8
```

#### Frontend Unit Tests

```bash
# Run all FE unit tests (SLOW!)
./run-fe-ut.sh

# Run specific test class
./run-fe-ut.sh --test com.starrocks.sql.optimizer.OptimizerTest

# Run with coverage
./run-fe-ut.sh --coverage

# Run with profiler
./run-fe-ut.sh --enable-profiler

# Run in parallel
./run-fe-ut.sh -j 8
```

#### Integration Tests (SQL Tests)

```bash
cd test
# Run all tests (VERY SLOW!)
python3 run.py

# Run specific test suite
python3 run.py --run test_agg

# Record mode (generate expected results)
python3 run.py --run test_agg --record

# Validate mode (compare results)
python3 run.py --run test_agg --validate

# Filter by tags
python3 run.py -a "quick"

# Run with specific concurrency
python3 run.py --parallel 4
```

### Git Workflow

#### Branch Strategy

- **main/master** - Main development branch
- **branch-3.x** - Release branches (3.3, 3.4, 3.5, 4.0)
- **feature/** - Feature development branches
- **bugfix/** - Bug fix branches

#### Commit Message Format

```
[Category] Brief description (50 chars or less)

Detailed explanation of changes and rationale.
Wrap at 72 characters.

- Key change 1
- Key change 2
- Key change 3

Fixes: #issue_number
Closes: #issue_number
```

**Categories**:
- `[BugFix]` - Bug fixes
- `[Feature]` - New features
- `[Enhancement]` - Improvements to existing features
- `[Refactor]` - Code refactoring
- `[Test]` - Test changes
- `[Doc]` - Documentation
- `[Build]` - Build system changes
- `[Performance]` - Performance optimizations

**Example**:
```
[Feature] Add Apache Iceberg table format support

Implement Iceberg connector to enable querying Iceberg tables
directly from StarRocks including metadata reading, partition
pruning, and schema evolution support.

- Add IcebergConnector and IcebergMetadata classes
- Implement partition and file pruning optimizations
- Support for Iceberg v1 and v2 table formats
- Add comprehensive unit tests

Closes: #12345
```

#### Pull Request Workflow

1. **Fork and Clone**:
   ```bash
   # Fork on GitHub, then:
   git clone https://github.com/YOUR_USERNAME/starrocks.git
   cd starrocks
   git remote add upstream https://github.com/StarRocks/starrocks.git
   ```

2. **Create Feature Branch**:
   ```bash
   git checkout -b feature/my-feature
   ```

3. **Make Changes and Commit**:
   ```bash
   git add .
   git commit -m "[Feature] Add my feature"
   ```

4. **Push and Create PR**:
   ```bash
   git push origin feature/my-feature
   # Create PR on GitHub
   ```

5. **PR Requirements**:
   - Follow PR template (`.github/PULL_REQUEST_TEMPLATE.md`)
   - Add appropriate labels (BugFix, Feature, Enhancement, etc.)
   - Link related issues
   - Include test cases
   - Update documentation if needed
   - Sign CLA (required once)

6. **Backporting**:
   - Check version labels for auto-backporting
   - Supported versions: 3.3, 3.4, 3.5, 4.0

---

## Code Conventions

### C++ (Backend)

#### Style Guide

- **Base Style**: Google C++ Style Guide
- **Configuration**: `.clang-format` in root directory
- **Column Limit**: 120 characters
- **Indentation**: 4 spaces
- **Pointer Alignment**: Left (`int* ptr` not `int *ptr`)

#### Formatting

```bash
# Format C++ code
clang-format -i be/src/path/to/file.cpp
```

#### Key Conventions

**Naming**:
- Classes: `PascalCase` (e.g., `TabletManager`)
- Functions: `snake_case` (e.g., `load_tablet()`)
- Variables: `snake_case` (e.g., `tablet_id`)
- Constants: `UPPER_SNAKE_CASE` (e.g., `MAX_TABLET_SIZE`)
- Private members: `_member_name` with leading underscore

**File Organization**:
- Header files: `.h` extension
- Implementation: `.cpp` extension
- Each class in its own file pair
- Include guards: `#pragma once`

**Memory Management**:
- Prefer smart pointers (`std::unique_ptr`, `std::shared_ptr`)
- Use RAII patterns
- Avoid raw `new`/`delete`
- Manual memory management only in performance-critical paths

**Error Handling**:
- Return `Status` objects for operations that can fail
- Use `StatusOr<T>` for returning values with status
- Log errors appropriately

**Example**:
```cpp
// tablet_manager.h
#pragma once

#include <memory>
#include "common/status.h"

namespace starrocks {

class TabletManager {
public:
    TabletManager();
    ~TabletManager();

    Status create_tablet(int64_t tablet_id);
    StatusOr<TabletSharedPtr> get_tablet(int64_t tablet_id);

private:
    std::unordered_map<int64_t, TabletSharedPtr> _tablets;
};

} // namespace starrocks
```

### Java (Frontend)

#### Style Guide

- **Base Style**: Google Java Style Guide
- **Configuration**: `fe/checkstyle.xml`
- **Column Limit**: 120 characters
- **Indentation**: 4 spaces

#### Formatting

```bash
# Check style
cd fe
mvn checkstyle:check
```

#### Key Conventions

**Naming**:
- Classes: `PascalCase` (e.g., `QueryOptimizer`)
- Methods: `camelCase` (e.g., `optimizeQuery()`)
- Variables: `camelCase` (e.g., `queryPlan`)
- Constants: `UPPER_SNAKE_CASE` (e.g., `MAX_QUERY_TIMEOUT`)
- Packages: lowercase (e.g., `com.starrocks.sql.optimizer`)

**Package Organization**:
- Core logic: `com.starrocks.sql.*`
- Catalog: `com.starrocks.catalog.*`
- Connectors: `com.starrocks.connector.*`
- Common: `com.starrocks.common.*`

**Exception Handling**:
- Use specific exception types
- Provide meaningful error messages
- Log exceptions with context

**Example**:
```java
package com.starrocks.sql.optimizer;

import com.starrocks.sql.plan.PlanNode;
import com.starrocks.common.AnalysisException;

public class QueryOptimizer {
    private static final int MAX_OPTIMIZATION_ROUNDS = 10;

    public PlanNode optimize(PlanNode inputPlan) throws AnalysisException {
        if (inputPlan == null) {
            throw new AnalysisException("Input plan cannot be null");
        }
        // Optimization logic
        return optimizedPlan;
    }

    private void validatePlan(PlanNode plan) {
        // Validation logic
    }
}
```

### SQL Test Format

#### Directory Structure

```
test_suite_name/
├── T/
│   ├── test_case_1.sql
│   └── test_case_2.sql
├── R/
│   ├── test_case_1.sql.out
│   └── test_case_2.sql.out
└── data/
    └── test_data.csv
```

#### Test Case Format

```sql
-- name: test_basic_aggregation
-- Test basic aggregation with group by

CREATE TABLE t1 (
    id INT,
    value DECIMAL(10,2),
    category VARCHAR(50)
) DUPLICATE KEY(id);

INSERT INTO t1 VALUES
    (1, 100.00, 'A'),
    (2, 200.00, 'B'),
    (3, 150.00, 'A');

SELECT category, SUM(value) as total
FROM t1
GROUP BY category
ORDER BY category;

-- Expected result in R/ directory
```

---

## Testing Practices

### Test Organization

#### Backend Tests (`/be/test/`)

Tests mirror the source directory structure:

```
be/test/
├── column/              # Column tests
├── exec/                # Execution tests
│   ├── pipeline/       # Pipeline execution tests
│   └── vectorized/     # Vectorized operator tests
├── storage/            # Storage layer tests
├── formats/            # Format reader tests
├── exprs/              # Expression evaluation tests
└── util/               # Utility tests
```

#### Frontend Tests (`/fe/fe-core/src/test/java/`)

```
test/java/com/starrocks/
├── sql/
│   ├── analyzer/       # Semantic analysis tests
│   ├── optimizer/      # Optimizer tests
│   └── plan/           # Plan generation tests
├── catalog/            # Catalog tests
├── connector/          # Connector tests
└── qe/                 # Query execution tests
```

### Writing Tests

#### Backend Unit Test (Google Test)

```cpp
// be/test/storage/tablet_test.cpp
#include <gtest/gtest.h>
#include "storage/tablet.h"

namespace starrocks {

class TabletTest : public testing::Test {
protected:
    void SetUp() override {
        // Setup code
    }

    void TearDown() override {
        // Cleanup code
    }
};

TEST_F(TabletTest, BasicCreation) {
    auto tablet = std::make_shared<Tablet>();
    ASSERT_NE(tablet, nullptr);
    EXPECT_EQ(tablet->tablet_id(), 0);
}

TEST_F(TabletTest, LoadData) {
    auto tablet = create_test_tablet();
    Status st = tablet->load_rowset(create_test_rowset());
    ASSERT_TRUE(st.ok());
}

} // namespace starrocks
```

#### Frontend Unit Test (JUnit)

```java
// fe/fe-core/src/test/java/com/starrocks/sql/optimizer/OptimizerTest.java
package com.starrocks.sql.optimizer;

import org.junit.Assert;
import org.junit.Before;
import org.junit.Test;

public class OptimizerTest {
    private QueryOptimizer optimizer;

    @Before
    public void setUp() {
        optimizer = new QueryOptimizer();
    }

    @Test
    public void testBasicOptimization() {
        PlanNode input = createTestPlan();
        PlanNode result = optimizer.optimize(input);

        Assert.assertNotNull(result);
        Assert.assertTrue(result.getCost() <= input.getCost());
    }

    @Test(expected = AnalysisException.class)
    public void testInvalidPlan() {
        optimizer.optimize(null);
    }
}
```

#### Integration SQL Test

```sql
-- test/sql/test_agg/T/test_basic.sql
-- name: test_sum_aggregation

CREATE TABLE test_table (
    id INT,
    value BIGINT
) DUPLICATE KEY(id)
DISTRIBUTED BY HASH(id);

INSERT INTO test_table VALUES (1, 100), (2, 200), (3, 300);

SELECT SUM(value) FROM test_table;
-- Expected: 600

SELECT id, SUM(value) FROM test_table GROUP BY id ORDER BY id;
-- Expected: (1,100), (2,200), (3,300)
```

### Test Best Practices

1. **Test Naming**:
   - Descriptive names: `test_aggregation_with_null_values`
   - Clear intent: What is being tested?

2. **Test Organization**:
   - One test file per source file (BE)
   - Logical grouping by functionality (FE)
   - Clear setup and teardown

3. **Test Coverage**:
   - Happy path tests
   - Error cases
   - Edge cases (null, empty, max values)
   - Boundary conditions

4. **Test Independence**:
   - Tests should not depend on each other
   - Clean state between tests
   - No shared mutable state

5. **Performance Considerations**:
   - Keep unit tests fast (< 1 second)
   - Use mocks for external dependencies
   - Integration tests can be slower

---

## Common Tasks

### Adding a New Feature

1. **Plan the Feature**:
   - Review related code
   - Identify components to modify
   - Plan test strategy

2. **Implement in Backend (if needed)**:
   ```bash
   # Create new files in appropriate directory
   vim be/src/exec/my_new_operator.cpp
   vim be/src/exec/my_new_operator.h

   # Add to CMakeLists.txt if needed
   vim be/src/exec/CMakeLists.txt
   ```

3. **Implement in Frontend (if needed)**:
   ```bash
   # Create new Java classes
   vim fe/fe-core/src/main/java/com/starrocks/sql/optimizer/MyNewOptimizer.java
   ```

4. **Add Tests**:
   ```bash
   # Backend test
   vim be/test/exec/my_new_operator_test.cpp

   # Frontend test
   vim fe/fe-core/src/test/java/com/starrocks/sql/optimizer/MyNewOptimizerTest.java

   # Integration test
   mkdir -p test/sql/test_my_feature/{T,R}
   vim test/sql/test_my_feature/T/test_basic.sql
   ```

5. **Build and Test**:
   ```bash
   # Build affected component
   ./build.sh --be  # or --fe

   # Run unit tests
   ./run-be-ut.sh --test my_new_operator_test
   ./run-fe-ut.sh --test MyNewOptimizerTest

   # Run integration tests
   cd test
   python3 run.py --run test_my_feature --record
   ```

6. **Create PR**:
   - Follow commit message format
   - Fill out PR template
   - Link related issue

### Fixing a Bug

1. **Reproduce the Bug**:
   - Create a minimal test case
   - Understand the failure

2. **Locate the Issue**:
   - Use grep/search to find relevant code
   - Read surrounding code for context
   - Check recent changes with `git blame`

3. **Fix the Bug**:
   - Make minimal changes
   - Add comments explaining the fix
   - Consider edge cases

4. **Add Regression Test**:
   ```cpp
   // Add test that would have caught the bug
   TEST_F(MyComponentTest, RegressionTestForIssue12345) {
       // Test case that reproduces the original bug
       EXPECT_TRUE(fixed_behavior());
   }
   ```

5. **Verify Fix**:
   ```bash
   # Run affected tests
   ./run-be-ut.sh --test my_component_test

   # Run broader test suite
   ./run-be-ut.sh
   ```

6. **Create PR**:
   - Use `[BugFix]` prefix
   - Reference issue number: `Fixes: #12345`

### Adding a New Connector

1. **Backend Components** (if needed):
   ```bash
   # Create connector implementation
   mkdir -p be/src/connector/my_connector
   vim be/src/connector/my_connector/my_connector.cpp
   ```

2. **Frontend Components**:
   ```bash
   # Create connector classes
   mkdir -p fe/fe-core/src/main/java/com/starrocks/connector/myconnector
   vim fe/fe-core/src/main/java/com/starrocks/connector/myconnector/MyConnector.java
   vim fe/fe-core/src/main/java/com/starrocks/connector/myconnector/MyMetadata.java
   ```

3. **Java Extension** (for heavy operations):
   ```bash
   # Create Maven module
   mkdir -p java-extensions/my-connector
   vim java-extensions/my-connector/pom.xml
   ```

4. **Add Tests**:
   - Unit tests for connector logic
   - Integration tests with actual external system
   - SQL tests for end-to-end validation

5. **Documentation**:
   - Update connector documentation
   - Add examples
   - Document configuration options

### Code Navigation

#### Finding Code

```bash
# Find files by name pattern
find . -name "*tablet*" -type f

# Search for text in code
grep -r "class Tablet" --include="*.cpp" --include="*.h"

# Search for SQL keywords
grep -r "CREATE TABLE" test/sql/

# Find class definition
grep -r "^class TabletManager" be/src/
```

#### Using IDE

**IntelliJ IDEA (Frontend)**:
- Open `fe/pom.xml` as project
- Use Ctrl+N (Cmd+O on Mac) for class navigation
- Use Ctrl+Shift+N for file navigation
- Use Ctrl+Alt+B for implementation navigation

**CLion (Backend)**:
- Open `be/CMakeLists.txt` as project
- Use Ctrl+N for class navigation
- Use Ctrl+Shift+N for file navigation
- Use Ctrl+B for definition navigation

### Debugging

#### Backend Debugging

```bash
# Build with debug symbols
BUILD_TYPE=Debug ./build.sh --be

# Run with debugger
gdb --args be/output/bin/starrocks_be --flagfile=conf/be.conf

# Common GDB commands
(gdb) break tablet.cpp:123
(gdb) run
(gdb) backtrace
(gdb) print variable_name
(gdb) next
(gdb) continue
```

#### Frontend Debugging

```bash
# Run with remote debugging
JAVA_OPTS="-agentlib:jdwp=transport=dt_socket,server=y,suspend=n,address=5005" \
    bin/start_fe.sh

# Connect from IntelliJ IDEA:
# Run > Edit Configurations > + > Remote JVM Debug
# Host: localhost, Port: 5005
```

---

## Important Considerations

### Performance Considerations

1. **Vectorization**:
   - Backend operations should be vectorized
   - Process data in batches (typically 4096 rows)
   - Use SIMD instructions where possible

2. **Memory Management**:
   - Be aware of memory allocations in hot paths
   - Use object pools for frequently allocated objects
   - Monitor memory usage with metrics

3. **Query Optimization**:
   - CBO relies on statistics - ensure stats are up to date
   - Partition pruning is critical for performance
   - Predicate pushdown to storage layer

4. **I/O Efficiency**:
   - Minimize disk I/O with proper indexing
   - Use columnar format for analytical queries
   - Leverage caching where appropriate

### Compatibility Considerations

1. **Backward Compatibility**:
   - Metadata changes need migration plans
   - Wire protocol changes need versioning
   - SQL syntax changes need documentation

2. **Forward Compatibility**:
   - Consider upgrade scenarios
   - Test with older FE/BE versions
   - Document compatibility matrix

3. **External System Compatibility**:
   - Hive metastore versions
   - Hadoop distributions
   - Cloud provider APIs

### Security Considerations

1. **Authentication**:
   - Support for various auth mechanisms
   - Password policy enforcement
   - LDAP/Kerberos integration

2. **Authorization**:
   - Row-level security
   - Column-level access control
   - Apache Ranger integration

3. **Data Protection**:
   - Encryption at rest
   - Encryption in transit (TLS/SSL)
   - Secure credential management

### Deployment Considerations

1. **Configuration Management**:
   - FE config: `conf/fe.conf`
   - BE config: `conf/be.conf`
   - Runtime updates via HTTP API

2. **High Availability**:
   - Multiple FE nodes (leader + followers)
   - BE node replication
   - Automatic failover

3. **Monitoring**:
   - Metrics exposed via HTTP endpoints
   - Grafana dashboards in `/extra/`
   - Query audit logs

### Code Review Guidelines

When reviewing code, consider:

1. **Correctness**:
   - Does it solve the stated problem?
   - Are edge cases handled?
   - Are there potential race conditions?

2. **Performance**:
   - Is it efficient?
   - Are there unnecessary allocations?
   - Is I/O minimized?

3. **Maintainability**:
   - Is the code readable?
   - Are comments helpful?
   - Is it properly tested?

4. **Style**:
   - Does it follow style guidelines?
   - Is naming consistent?
   - Is it properly formatted?

---

## Resources

### Documentation

- **Main Documentation**: https://docs.starrocks.io/
- **GitHub Repository**: https://github.com/StarRocks/starrocks
- **Developer Guide**: `/docs/en/developers/`
- **Contributing Guide**: `/CONTRIBUTING.md`
- **PR Template**: `/.github/PULL_REQUEST_TEMPLATE.md`

### Configuration Files

- **Cursor Rules**: `.cursorrules` (AI assistant guidelines)
- **C++ Style**: `.clang-format`
- **Java Style**: `fe/checkstyle.xml`
- **Git Ignore**: `.gitignore`

### Build & Test Scripts

- **Main Build**: `build.sh`
- **Docker Build**: `build-in-docker.sh`
- **BE Unit Tests**: `run-be-ut.sh`
- **FE Unit Tests**: `run-fe-ut.sh`
- **Integration Tests**: `test/run.py`

### IDE Configuration

- **IntelliJ IDEA**: See [IDEA setup guide](https://docs.starrocks.io/docs/developers/development-environment/ide-setup/)
- **CLion**: See [CLion setup guide](https://github.com/StarRocks/community/blob/main/Contributors/guide/Clion.md)

### Community Resources

- **Slack**: https://try.starrocks.com/join-starrocks-on-slack
- **GitHub Issues**: https://github.com/StarRocks/starrocks/issues
- **Developer Mailing List**: https://groups.google.com/g/starrocks-dev
- **YouTube**: https://www.youtube.com/channel/UC38wR-ogamk4naaWNQ45y7Q

### Key Contact Points

- **Contributor License Agreement**: https://cla-assistant.io/StarRocks/starrocks
- **Code of Conduct**: `/CODE_OF_CONDUCT.md`
- **Security Policy**: `/SECURITY.md`

### External Dependencies

- **Third-party Libraries**: `/thirdparty/`
- **Build Dependencies**: Requires CMake, Java 11+, Maven/Gradle, GCC 10+
- **Runtime Dependencies**: See deployment documentation

---

## Quick Reference

### File Extensions & Languages

| Extension | Language | Usage |
|-----------|----------|-------|
| `.cpp`, `.h`, `.cc` | C++ | Backend implementation |
| `.java` | Java | Frontend & extensions |
| `.py` | Python | Testing & build scripts |
| `.proto` | Protocol Buffers | Data structures |
| `.thrift` | Thrift | RPC interfaces |
| `.g4` | ANTLR | SQL grammar |
| `.sql` | SQL | Test cases |
| `.sh` | Shell | Build & deployment |

### Component Summary

| Component | Path | Language | Purpose |
|-----------|------|----------|---------|
| Frontend | `/fe/` | Java | SQL processing, optimization, metadata |
| Backend | `/be/` | C++ | Query execution, storage |
| Extensions | `/java-extensions/` | Java | External connectors |
| Tests | `/test/` | Python | Integration testing |
| Generated | `/gensrc/` | Mixed | Auto-generated code |
| Config | `/conf/` | Config | Configuration templates |
| Docs | `/docs/` | Markdown | Documentation |

### Common Commands

```bash
# Build
./build.sh --fe                    # Build frontend only
./build.sh --be                    # Build backend only
BUILD_TYPE=Debug ./build.sh --be  # Debug build

# Test
./run-fe-ut.sh --test ClassName    # Run FE test
./run-be-ut.sh --test test_name    # Run BE test
cd test && python3 run.py          # Run SQL tests

# Development
source env.sh                      # Setup environment
git describe --tags                # Check version
clang-format -i file.cpp          # Format C++
cd fe && mvn checkstyle:check     # Check Java style
```

---

**Last Updated**: 2025-11-27
**Version**: Based on commit 6a1d9d21 (branch: main)

For questions or clarifications, please refer to the community resources or create an issue on GitHub.
