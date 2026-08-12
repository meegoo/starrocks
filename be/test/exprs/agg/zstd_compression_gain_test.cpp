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

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "base/string/slice.h"
#include "column/binary_column.h"
#include "column/nullable_column.h"
#include "exprs/agg/aggregate_factory.h"
#include "exprs/agg/aggregate_state_allocator.h"
#include "exprs/agg/base_aggregate_test.h"

namespace starrocks {

// The sample buffer goes through the aggregate state allocator, which the
// aggregate operator installs per thread. Stand one up for the test.
class ZstdCompressionGainTest : public ::testing::Test {
public:
    void SetUp() override {
        _allocator = std::make_unique<CountingAllocatorWithHook>();
        tls_agg_state_allocator = _allocator.get();
    }
    void TearDown() override {
        tls_agg_state_allocator = nullptr;
        _allocator.reset();
    }

private:
    std::unique_ptr<CountingAllocatorWithHook> _allocator;
};

namespace {

// Pull one integer field out of the JSON report. Keeps the test independent of
// the exact key order.
int64_t json_int(const std::string& report, const std::string& key) {
    size_t pos = report.find("\"" + key + "\":");
    EXPECT_NE(std::string::npos, pos) << key << " missing from " << report;
    pos += key.size() + 3;
    return std::stoll(report.substr(pos));
}

double json_double(const std::string& report, const std::string& key) {
    size_t pos = report.find("\"" + key + "\":");
    EXPECT_NE(std::string::npos, pos) << key << " missing from " << report;
    pos += key.size() + 3;
    return std::stod(report.substr(pos));
}

std::string run(const std::vector<std::string>& values) {
    const AggregateFunction* func = get_aggregate_function("zstd_compression_gain", TYPE_VARCHAR, TYPE_VARCHAR, false);
    EXPECT_NE(nullptr, func);

    auto column = BinaryColumn::create();
    for (const auto& v : values) {
        column->append(Slice(v));
    }

    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    auto state = ManagedAggrState::create(ctx.get(), func);
    const Column* raw = column.get();
    for (size_t i = 0; i < values.size(); ++i) {
        func->update(ctx.get(), &raw, state->state(), i);
    }

    auto result = BinaryColumn::create();
    func->finalize_to_column(ctx.get(), state->state(), result.get());
    EXPECT_EQ(1, result->size());
    return result->get_slice(0).to_string();
}

// One highly repetitive row, the shape the property targets.
std::string repetitive_row(int n) {
    std::string row;
    for (int i = 0; i <= n; ++i) {
        row += R"({"role":"assistant","content":"the quick brown fox jumps over the lazy dog again and again"},)";
    }
    return row;
}

} // namespace

// A column whose rows repeat each other heavily should come out clearly smaller
// with a dictionary than with either plain codec.
TEST_F(ZstdCompressionGainTest, redundant_column_reports_a_gain) {
    std::vector<std::string> values;
    for (int i = 0; i < 200; ++i) {
        values.emplace_back(repetitive_row(i % 40));
    }
    std::string report = run(values);

    EXPECT_EQ(200, json_int(report, "rows"));
    EXPECT_EQ(0, json_int(report, "null_rows"));
    EXPECT_GT(json_int(report, "sampled_bytes"), 0);
    // sampled_pages counts the pages the MEASURED region fills: the head of the
    // sample is reserved for the dictionary and left out of every arm, so that all
    // page sizes are compared over the same bytes.
    EXPECT_GT(json_int(report, "sampled_pages"), 0) << report;
    EXPECT_EQ(json_int(report, "sampled_pages"), json_int(report, "measured_pages")) << report;
    EXPECT_GT(json_int(report, "suggested_page_size"), 0) << report;
    EXPECT_NE(std::string::npos, report.find("\"page_size_options\":[")) << report;

    const int64_t lz4 = json_int(report, "lz4_bytes");
    const int64_t zstd = json_int(report, "zstd_bytes");
    const int64_t with_dict = json_int(report, "zstd_with_dict_bytes");
    EXPECT_GT(lz4, 0);
    EXPECT_GT(zstd, 0);
    EXPECT_GT(with_dict, 0);
    EXPECT_LT(with_dict, lz4) << report;
    EXPECT_LT(with_dict, zstd) << report;

    EXPECT_GT(json_double(report, "times_smaller_than_lz4"), 1.0) << report;
    EXPECT_NE(std::string::npos, report.find("\"suggestion\":")) << report;
}

// Incompressible data must not be sold as a win.
TEST_F(ZstdCompressionGainTest, random_column_reports_no_gain) {
    std::vector<std::string> values;
    uint64_t x = 88172645463325252ULL;
    for (int i = 0; i < 400; ++i) {
        std::string v;
        v.reserve(512);
        for (int j = 0; j < 512; ++j) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            v.push_back(static_cast<char>(x & 0xff));
        }
        values.emplace_back(std::move(v));
    }
    std::string report = run(values);

    EXPECT_LT(json_double(report, "times_smaller_than_lz4"), 1.5) << report;
    EXPECT_NE(std::string::npos, report.find("little to gain")) << report;
}

// An empty column, or one holding only empty strings, must report cleanly
// instead of dividing by zero.
TEST_F(ZstdCompressionGainTest, empty_input_reports_nothing_to_estimate) {
    std::string report = run({});
    EXPECT_EQ(0, json_int(report, "rows"));
    EXPECT_EQ(0, json_int(report, "sampled_bytes"));
    EXPECT_NE(std::string::npos, report.find("nothing to estimate")) << report;

    report = run({"", "", ""});
    EXPECT_EQ(3, json_int(report, "rows"));
    EXPECT_EQ(0, json_int(report, "sampled_bytes"));
    EXPECT_NE(std::string::npos, report.find("nothing to estimate")) << report;
}

// A sample with nothing left after the head reserved for the dictionary carries no
// usable signal.
TEST_F(ZstdCompressionGainTest, single_page_sample_refuses_to_estimate) {
    std::string report = run({repetitive_row(20)});
    EXPECT_EQ(1, json_int(report, "rows"));
    EXPECT_GT(json_int(report, "sampled_bytes"), 0);
    EXPECT_NE(std::string::npos, report.find("too small to estimate from")) << report;
}

// Two partial states must merge into the same answer a single state would give.
TEST_F(ZstdCompressionGainTest, merge_matches_a_single_state) {
    const AggregateFunction* func = get_aggregate_function("zstd_compression_gain", TYPE_VARCHAR, TYPE_VARCHAR, false);
    ASSERT_NE(nullptr, func);

    std::vector<std::string> values;
    for (int i = 0; i < 120; ++i) {
        values.emplace_back(repetitive_row(i % 20));
    }
    auto column = BinaryColumn::create();
    for (const auto& v : values) {
        column->append(Slice(v));
    }
    const Column* raw = column.get();

    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());

    auto left = ManagedAggrState::create(ctx.get(), func);
    auto right = ManagedAggrState::create(ctx.get(), func);
    for (size_t i = 0; i < values.size() / 2; ++i) {
        func->update(ctx.get(), &raw, left->state(), i);
    }
    for (size_t i = values.size() / 2; i < values.size(); ++i) {
        func->update(ctx.get(), &raw, right->state(), i);
    }

    // Serialize the right half and merge it into the left one.
    auto serialized = BinaryColumn::create();
    func->serialize_to_column(ctx.get(), right->state(), serialized.get());
    ASSERT_EQ(1, serialized->size());
    func->merge(ctx.get(), serialized.get(), left->state(), 0);

    auto merged = BinaryColumn::create();
    func->finalize_to_column(ctx.get(), left->state(), merged.get());

    auto whole = ManagedAggrState::create(ctx.get(), func);
    for (size_t i = 0; i < values.size(); ++i) {
        func->update(ctx.get(), &raw, whole->state(), i);
    }
    auto expected = BinaryColumn::create();
    func->finalize_to_column(ctx.get(), whole->state(), expected.get());

    EXPECT_EQ(expected->get_slice(0).to_string(), merged->get_slice(0).to_string());
}

// A truncated or otherwise unusable serialized state must be ignored, not
// treated as a length prefix to trust.
TEST_F(ZstdCompressionGainTest, corrupt_serialized_state_is_ignored) {
    const AggregateFunction* func = get_aggregate_function("zstd_compression_gain", TYPE_VARCHAR, TYPE_VARCHAR, false);
    ASSERT_NE(nullptr, func);

    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    auto state = ManagedAggrState::create(ctx.get(), func);

    auto bad = BinaryColumn::create();
    bad->append(Slice(std::string("short")));
    bad->append(Slice(std::string(41, '\0'))); // header says 0 sample bytes, one byte follows
    func->merge(ctx.get(), bad.get(), state->state(), 0);
    func->merge(ctx.get(), bad.get(), state->state(), 1);

    auto result = BinaryColumn::create();
    func->finalize_to_column(ctx.get(), state->state(), result.get());
    std::string report = result->get_slice(0).to_string();
    EXPECT_EQ(0, json_int(report, "rows"));
    EXPECT_NE(std::string::npos, report.find("nothing to estimate")) << report;
}

} // namespace starrocks
