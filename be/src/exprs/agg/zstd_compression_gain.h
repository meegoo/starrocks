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

#pragma once

#include <fmt/format.h>

#include <cstring>
#include <string>
#include <vector>

#include "base/compression/block_compression.h"
#include "base/compression/zstd_dict.h"
#include "base/string/slice.h"
#include "column/binary_column.h"
#include "column/json_column.h"
#include "column/nullable_column.h"
#include "column/vectorized_fwd.h"
#include "common/config_rowset_fwd.h"
#include "exprs/agg/aggregate.h"
#include "exprs/agg/aggregate_state_allocator.h"
#include "gen_cpp/segment.pb.h"
#include "gutil/casts.h"
#include "types/json_value.h"

namespace starrocks {

// zstd_compression_gain(col) answers one question: if this column were listed in the
// table property zstd_compression_columns, how much smaller would it get?
//
// It answers it by doing what the segment writer does. Values are concatenated
// into a bounded sample, the sample is cut into pages of the configured data
// page size, and the dictionary is taken from the first page -- exactly how the
// writer builds one in "sample" mode. Every page AFTER that first one is then
// compressed three ways: LZ4, ZSTD, and ZSTD against the dictionary. Comparing
// the three totals is the estimate.
//
// The page the dictionary came from is left out of all three totals on purpose.
// It would compress against itself and collapse to almost nothing, which is a
// real but one-off effect: in a segment of hundreds of pages it is noise, but in
// a sample of three pages it would be most of the reported gain.
//
// The sample is capped, so the function reads the whole column but only measures
// the first kMaxSampleBytes of it. That mirrors reality: the writer's dictionary
// is also built from one segment's worth of data, not from the whole table.
struct ZstdCompressionGainState {
    // Concatenated raw bytes of the values that fit within the cap. It goes
    // through the aggregate state allocator so the sample counts against the
    // query's memory, like every other aggregate state does.
    VectorWithAggStateAllocator<char> sample;
    // Every non-null value counts here, including those that did not fit.
    int64_t total_bytes = 0;
    int64_t rows = 0;
    int64_t null_rows = 0;
    int64_t sampled_rows = 0;
};

template <LogicalType LT>
class ZstdCompressionGainAggregateFunction final
        : public AggregateFunctionBatchHelper<ZstdCompressionGainState, ZstdCompressionGainAggregateFunction<LT>> {
public:
    // How much of the column is actually measured. Large enough to hold many
    // pages (so the per-page numbers are stable) and small enough that the
    // aggregate state stays bounded regardless of the column's real size.
    static constexpr size_t kMaxSampleBytes = 8 * 1024 * 1024;

    void update(FunctionContext* ctx, const Column** columns, AggDataPtr __restrict state,
                size_t row_num) const override {
        auto& s = this->data(state);
        s.rows++;
        Slice v = value_at(columns[0], row_num);
        if (v.data == nullptr) {
            s.null_rows++;
            return;
        }
        s.total_bytes += static_cast<int64_t>(v.size);
        if (append_to_sample(s, v)) {
            s.sampled_rows++;
        }
    }

    void merge(FunctionContext* ctx, const Column* column, AggDataPtr __restrict state, size_t row_num) const override {
        auto& s = this->data(state);
        Slice packed = down_cast<const BinaryColumn*>(column)->get_slice(row_num);
        ZstdCompressionGainState other;
        if (!unpack(packed, &other)) {
            return;
        }
        s.rows += other.rows;
        s.null_rows += other.null_rows;
        s.total_bytes += other.total_bytes;
        // The sample is appended whole or not at all, so the rows behind it are
        // either all counted or none of them are.
        if (append_to_sample(s, Slice(other.sample.data(), other.sample.size()))) {
            s.sampled_rows += other.sampled_rows;
        }
    }

    void serialize_to_column(FunctionContext* ctx, ConstAggDataPtr __restrict state, Column* to) const override {
        std::string packed = pack(this->data(state));
        down_cast<BinaryColumn*>(to)->append(Slice(packed));
    }

    void convert_to_serialize_format(FunctionContext* ctx, const Columns& src, size_t chunk_size,
                                     MutableColumnPtr& dst) const override {
        auto* binary = down_cast<BinaryColumn*>(dst.get());
        for (size_t i = 0; i < chunk_size; ++i) {
            ZstdCompressionGainState s;
            s.rows = 1;
            Slice v = value_at(src[0].get(), i);
            if (v.data == nullptr) {
                s.null_rows = 1;
            } else {
                s.total_bytes = static_cast<int64_t>(v.size);
                if (append_to_sample(s, v)) {
                    s.sampled_rows = 1;
                }
            }
            std::string packed = pack(s);
            binary->append(Slice(packed));
        }
    }

    void finalize_to_column(FunctionContext* ctx, ConstAggDataPtr __restrict state, Column* to) const override {
        std::string report = build_report(this->data(state));
        down_cast<BinaryColumn*>(to)->append(Slice(report));
    }

    std::string get_name() const override { return "zstd_compression_gain"; }

private:
    // Raw bytes of a value, in the form the storage layer would compress. A null
    // value is reported as a slice with a null data pointer.
    //
    // This function is registered for nullable and non-nullable inputs alike, so
    // the column handed to it may still be wrapped.
    static Slice value_at(const Column* column, size_t row_num) {
        if (column->is_nullable()) {
            const auto* nullable = down_cast<const NullableColumn*>(column);
            if (nullable->is_null(row_num)) {
                return {static_cast<const char*>(nullptr), 0};
            }
            column = nullable->data_column().get();
        }
        if constexpr (LT == TYPE_JSON) {
            const auto* json_column = down_cast<const JsonColumn*>(column);
            const JsonValue* v = json_column->get_object(row_num);
            if (v == nullptr) {
                return {static_cast<const char*>(nullptr), 0};
            }
            return v->get_slice();
        } else {
            return down_cast<const BinaryColumn*>(column)->get_slice(row_num);
        }
    }

    // Returns whether the bytes were taken. A value is taken whole or not at
    // all, so the sample never ends with half a value -- the writer never splits
    // a value across pages either.
    static bool append_to_sample(ZstdCompressionGainState& s, const Slice& v) {
        if (v.size == 0 || v.size > kMaxSampleBytes - s.sample.size()) {
            return false;
        }
        s.sample.insert(s.sample.end(), v.data, v.data + v.size);
        return true;
    }

    // [rows][null_rows][total_bytes][sampled_rows][sample_len][sample bytes]
    static std::string pack(const ZstdCompressionGainState& s) {
        std::string out;
        out.reserve(5 * sizeof(int64_t) + s.sample.size());
        auto put = [&out](int64_t v) { out.append(reinterpret_cast<const char*>(&v), sizeof(v)); };
        put(s.rows);
        put(s.null_rows);
        put(s.total_bytes);
        put(s.sampled_rows);
        put(static_cast<int64_t>(s.sample.size()));
        out.append(s.sample.data(), s.sample.size());
        return out;
    }

    static bool unpack(const Slice& packed, ZstdCompressionGainState* s) {
        constexpr size_t kHeader = 5 * sizeof(int64_t);
        if (packed.size < kHeader) {
            return false;
        }
        const char* p = packed.data;
        auto get = [&p]() {
            int64_t v;
            std::memcpy(&v, p, sizeof(v));
            p += sizeof(v);
            return v;
        };
        s->rows = get();
        s->null_rows = get();
        s->total_bytes = get();
        s->sampled_rows = get();
        int64_t sample_len = get();
        if (sample_len < 0 || static_cast<size_t>(sample_len) != packed.size - kHeader) {
            return false;
        }
        s->sample.assign(p, p + sample_len);
        return true;
    }

    // Sum of the per-page compressed sizes, the way the segment writer would
    // produce them, over every page except the first -- that one is the
    // dictionary. `cdict` is optional; when set the pages are compressed
    // against it.
    static bool compress_pages(const BlockCompressionCodec* codec, const Slice& sample, size_t page_bytes,
                               const compression::ZstdCDict* cdict, int64_t* compressed_bytes, int64_t* pages) {
        std::string buf;
        int64_t total = 0;
        int64_t page_count = 0;
        for (size_t off = page_bytes; off < sample.size; off += page_bytes) {
            size_t len = std::min(page_bytes, sample.size - off);
            Slice page(sample.data + off, len);
            buf.resize(codec->max_compressed_len(len));
            Slice out(buf.data(), buf.size());
            Status st = cdict == nullptr
                                ? codec->compress(std::vector<Slice>{page}, &out)
                                : codec->compress(std::vector<Slice>{page}, &out, false, len, nullptr, nullptr, cdict);
            if (!st.ok()) {
                return false;
            }
            total += static_cast<int64_t>(out.size);
            page_count++;
        }
        *compressed_bytes = total;
        *pages = page_count;
        return true;
    }

    static std::string build_report(const ZstdCompressionGainState& s) {
        const size_t page_bytes = static_cast<size_t>(std::max(1, config::data_page_size));
        const Slice sample(s.sample.data(), s.sample.size());
        const int64_t sampled_bytes = static_cast<int64_t>(sample.size);

        auto header = [&](const char* note) {
            return fmt::format(
                    R"({{"rows":{},"null_rows":{},"total_bytes":{},"sampled_bytes":{},"page_bytes":{},"note":"{}"}})",
                    s.rows, s.null_rows, s.total_bytes, sampled_bytes, page_bytes, note);
        };

        if (sampled_bytes == 0) {
            return header("no non-empty value was sampled, nothing to estimate");
        }

        const BlockCompressionCodec* lz4 = nullptr;
        const BlockCompressionCodec* zstd = nullptr;
        if (!get_block_compression_codec(CompressionTypePB::LZ4, &lz4).ok() ||
            !get_block_compression_codec(CompressionTypePB::ZSTD, &zstd).ok()) {
            return header("compression codecs are unavailable on this BE");
        }

        const int64_t sampled_pages = static_cast<int64_t>((sample.size + page_bytes - 1) / page_bytes);
        if (sampled_pages < 2) {
            return header("the sample filled a single page, too little data to estimate from");
        }

        int64_t lz4_bytes = 0;
        int64_t zstd_bytes = 0;
        int64_t pages = 0;
        if (!compress_pages(lz4, sample, page_bytes, nullptr, &lz4_bytes, &pages) ||
            !compress_pages(zstd, sample, page_bytes, nullptr, &zstd_bytes, &pages)) {
            return header("compressing the sample failed");
        }

        // The dictionary the writer would build: the first page, verbatim.
        Slice dict_src(sample.data, std::min(page_bytes, sample.size));
        auto cdict = compression::ZstdCDict::create(dict_src, -1);
        if (!cdict.ok()) {
            return header("building a dictionary from the sample failed");
        }
        int64_t zstd_dict_body_bytes = 0;
        if (!compress_pages(zstd, sample, page_bytes, cdict.value().get(), &zstd_dict_body_bytes, &pages)) {
            return header("compressing the sample against a dictionary failed");
        }
        // The dictionary is stored once per column per segment. A real segment
        // holds far more pages than this sample does, so folding the dictionary
        // into the total here would charge it against a handful of pages and
        // understate the gain. It is reported separately instead, as the fixed
        // cost it is.
        const int64_t dict_bytes = static_cast<int64_t>(dict_src.size);
        const int64_t zstd_dict_bytes = zstd_dict_body_bytes;

        auto ratio = [](int64_t baseline, int64_t candidate) {
            return candidate <= 0 ? 0.0 : static_cast<double>(baseline) / static_cast<double>(candidate);
        };
        const double vs_lz4 = ratio(lz4_bytes, zstd_dict_bytes);
        const double vs_zstd = ratio(zstd_bytes, zstd_dict_bytes);

        // The property is worth setting when it beats what the column costs
        // today by enough to pay for switching codec. Below 1.2x the difference
        // is within the noise of how the data happens to be laid out.
        const char* suggestion = vs_lz4 >= 1.5   ? "enable zstd_compression_columns on this column"
                                 : vs_lz4 >= 1.2 ? "a modest gain, worth enabling only if storage is the concern"
                                                 : "little to gain, leave the column as it is";

        return fmt::format(
                R"({{"rows":{},"null_rows":{},"total_bytes":{},"avg_row_bytes":{},)"
                R"("sampled_rows":{},"sampled_bytes":{},"page_bytes":{},"sampled_pages":{},"measured_pages":{},)"
                R"("lz4_bytes":{},"zstd_bytes":{},"zstd_with_dict_bytes":{},"dict_bytes":{},)"
                R"("times_smaller_than_lz4":{:.2f},"times_smaller_than_zstd":{:.2f},"suggestion":"{}"}})",
                s.rows, s.null_rows, s.total_bytes,
                s.rows - s.null_rows > 0 ? s.total_bytes / (s.rows - s.null_rows) : 0, s.sampled_rows, sampled_bytes,
                page_bytes, sampled_pages, pages, lz4_bytes, zstd_bytes, zstd_dict_bytes, dict_bytes, vs_lz4, vs_zstd,
                suggestion);
    }
};

} // namespace starrocks
