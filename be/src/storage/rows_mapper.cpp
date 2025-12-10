// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "storage/rows_mapper.h"

#include <fmt/format.h>

#include "fs/fs.h"
#include "storage/data_dir.h"
#include "storage/storage_engine.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/raw_container.h"

namespace starrocks {

Status RowsMapperBuilder::_init() {
    ASSIGN_OR_RETURN(auto fs, FileSystem::CreateSharedFromString(_filename));
    WritableFileOptions wblock_opts{.sync_on_close = true, .mode = FileSystem::CREATE_OR_OPEN_WITH_TRUNCATE};
    ASSIGN_OR_RETURN(_wfile, fs->new_writable_file(wblock_opts, _filename));
    return Status::OK();
}

Status RowsMapperBuilder::append(const std::vector<uint64_t>& rssid_rowids) {
    if (rssid_rowids.empty()) {
        // skip create rows mapper file when output rowset is empty.
        return Status::OK();
    }
    if (_wfile == nullptr) {
        RETURN_IF_ERROR(_init());
    }
    RETURN_IF_ERROR(_wfile->append(Slice((const char*)rssid_rowids.data(), rssid_rowids.size() * 8)));
    _checksum = crc32c::Extend(_checksum, (const char*)rssid_rowids.data(), rssid_rowids.size() * 8);
    _row_count += rssid_rowids.size();
    return Status::OK();
}

Status RowsMapperBuilder::finalize() {
    if (_wfile == nullptr) {
        // Empty rows, skip finalize
        return Status::OK();
    }
    std::string row_count_str;
    // row count
    put_fixed64_le(&row_count_str, _row_count);
    _checksum = crc32c::Extend(_checksum, row_count_str.data(), row_count_str.size());
    // checksum
    std::string checksum_str;
    put_fixed32_le(&checksum_str, _checksum);
    RETURN_IF_ERROR(_wfile->append(row_count_str));
    RETURN_IF_ERROR(_wfile->append(checksum_str));
    return _wfile->close();
}

RowsMapperIterator::~RowsMapperIterator() {
    if (_rfile != nullptr) {
        const std::string filename = _rfile->filename();
        _rfile.reset(nullptr);
        auto st = fs::delete_file(filename);
        if (!st.ok()) {
            LOG(ERROR) << "delete rows mapper file fail, st: " << st;
        }
    }
}

// Open file
Status RowsMapperIterator::open(const std::string& filename) {
    ASSIGN_OR_RETURN(auto fs, FileSystem::CreateSharedFromString(filename));
    ASSIGN_OR_RETURN(_rfile, fs->new_random_access_file(filename));
    ASSIGN_OR_RETURN(int64_t file_size, _rfile->get_size());
    // 1. read checksum
    std::string checksum_str;
    raw::stl_string_resize_uninitialized(&checksum_str, 4);
    RETURN_IF_ERROR(_rfile->read_at_fully(file_size - 4, checksum_str.data(), checksum_str.size()));
    _expected_checksum = decode_fixed32_le((const uint8_t*)checksum_str.data());
    // 2. read row count
    std::string row_count_str;
    raw::stl_string_resize_uninitialized(&row_count_str, 8);
    RETURN_IF_ERROR(_rfile->read_at_fully(file_size - 12, row_count_str.data(), row_count_str.size()));
    _row_count = decode_fixed64_le((const uint8_t*)row_count_str.data());
    // 3. check file size (should be 4 bytes(checksum) + 8 bytes(row count) + id list)
    if (file_size != 12 + _row_count * EACH_ROW_SIZE) {
        return Status::Corruption(
                fmt::format("RowsMapper file corruption. file size: {}, row count: {}", file_size, _row_count));
    }
    return Status::OK();
}

Status RowsMapperIterator::next_values(size_t fetch_cnt, std::vector<uint64_t>* rssid_rowids) {
    if (fetch_cnt == 0) {
        // No need to fetch
        return Status::OK();
    }
    if (_pos + fetch_cnt > _row_count) {
        return Status::EndOfFile(fmt::format("RowsMapperIterator end of file. position/fetch cnt/row count: {}/{}/{}",
                                             _pos, fetch_cnt, _row_count));
    }
    rssid_rowids->resize(fetch_cnt);
    RETURN_IF_ERROR(_rfile->read_at_fully(_pos * EACH_ROW_SIZE, rssid_rowids->data(), fetch_cnt * EACH_ROW_SIZE));
    _current_checksum = crc32c::Extend(_current_checksum, (const char*)rssid_rowids->data(), rssid_rowids->size() * 8);
    _pos += fetch_cnt;
    return Status::OK();
}

Status RowsMapperIterator::status() {
    if (_pos != _row_count) {
        return Status::Corruption(fmt::format("Chunk vs rows mapper's row count mismatch. {} vs {} filename: {}", _pos,
                                              _row_count, _rfile->filename()));
    }
    std::string row_count_str;
    // row count
    put_fixed64_le(&row_count_str, _row_count);
    _current_checksum = crc32c::Extend(_current_checksum, row_count_str.data(), row_count_str.size());
    if (_expected_checksum != _current_checksum) {
        return Status::Corruption(fmt::format("checksum mismatch. cur: {} expected: {} filename: {}", _current_checksum,
                                              _expected_checksum, _rfile->filename()));
    }
    return Status::OK();
}

StatusOr<std::string> lake_rows_mapper_filename(int64_t tablet_id, int64_t txn_id) {
    auto data_dir = StorageEngine::instance()->get_persistent_index_store(tablet_id);
    if (data_dir == nullptr) {
        return Status::NotFound(fmt::format("Not local disk found. tablet id: {}", tablet_id));
    }
    return data_dir->get_tmp_path() + "/" + fmt::format("{:016X}_{:016X}.crm", tablet_id, txn_id);
}

StatusOr<std::string> lake_rows_mapper_filename(int64_t tablet_id, int64_t txn_id, int32_t subtask_id) {
    auto data_dir = StorageEngine::instance()->get_persistent_index_store(tablet_id);
    if (data_dir == nullptr) {
        return Status::NotFound(fmt::format("Not local disk found. tablet id: {}", tablet_id));
    }
    return data_dir->get_tmp_path() + "/" + fmt::format("{:016X}_{:016X}_{}.crm", tablet_id, txn_id, subtask_id);
}

StatusOr<uint64_t> lake_rows_mapper_row_count(int64_t tablet_id, int64_t txn_id) {
    ASSIGN_OR_RETURN(auto filename, lake_rows_mapper_filename(tablet_id, txn_id));
    ASSIGN_OR_RETURN(auto fs, FileSystem::CreateSharedFromString(filename));
    ASSIGN_OR_RETURN(auto rfile, fs->new_random_access_file(filename));
    ASSIGN_OR_RETURN(int64_t file_size, rfile->get_size());
    if (file_size < 12) {
        return Status::Corruption(fmt::format("RowsMapper file too small. file size: {}", file_size));
    }
    // Read row count from file (8 bytes before the last 4 bytes checksum)
    std::string row_count_str;
    raw::stl_string_resize_uninitialized(&row_count_str, 8);
    RETURN_IF_ERROR(rfile->read_at_fully(file_size - 12, row_count_str.data(), row_count_str.size()));
    return decode_fixed64_le((const uint8_t*)row_count_str.data());
}

StatusOr<uint64_t> merge_lake_rows_mapper_files(int64_t tablet_id, int64_t txn_id, int32_t subtask_count) {
    if (subtask_count <= 0) {
        return Status::InvalidArgument("subtask_count must be positive");
    }

    // Get the merged output filename
    ASSIGN_OR_RETURN(auto merged_filename, lake_rows_mapper_filename(tablet_id, txn_id));

    // Create the merged file writer
    ASSIGN_OR_RETURN(auto fs, FileSystem::CreateSharedFromString(merged_filename));
    WritableFileOptions wblock_opts{.sync_on_close = true, .mode = FileSystem::CREATE_OR_OPEN_WITH_TRUNCATE};
    ASSIGN_OR_RETURN(auto wfile, fs->new_writable_file(wblock_opts, merged_filename));

    uint32_t merged_checksum = 0;
    uint64_t merged_row_count = 0;
    constexpr size_t kBufferSize = 4096 * 8; // 4096 rows per buffer

    // Merge all subtask files in order
    for (int32_t subtask_id = 0; subtask_id < subtask_count; subtask_id++) {
        ASSIGN_OR_RETURN(auto subtask_filename, lake_rows_mapper_filename(tablet_id, txn_id, subtask_id));

        if (!fs::path_exist(subtask_filename)) {
            // Subtask file doesn't exist, skip (subtask may have produced empty output)
            continue;
        }

        ASSIGN_OR_RETURN(auto rfile, fs->new_random_access_file(subtask_filename));
        ASSIGN_OR_RETURN(int64_t file_size, rfile->get_size());

        if (file_size < 12) {
            // Invalid file, skip
            continue;
        }

        // Read row count from subtask file
        std::string row_count_str;
        raw::stl_string_resize_uninitialized(&row_count_str, 8);
        RETURN_IF_ERROR(rfile->read_at_fully(file_size - 12, row_count_str.data(), row_count_str.size()));
        uint64_t subtask_row_count = decode_fixed64_le((const uint8_t*)row_count_str.data());

        // Copy data from subtask file (excluding row count and checksum at the end)
        int64_t data_size = file_size - 12;
        int64_t offset = 0;
        std::vector<uint8_t> buffer(kBufferSize);

        while (offset < data_size) {
            size_t to_read = std::min<size_t>(kBufferSize, data_size - offset);
            RETURN_IF_ERROR(rfile->read_at_fully(offset, buffer.data(), to_read));
            RETURN_IF_ERROR(wfile->append(Slice(buffer.data(), to_read)));
            merged_checksum = crc32c::Extend(merged_checksum, (const char*)buffer.data(), to_read);
            offset += to_read;
        }

        merged_row_count += subtask_row_count;
    }

    // Write merged row count
    std::string row_count_str;
    put_fixed64_le(&row_count_str, merged_row_count);
    merged_checksum = crc32c::Extend(merged_checksum, row_count_str.data(), row_count_str.size());

    // Write checksum
    std::string checksum_str;
    put_fixed32_le(&checksum_str, merged_checksum);

    RETURN_IF_ERROR(wfile->append(row_count_str));
    RETURN_IF_ERROR(wfile->append(checksum_str));
    RETURN_IF_ERROR(wfile->close());

    // Delete subtask files after successful merge
    for (int32_t subtask_id = 0; subtask_id < subtask_count; subtask_id++) {
        auto subtask_filename_st = lake_rows_mapper_filename(tablet_id, txn_id, subtask_id);
        if (subtask_filename_st.ok() && fs::path_exist(subtask_filename_st.value())) {
            auto st = fs::delete_file(subtask_filename_st.value());
            if (!st.ok()) {
                LOG(WARNING) << "Failed to delete subtask rows mapper file: " << subtask_filename_st.value()
                             << ", error: " << st;
            }
        }
    }

    return merged_row_count;
}

std::string local_rows_mapper_filename(Tablet* tablet, const std::string& rowset_id) {
    return tablet->data_dir()->get_tmp_path() + "/" + fmt::format("{:016X}_{}.crm", tablet->tablet_id(), rowset_id);
}

} // namespace starrocks