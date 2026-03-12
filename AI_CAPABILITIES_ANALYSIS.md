# StarRocks AI 时代能力分析报告

> 分析日期：2026-03-12
> 基于 StarRocks 代码库的深入分析

## 一、AI 时代对分析型数据库的新要求

| 能力维度 | 说明 | 重要程度 |
|---------|------|---------|
| **向量存储与检索** | 高维向量的存储、ANN 索引和相似度搜索，服务于 RAG、语义搜索 | ★★★★★ |
| **AI 函数集成** | SQL 中直接调用 LLM/Embedding 模型 | ★★★★★ |
| **Python 生态集成** | AI 从业者以 Python 为主，数据库需深度融入 Python 工作流 | ★★★★★ |
| **半结构化数据处理** | 高效处理 JSON、嵌套类型 | ★★★★ |
| **实时数据管道** | AI 应用需要实时特征计算和推理结果的写入查询 | ★★★★ |
| **湖仓一体** | 统一管理训练数据（数据湖）和服务数据（数据仓库） | ★★★★ |
| **GPU 加速** | 大规模向量计算、复杂分析查询的硬件加速 | ★★★ |
| **工作负载隔离** | AI 推理查询（低延迟）与训练数据准备（高吞吐）的资源隔离 | ★★★ |

## 二、StarRocks 当前能力盘点

### 已具备的能力

| 能力 | 现状 | 代码位置 |
|------|------|---------|
| 向量索引 | Beta，支持 HNSW 和 IVFPQ | `be/src/storage/index/vector/` |
| 向量搜索函数 | `approx_l2_distance()`, `approx_cosine_similarity()` | `be/src/exprs/` |
| AI Query 函数 | 实验性，支持 OpenAI 兼容接口 | `be/src/exprs/ai_functions.cpp` |
| LLM 查询服务 | 实验性，含 LRU 缓存 | `be/src/util/llm_query_service.cpp` |
| 数据湖联邦 | 成熟，Iceberg/Hudi/Delta Lake/Paimon | `fe/fe-core/.../connector/` |
| 半结构化数据 | JSON/ARRAY/MAP/STRUCT 完整支持 | 多处 |
| 实时摄入 | Stream Load/Routine Load/Flink Connector | 多处 |
| 物化视图 | 异步 MV，自动改写、增量刷新 | `fe/fe-core/.../mv/` |
| 资源隔离 | 资源组，CPU/内存/并发限制 | `fe/fe-core/.../resource/` |
| Python UDF | 标量函数，支持 PyArrow 向量化（v3.4+ 实验性） | — |

### 明显缺失或薄弱的能力

| 能力 | 现状 | 影响 |
|------|------|------|
| Embedding 生成 | 无内置能力 | 无法在 SQL 中生成向量 |
| 向量索引成熟度 | Beta、单表仅一个向量索引、仅 shared-nothing | 生产可用性受限 |
| Python UDF 完整性 | 仅标量函数 | 无法做聚合/窗口级 Python 分析 |
| GPU 加速 | 完全没有 | 大规模向量计算性能受限 |
| ML Pipeline 集成 | 无 MLflow/Ray/Feature Store | AI 工作流断裂 |
| RAG 框架支持 | 无原生支持 | 无法一站式 RAG |

## 三、优先级排序：最需要尽快提升的能力

### P0：向量搜索从 Beta 走向生产级

**理由：** 向量搜索是 AI 应用最直接的数据库需求（RAG、语义搜索、推荐系统）。

需要补齐：
- 多向量索引支持（当前单表仅一个）
- Shared-data 模式支持
- 混合查询优化（向量 + 标量联合查询的 pre/post filter）
- 索引算法扩展（DiskANN、ScaNN）
- 高维向量支持（4096+，适配最新 LLM embedding）

### P1：SQL 原生 AI 函数生态

**理由：** 让分析师在 SQL 中直接用 AI 能力是最大的差异化价值。

需要补齐：
- `ai_embedding()` 函数，支持多模型后端
- 内置语义分析函数（分类、情感分析等）
- 多 LLM 后端支持（Claude、Ollama、vLLM 等）
- 批量推理优化（当前逐行调用）
- AI 函数结果与物化视图的集成

### P2：Python 生态深度集成

**理由：** AI 开发者的母语是 Python。

需要补齐：
- Python UDF 扩展到聚合/窗口函数
- 原生 DataFrame API（类似 Snowpark）
- Arrow Flight SQL 高性能数据传输
- Jupyter 深度集成

### P3：AI 工作流平台集成

需要补齐：
- Feature Store 集成（Feast 等）
- MLflow / Ray 集成
- LangChain / LlamaIndex 官方 vector store 后端

### P4：GPU 加速（中长期）

- 向量距离计算 GPU 加速
- 大规模 JOIN/聚合 GPU 加速
- RAPIDS 生态集成

## 四、竞品对标

| 能力 | StarRocks | Databricks | Snowflake | ClickHouse |
|------|-----------|------------|-----------|------------|
| 向量搜索 | Beta | GA | GA | GA |
| AI 函数 | 实验性 | `ai_query()` GA | Cortex AI GA | `embedText()` |
| Python 集成 | 标量 UDF | Spark + Python | Snowpark | — |
| ML 平台集成 | 无 | MLflow 原生 | — | — |
| GPU | 无 | 有 | 有 | — |

## 五、核心结论

StarRocks 的独特优势在于 **实时性 + 高并发**，应围绕此构建 AI 能力——即 **实时 AI 分析（Real-time AI Analytics）**，而非竞争离线训练场景。

**最优先的两件事：**
1. 向量搜索生产化（让 StarRocks 成为实时 RAG 的可选基础设施）
2. SQL 原生 AI 函数生态（让 SQL 用户无需离开 StarRocks 就能使用 AI）
