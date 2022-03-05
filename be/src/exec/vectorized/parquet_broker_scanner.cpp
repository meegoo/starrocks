// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "exec/vectorized/parquet_broker_scanner.h"

#include "gutil/strings/substitute.h"

namespace starrocks::vectorized {

ParquetBrokerScanner::ParquetBrokerScanner(RuntimeState* state, RuntimeProfile* profile, const TBrokerScanRange& scan_range,
                               ScannerCounter* counter)
        : FileScanner(state, profile, scan_range.params, counter),
          _scan_range(scan_range),
          _next_range(0),
          _scanner_eof(false) {
}

ParquetBrokerScanner::~ParquetBrokerScanner() {
    close();
}

Status ParquetBrokerScanner::open() {
    RETURN_IF_ERROR(FileScanner::open());
    if (_scan_range.ranges.empty()) {
        return Status::OK();
    }

    auto range = _scan_range.ranges[0];

    // column from path
    if (range.__isset.num_of_columns_from_file) {
        int nums = range.columns_from_path.size();
        for (const auto& rng : _scan_range.ranges) {
            if (nums != rng.columns_from_path.size()) {
                return Status::InternalError("Different range different columns.");
            }
        }
    }

    _build_file_read_param();

    RETURN_IF_ERROR(_next_scan_range());

    return Status::OK();
}

StatusOr<ChunkPtr> ParquetBrokerScanner::get_next() {
    auto chunk = std::make_shared<Chunk>();
    RETURN_IF_ERROR(_initialize_src_chunk(&chunk));

    while (!_scanner_eof) {
        Status status = _reader->get_next(&chunk);
        if (status.is_end_of_file()) {
            RETURN_IF_ERROR(_next_scan_range());
            continue;
        } else if (!status.ok()) {
            return status;
        }

        return std::move(chunk);
    }

    return Status::EndOfFile("");
}

void ParquetBrokerScanner::close() {
    _reader.reset();
}

Status ParquetBrokerScanner::_initialize_src_chunk(ChunkPtr* chunk) {
    _pool.clear();
    for (auto i = 0; i < _scan_range.params.src_slot_ids.size(); ++i) {
        auto* slot = _state->desc_tbl().get_slot_descriptor(_scan_range.params.src_slot_ids[i]);
        if (slot == nullptr) {
            continue;
        }
        ColumnPtr column;

        auto& type_desc = slot->type();
        auto pt = type_desc.type;
        if (pt == TYPE_ARRAY) {
            return Status::InternalError(strings::Substitute("ARRAY type($0) is not supported", slot->col_name()));
        }

        column = ColumnHelper::create_column(type_desc, slot->is_nullable());
        column->reserve(_state->chunk_size());
        (*chunk)->append_column(column, slot->id());
    }
    return Status::OK();
}

Status ParquetBrokerScanner::_next_scan_range() {
    while (true) {
        if (_next_range >= _scan_range.ranges.size()) {
            _scanner_eof = true;
            return Status::OK();
        }

        // 1. create file reader
        std::shared_ptr<RandomAccessFile> file;
        const TBrokerRangeDesc& range_desc = _scan_range.ranges[_next_range];
        RETURN_IF_ERROR(create_random_access_file(range_desc, _scan_range.broker_addresses[0], _scan_range.params,
                                              CompressionTypePB::NO_COMPRESSION, &file));

        // 2. create parquet reader
        _reader = std::make_shared<parquet::FileReader>(_state->chunk_size(), file.get(), _scan_range.ranges[_next_range].file_size);

        _update_file_reader_scan_range();

        RETURN_IF_ERROR(_reader->init(_file_read_param));

        _next_range++;

        return Status::OK();
    }
}

void ParquetBrokerScanner::_build_file_read_param() {
    HdfsFileReaderParam& param = _file_read_param;

    // build columns of materialized and partition.
    for (size_t i = 0; i < _scan_range.params.src_slot_ids.size(); i++) {
        auto* slot = _state->desc_tbl().get_slot_descriptor(_scan_range.params.src_slot_ids[i]);

        HdfsFileReaderParam::ColumnInfo column;
        column.slot_desc = slot;
        column.col_idx = i;
        column.col_type = slot->type();
        column.slot_id = slot->id();
        column.col_name = slot->col_name();

        param.materialized_columns.emplace_back(std::move(column));
    }

    param.tuple_desc = _state->desc_tbl().get_tuple_descriptor(_params.src_tuple_id);
    param.timezone = _state->timezone();
    param.stats = &_stats;
}

void ParquetBrokerScanner::_update_file_reader_scan_range() {
    HdfsFileReaderParam& param = _file_read_param;

    _hdfs_scan_range.relative_path = _scan_range.ranges[_next_range].path;
    _hdfs_scan_range.offset = _scan_range.ranges[_next_range].start_offset;
    _hdfs_scan_range.length = _scan_range.ranges[_next_range].size;
    _hdfs_scan_range.file_length = _scan_range.ranges[_next_range].file_size;
    _hdfs_scan_range.file_format = THdfsFileFormat::PARQUET;

    param.scan_ranges.clear();
    param.scan_ranges.emplace_back(&_hdfs_scan_range);
}

} // namespace starrocks::vectorized
