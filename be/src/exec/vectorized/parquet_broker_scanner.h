// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#pragma once

#include "env/env.h"
#include "exec/parquet/file_reader.h"
#include "exec/vectorized/file_scanner.h"
#include "exec/vectorized/hdfs_scanner.h"
#include "exec/vectorized/orc_scanner_adapter.h"

namespace starrocks::vectorized {

class ParquetBrokerScanner : public FileScanner {
public:
    ParquetBrokerScanner(RuntimeState* state, RuntimeProfile* profile, const TBrokerScanRange& scan_range,
               starrocks::vectorized::ScannerCounter* counter);

    ~ParquetBrokerScanner();

    Status open() override;

    StatusOr<ChunkPtr> get_next() override;

    void close() override;

private:
    Status _next_scan_range();
    void _build_file_read_param();
    void _update_file_reader_scan_range();
    Status _initialize_src_chunk(ChunkPtr* chunk);

    const TBrokerScanRange& _scan_range;
    THdfsScanRange _hdfs_scan_range;
    size_t _next_range;
    bool _scanner_eof;
    HdfsScanStats _stats;
    HdfsFileReaderParam _file_read_param;
    ObjectPool _pool;

    std::shared_ptr<parquet::FileReader> _reader = nullptr;
};

} // namespace starrocks::vectorized
