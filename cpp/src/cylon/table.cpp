/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <glog/logging.h>
#include <arrow/compute/api.h>
#include <arrow/table.h>

#include <fstream>
#include <future>
#include <memory>
#include <unordered_map>
#include <iostream>

#include <cylon/table.hpp>
#include <cylon/join/join_utils.hpp>
#include <cylon/arrow/arrow_all_to_all.hpp>
#include <cylon/arrow/arrow_comparator.hpp>
#include <cylon/arrow/arrow_types.hpp>
#include <cylon/ctx/arrow_memory_pool_utils.hpp>
#include <cylon/io/arrow_io.hpp>
#include <cylon/join/join.hpp>
#include <cylon/partition/partition.hpp>
#include <cylon/table_api_extended.hpp>
#include <cylon/thridparty/flat_hash_map/bytell_hash_map.hpp>
#include <cylon/util/arrow_utils.hpp>
#include <cylon/util/macros.hpp>
#include <cylon/util/to_string.hpp>

namespace cylon {

/**
 * creates an Arrow array based on col_idx, filtered by row_indices
 * @param ctx
 * @param table
 * @param col_idx
 * @param row_indices
 * @param array_vector
 * @return
 */
Status PrepareArray(std::shared_ptr<cylon::CylonContext> &ctx,
					const std::shared_ptr<arrow::Table> &table, const int32_t col_idx,
					const std::vector<int64_t> &row_indices, arrow::ArrayVector &array_vector) {
  std::shared_ptr<arrow::Array> destination_col_array;
  arrow::Status ar_status =
	  cylon::util::copy_array_by_indices(row_indices, cylon::util::GetChunkOrEmptyArray(table->column(col_idx), 0),
										 &destination_col_array, cylon::ToArrowPool(ctx));
  if (ar_status != arrow::Status::OK()) {
	LOG(FATAL) << "Failed while copying a column to the final table from tables."
			   << ar_status.ToString();
	return Status(static_cast<int>(ar_status.code()), ar_status.message());
  }
  array_vector.push_back(destination_col_array);
  return Status::OK();
}

static inline Status all_to_all_arrow_tables(const std::shared_ptr<CylonContext> &ctx,
                                             const std::shared_ptr<arrow::Schema> &schema,
                                             const std::vector<std::shared_ptr<arrow::Table>> &partitioned_tables,
                                             std::shared_ptr<arrow::Table> &table_out) {
  const auto &neighbours = ctx->GetNeighbours(true);
  std::vector<std::shared_ptr<arrow::Table>> received_tables;
  received_tables.reserve(neighbours.size());

  // define call back to catch the receiving tables
  ArrowCallback arrow_callback =
      [&received_tables](int source, const std::shared_ptr<arrow::Table> &table_, int reference) {
        received_tables.push_back(table_);
        return true;
      };

  // doing all to all communication to exchange tables
  cylon::ArrowAllToAll all_to_all(ctx, neighbours, neighbours, ctx->GetNextSequence(),
								  arrow_callback, schema);

  // if world size == partitions, simply send paritions based on index
  const size_t world_size = (size_t)ctx->GetWorldSize(), num_partitions = partitioned_tables.size(),
	  rank = ctx->GetRank();
  if (world_size == num_partitions) {
	for (size_t i = 0; i < partitioned_tables.size(); i++) {
	  if (i != rank) {
		all_to_all.insert(partitioned_tables[i], i);
	  } else {
		received_tables.push_back(partitioned_tables[i]);
	  }
	}
  } else {  // divide parititions to world_size potions and send accordingly
	for (size_t i = 0; i < partitioned_tables.size(); i++) {
	  size_t target = i * world_size / num_partitions;
	  if (target != rank) {
		all_to_all.insert(partitioned_tables[i], target);
	  } else {
		received_tables.push_back(partitioned_tables[i]);
	  }
	}
  }

  // now complete the communication
  all_to_all.finish();
  while (!all_to_all.isComplete()) {
  }
  all_to_all.close();

  /*  // now clear locally partitioned tables
  partitioned_tables.clear();*/

  // now we have the final set of tables
  LOG(INFO) << "Concatenating tables, Num of tables :  " << received_tables.size();
  arrow::Result<std::shared_ptr<arrow::Table>> concat_res =
	  arrow::ConcatenateTables(received_tables);
  RETURN_CYLON_STATUS_IF_ARROW_FAILED(concat_res.status());
  const auto &final_table = concat_res.ValueOrDie();
  LOG(INFO) << "Done concatenating tables, rows :  " << final_table->num_rows();

  arrow::Result<std::shared_ptr<arrow::Table>> combine_res =
	  final_table->CombineChunks(cylon::ToArrowPool(ctx));
  RETURN_CYLON_STATUS_IF_ARROW_FAILED(concat_res.status());
  table_out = combine_res.ValueOrDie();

  return Status::OK();
}

template<typename T>
// T is int32_t or const std::vector<int32_t>&
static inline Status shuffle_table_by_hashing(const std::shared_ptr<CylonContext> &ctx,
                                              std::shared_ptr<Table> &table, T hash_column,
                                              std::shared_ptr<arrow::Table> &table_out) {
  // partition the tables locally
  std::vector<uint32_t> outPartitions, counts;
  int no_of_partitions = ctx->GetWorldSize();
  RETURN_CYLON_STATUS_IF_FAILED(MapToHashPartitions(table,
                                                    hash_column,
                                                    no_of_partitions,
                                                    outPartitions,
                                                    counts));

  std::vector<std::shared_ptr<arrow::Table>> partitioned_tables;
  RETURN_CYLON_STATUS_IF_FAILED(Split(table, no_of_partitions, outPartitions, counts, partitioned_tables));

  std::shared_ptr<arrow::Schema> schema = table->get_table()->schema();
  // we are going to free if retain is set to false
  if (!table->IsRetain()) {
	table.reset();
  }

  return all_to_all_arrow_tables(ctx, schema, partitioned_tables, table_out);
}

template<typename T>
// T is int32_t or const std::vector<int32_t>&
static inline Status shuffle_two_tables_by_hashing(const std::shared_ptr<cylon::CylonContext> &ctx,
                                                   std::shared_ptr<cylon::Table> &left_table,
                                                   T left_hash_column,
                                                   std::shared_ptr<cylon::Table> &right_table,
                                                   T right_hash_column,
                                                   std::shared_ptr<arrow::Table> &left_table_out,
                                                   std::shared_ptr<arrow::Table> &right_table_out) {
  LOG(INFO) << "Shuffling two tables with total rows : " << left_table->Rows() + right_table->Rows();
  auto t1 = std::chrono::high_resolution_clock::now();
  RETURN_CYLON_STATUS_IF_FAILED(shuffle_table_by_hashing(ctx, left_table, left_hash_column, left_table_out));

  auto t2 = std::chrono::high_resolution_clock::now();
  LOG(INFO) << "Left shuffle time : "
            << std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();

  RETURN_CYLON_STATUS_IF_FAILED(shuffle_table_by_hashing(ctx, right_table, right_hash_column, right_table_out));

  auto t3 = std::chrono::high_resolution_clock::now();
  LOG(INFO) << "Right shuffle time : " << std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();

  return Status::OK();
}

Status FromCSV(const std::shared_ptr<CylonContext> &ctx, const std::string &path,
               std::shared_ptr<Table> &tableOut, const cylon::io::config::CSVReadOptions &options) {
  arrow::Result<std::shared_ptr<arrow::Table>> result = cylon::io::read_csv(ctx, path, options);
  if (result.ok()) {
    std::shared_ptr<arrow::Table> &table = result.ValueOrDie();
    LOG(INFO) << "Chunks " << table->column(0)->chunks().size();
    if (table->column(0)->chunks().size() > 1) {
      const auto &combine_res = table->CombineChunks(ToArrowPool(ctx));
      if (!combine_res.ok()) {
        return Status(static_cast<int>(combine_res.status().code()),
                      combine_res.status().message());
      }
      table = combine_res.ValueOrDie();
    }
    // slice the table if required
    if (options.IsSlice() && ctx->GetWorldSize() > 1) {
      int32_t rows_per_worker = table->num_rows() / ctx->GetWorldSize();
      int32_t remainder = table->num_rows() % ctx->GetWorldSize();

      // first few workers will balance out the remainder
      int32_t balancer = 0;
      if (remainder != 0 && ctx->GetRank() < remainder) {
        balancer = 1;
      }

      // start index should offset the balanced out rows by previous workers
      int32_t offset = ctx->GetRank();
      if (ctx->GetRank() >= remainder) {
        offset = remainder;
      }

      int32_t starting_index = (ctx->GetRank() * rows_per_worker) + offset;
      // LOG(INFO) << ctx->GetRank() << " will start from " << starting_index << " and read "
      //           << (rows_per_worker + balancer) << " offset : " << offset;
      table = table->Slice(starting_index, rows_per_worker + balancer);
    }
    tableOut = std::make_shared<Table>(ctx, table);
    return Status::OK();
  }
  return Status(Code::IOError, result.status().message());
}

Status Table::FromArrowTable(const std::shared_ptr<CylonContext> &ctx,
                             std::shared_ptr<arrow::Table> table,
                             std::shared_ptr<Table> &tableOut) {
  RETURN_CYLON_STATUS_IF_FAILED(tarrow::CheckSupportedTypes(table));
  tableOut = std::make_shared<Table>(ctx, std::move(table));
  return Status::OK();
}

Status Table::FromArrowTable(const std::shared_ptr<CylonContext> &ctx,
                             std::shared_ptr<arrow::Table> table,
                             std::shared_ptr<BaseArrowIndex> index,
                             std::shared_ptr<Table> *output) {
  RETURN_CYLON_STATUS_IF_FAILED(tarrow::CheckSupportedTypes(table));
  *output = std::make_shared<Table>(ctx, std::move(table), std::move(index));
  return Status::OK();
}

Status Table::FromColumns(const std::shared_ptr<CylonContext> &ctx,
                          const std::vector<std::shared_ptr<Column>> &columns,
                          std::shared_ptr<Table> &tableOut) {
  tableOut = std::make_shared<Table>(ctx, columns);
  return Status::OK();
}

Status WriteCSV(const std::shared_ptr<Table> &table, const std::string &path,
                const cylon::io::config::CSVWriteOptions &options) {
  std::ofstream out_csv;
  out_csv.open(path);
  Status status = table->PrintToOStream(
      0, table->get_table()->num_columns(), 0, table->get_table()->num_rows(), out_csv,
      options.GetDelimiter(), options.IsOverrideColumnNames(), options.GetColumnNames());
  out_csv.close();
  return status;
}

int Table::Columns() { return table_->num_columns(); }

std::vector<std::string> Table::ColumnNames() { return table_->ColumnNames(); }

int64_t Table::Rows() { return table_->num_rows(); }

void Table::Print() { Print(0, this->Rows(), 0, this->Columns()); }

void Table::Print(int row1, int row2, int col1, int col2) {
  PrintToOStream(col1, col2, row1, row2, std::cout);
}

Status Merge(const std::vector<std::shared_ptr<cylon::Table>> &ctables, std::shared_ptr<Table> &tableOut) {
  if (!ctables.empty()) {
    std::vector<std::shared_ptr<arrow::Table>> tables;
    tables.reserve(ctables.size());
    for (const auto &t:ctables) {
      if (t->Rows()) {
        std::shared_ptr<arrow::Table> arrow;
        t->ToArrowTable(arrow);
        tables.push_back(std::move(arrow));
      }
    }

    const auto &ctx = ctables[0]->GetContext();
    const auto &concat_res = arrow::ConcatenateTables(tables);
    RETURN_CYLON_STATUS_IF_ARROW_FAILED(concat_res.status());

    auto combined_res = concat_res.ValueOrDie()->CombineChunks(cylon::ToArrowPool(ctx));
    RETURN_CYLON_STATUS_IF_ARROW_FAILED(combined_res.status());

    tableOut = std::make_shared<cylon::Table>(ctx, combined_res.ValueOrDie());
    return Status::OK();
  } else {
	return Status(Code::Invalid, "empty vector passed onto merge");
  }
}

Status Sort(std::shared_ptr<cylon::Table> &table, int sort_column,
			std::shared_ptr<cylon::Table> &out, bool ascending) {
  std::shared_ptr<arrow::Table> sorted_table;
  const auto &table_ = table->get_table();
  const auto &ctx = table->GetContext();
  auto pool = cylon::ToArrowPool(ctx);

  // if num_rows is 0 or 1, we dont need to sort
  if (table->Rows() < 2) {
	RETURN_CYLON_STATUS_IF_ARROW_FAILED(util::Duplicate(table_, pool, sorted_table));
  }

  RETURN_CYLON_STATUS_IF_ARROW_FAILED(cylon::util::SortTable(table_, sort_column, pool, sorted_table, ascending));
  return Table::FromArrowTable(ctx, sorted_table, out);
}

Status Sort(std::shared_ptr<cylon::Table> &table, const std::vector<int32_t> &sort_columns,
			std::shared_ptr<cylon::Table> &out, bool ascending) {
  const std::vector<bool> sort_direction(sort_columns.size(), ascending);
  return Sort(table, sort_columns, out, sort_direction);
}

Status Sort(std::shared_ptr<cylon::Table> &table, const std::vector<int32_t> &sort_columns,
			std::shared_ptr<cylon::Table> &out, const std::vector<bool> &sort_direction) {
  // if single index sort is passed
  if (sort_columns.size() == 1) {
	return Sort(table, sort_columns[0], out, sort_direction[0]);
  }

  std::shared_ptr<arrow::Table> sorted_table;
  auto table_ = table->get_table();
  const auto &ctx = table->GetContext();
  auto pool = cylon::ToArrowPool(ctx);

  // if num_rows is 0 or 1, we dont need to sort
  if (table->Rows() < 2) {
	RETURN_CYLON_STATUS_IF_ARROW_FAILED(util::Duplicate(table_, pool, sorted_table));
  }

  RETURN_CYLON_STATUS_IF_ARROW_FAILED(cylon::util::SortTableMultiColumns(table_,
																		 sort_columns,
																		 pool,
																		 sorted_table,
																		 sort_direction));
  return Table::FromArrowTable(ctx, sorted_table, out);
}

Status DistributedSort(std::shared_ptr<cylon::Table> &table,
					   int sort_column,
					   std::shared_ptr<Table> &output,
					   bool ascending,
					   SortOptions sort_options) {
  return DistributedSort(table, std::vector<int>{sort_column}, output, std::vector<bool>{ascending}, sort_options);
}

Status DistributedSort(std::shared_ptr<cylon::Table> &table,
					   const std::vector<int> &sort_columns,
					   std::shared_ptr<Table> &output,
					   const std::vector<bool> &sort_direction,
					   SortOptions sort_options) {
  const auto &ctx = table->GetContext();
  int world_sz = ctx->GetWorldSize();

  std::shared_ptr<arrow::Table> arrow_table, sorted_table;
  // first do distributed sort partitioning
  if (world_sz == 1) {
	arrow_table = table->get_table();
  } else {
	std::vector<uint32_t> target_partitions, partition_hist;
	std::vector<std::shared_ptr<arrow::Table>> split_tables;

	RETURN_CYLON_STATUS_IF_FAILED(MapToSortPartitions(table, sort_columns[0], world_sz, target_partitions,
													  partition_hist, sort_direction[0], sort_options.num_samples,
													  sort_options.num_bins));

	RETURN_CYLON_STATUS_IF_FAILED(Split(table, world_sz, target_partitions, partition_hist, split_tables));

	// we are going to free if retain is set to false. therefore, we need to make a copy of schema
	std::shared_ptr<arrow::Schema> schema = table->get_table()->schema();
	if (!table->IsRetain()) table.reset();

	RETURN_CYLON_STATUS_IF_FAILED(all_to_all_arrow_tables(ctx, schema, split_tables, arrow_table));
  }

  // then do a local sort
  if (sort_columns.size() == 1) {
	RETURN_CYLON_STATUS_IF_ARROW_FAILED(util::SortTable(arrow_table,
														sort_columns[0],
														ToArrowPool(ctx),
														sorted_table, sort_direction[0]));
  } else {
	RETURN_CYLON_STATUS_IF_ARROW_FAILED(util::SortTableMultiColumns(arrow_table,
																	sort_columns,
																	ToArrowPool(ctx),
																	sorted_table,
																	sort_direction));
  }

  return Table::FromArrowTable(ctx, sorted_table, output);
}

Status HashPartition(std::shared_ptr<cylon::Table> &table, const std::vector<int> &hash_columns,
					 int no_of_partitions,
					 std::unordered_map<int, std::shared_ptr<cylon::Table>> *out) {
  // keep arrays for each target, these arrays are used for creating the table
  std::vector<uint32_t> outPartitions, counts;
  RETURN_CYLON_STATUS_IF_FAILED(MapToHashPartitions(table,
                                                    hash_columns,
                                                    no_of_partitions,
                                                    outPartitions,
                                                    counts));

  std::vector<std::shared_ptr<arrow::Table>> partitioned_tables;
  RETURN_CYLON_STATUS_IF_FAILED(Split(table, no_of_partitions, outPartitions, counts, partitioned_tables));

  const auto &ctx = table->GetContext();
  out->reserve(no_of_partitions);
  for (int i = 0; i < no_of_partitions; i++) {
    out->emplace(i, std::make_shared<Table>(ctx, partitioned_tables[i]));
  }

  return Status::OK();
}

arrow::Status create_table_with_duplicate_index(arrow::MemoryPool *pool,
                                                std::shared_ptr<arrow::Table> &table,
                                                size_t index_column) {
  const std::vector<std::shared_ptr<arrow::ChunkedArray>> &chunk_arrays = table->columns();
  std::vector<std::shared_ptr<arrow::ChunkedArray>> new_arrays;
  new_arrays.reserve(chunk_arrays.size());
  for (size_t i = 0; i < chunk_arrays.size(); i++) {
    if (i != index_column) {
      new_arrays.push_back(chunk_arrays[i]);
    } else {
      std::shared_ptr<arrow::ChunkedArray> new_c_array;
      RETURN_ARROW_STATUS_IF_FAILED(cylon::util::Duplicate(chunk_arrays[i], pool, new_c_array));
      new_arrays.emplace_back(std::move(new_c_array));
    }
  }
  table = arrow::Table::Make(table->schema(), std::move(new_arrays));
  return arrow::Status::OK();
}

Status Join(std::shared_ptr<cylon::Table> &left, std::shared_ptr<cylon::Table> &right,
			const join::config::JoinConfig &join_config, std::shared_ptr<cylon::Table> &out) {
  if (left == NULLPTR) {
	return Status(Code::KeyError, "Couldn't find the left table");
  } else if (right == NULLPTR) {
	return Status(Code::KeyError, "Couldn't find the right table");
  } else {
    std::shared_ptr<arrow::Table> table, left_table, right_table;
    const auto &ctx = left->GetContext();
    auto pool = cylon::ToArrowPool(ctx);

    left->ToArrowTable(left_table);
    right->ToArrowTable(right_table);
    // if it is a sort algorithm and certain key types, we are going to do an in-place sort
    if (!join_config.IsMultiColumn() && join_config.GetAlgorithm() == cylon::join::config::SORT) {
      int lIndex = join_config.GetLeftColumnIdx()[0];
      int rIndex = join_config.GetRightColumnIdx()[0];
      auto left_type = left_table->column(lIndex)->type()->id();
      if (cylon::join::util::is_inplace_join_possible(left_type)) {
        // we don't have to copy if the table is freed
        if (left->IsRetain()) {
          RETURN_CYLON_STATUS_IF_ARROW_FAILED(create_table_with_duplicate_index(pool, left_table, lIndex));
        }
        if (right->IsRetain()) {
          RETURN_CYLON_STATUS_IF_ARROW_FAILED(create_table_with_duplicate_index(pool, right_table, rIndex));
        }
      }
    }

    RETURN_CYLON_STATUS_IF_ARROW_FAILED(join::JoinTables(left_table, right_table, join_config, &table, pool));
    out = std::make_shared<cylon::Table>(ctx, table);

    return Status::OK();
  }
}

Status Table::ToArrowTable(std::shared_ptr<arrow::Table> &out) {
  out = table_;
  return Status::OK();
}

Status DistributedJoin(std::shared_ptr<cylon::Table> &left, std::shared_ptr<cylon::Table> &right,
                       const join::config::JoinConfig &join_config,
                       std::shared_ptr<cylon::Table> &out) {
  // check whether the world size is 1
  const auto &ctx = left->GetContext();
  if (ctx->GetWorldSize() == 1) {
    return Join(left, right, join_config, out);
  }

  std::shared_ptr<arrow::Table> left_final_table, right_final_table;
  RETURN_CYLON_STATUS_IF_FAILED(shuffle_two_tables_by_hashing(ctx, left, join_config.GetLeftColumnIdx(),
                                                              right, join_config.GetRightColumnIdx(),
                                                              left_final_table, right_final_table));

  std::shared_ptr<arrow::Table> table;
  RETURN_CYLON_STATUS_IF_ARROW_FAILED(join::JoinTables(left_final_table, right_final_table, join_config, &table,
                                                       cylon::ToArrowPool(ctx)));
    out = std::make_shared<cylon::Table>(ctx, table);

  return Status::OK();
}

Status Select(std::shared_ptr<cylon::Table> &table, const std::function<bool(cylon::Row)> &selector,
			  std::shared_ptr<Table> &out) {
  // boolean builder to hold the mask
  const auto &ctx = table->GetContext();
  const auto &table_ = table->get_table();
  auto row = cylon::Row(table_);
  auto pool = cylon::ToArrowPool(ctx);
  std::shared_ptr<arrow::Table> out_table;

  auto kI = table->Rows();
  if (kI) {
	arrow::BooleanBuilder boolean_builder(pool);
	RETURN_CYLON_STATUS_IF_ARROW_FAILED(boolean_builder.Reserve(kI));

	for (int64_t row_index = 0; row_index < kI; row_index++) {
	  row.SetIndex(row_index);
	  boolean_builder.UnsafeAppend(selector(row));
	}
	// building the mask
	std::shared_ptr<arrow::Array> mask;
	RETURN_CYLON_STATUS_IF_ARROW_FAILED(boolean_builder.Finish(&mask));

	const arrow::Result<arrow::Datum> &filter_res = arrow::compute::Filter(table_, mask);
	RETURN_CYLON_STATUS_IF_ARROW_FAILED(filter_res.status());

	out_table = filter_res.ValueOrDie().table();
  } else {
	RETURN_CYLON_STATUS_IF_ARROW_FAILED(util::Duplicate(table_, pool, out_table));
  }
  out = std::make_shared<cylon::Table>(ctx, out_table);
  return Status::OK();
}

Status Union(const std::shared_ptr<Table> &first, const std::shared_ptr<Table> &second, std::shared_ptr<Table> &out) {
  std::shared_ptr<arrow::Table> ltab = first->get_table();
  std::shared_ptr<arrow::Table> rtab = second->get_table();
  const auto &ctx = first->GetContext();
  auto pool = ToArrowPool(ctx);

  COMBINE_CHUNKS_RETURN_CYLON_STATUS(ltab, pool);
  COMBINE_CHUNKS_RETURN_CYLON_STATUS(rtab, pool);

  RETURN_CYLON_STATUS_IF_FAILED(VerifyTableSchema(ltab, rtab));

  TwoTableRowIndexHash hash(ltab, rtab);
  TwoTableRowIndexEqualTo equal_to(ltab, rtab);

  const auto buckets_pre_alloc = (ltab->num_rows() + rtab->num_rows());
  ska::bytell_hash_set<int64_t, TwoTableRowIndexHash, TwoTableRowIndexEqualTo>
	  rows_set(buckets_pre_alloc, hash, equal_to);

  arrow::compute::ExecContext exec_context(pool);

  std::shared_ptr<arrow::Array> mask;

  // insert first table to the row set
  arrow::BooleanBuilder mask_builder(pool);
  RETURN_CYLON_STATUS_IF_ARROW_FAILED(mask_builder.Reserve(ltab->num_rows()));
  for (int64_t i = 0; i < ltab->num_rows(); i++) {
	const auto &res = rows_set.insert(i);
	// if res.second == true: it is a unique value
	// else: its already available
	mask_builder.UnsafeAppend(res.second);
  }
  RETURN_CYLON_STATUS_IF_ARROW_FAILED(mask_builder.Finish(&mask));

  const auto &options = arrow::compute::FilterOptions::Defaults();
  const arrow::Result<arrow::Datum> &l_res = arrow::compute::Filter(ltab, mask, options, &exec_context);
  RETURN_CYLON_STATUS_IF_ARROW_FAILED(l_res.status());

  // filtered first table
  const std::shared_ptr<arrow::Table> &f_ltab = l_res.ValueOrDie().table();

  // insert second table to the row set
  mask_builder.Reset();
  mask.reset();
  RETURN_CYLON_STATUS_IF_ARROW_FAILED(mask_builder.Reserve(rtab->num_rows()));
  for (int64_t i = 0; i < rtab->num_rows(); i++) {
	// setting the leading bit to 1 since we are inserting the second table
	const auto &res = rows_set.insert(util::SetBit(i));
	// if res.second == true: it is a unique value
	// else: its already available
	mask_builder.UnsafeAppend(res.second);
  }
  RETURN_CYLON_STATUS_IF_ARROW_FAILED(mask_builder.Finish(&mask));

  const arrow::Result<arrow::Datum> &r_res = arrow::compute::Filter(rtab, mask, options, &exec_context);
  RETURN_CYLON_STATUS_IF_ARROW_FAILED(r_res.status());

  // filtered second table
  const std::shared_ptr<arrow::Table> &f_rtab = r_res.ValueOrDie().table();

  // concat filtered tables
  const auto
	  &concat_res = arrow::ConcatenateTables({f_ltab, f_rtab}, arrow::ConcatenateTablesOptions::Defaults(), pool);
  RETURN_CYLON_STATUS_IF_ARROW_FAILED(concat_res.status());

  // combine chunks
  auto merge_res = concat_res.ValueOrDie()->CombineChunks();
  RETURN_CYLON_STATUS_IF_ARROW_FAILED(merge_res.status());

  out = std::make_shared<cylon::Table>(ctx, merge_res.ValueOrDie());
  return Status::OK();
}

Status Subtract(const std::shared_ptr<Table> &first, const std::shared_ptr<Table> &second,
				std::shared_ptr<Table> &out) {
  std::shared_ptr<arrow::Table> ltab = first->get_table();
  std::shared_ptr<arrow::Table> rtab = second->get_table();
  const auto &ctx = first->GetContext();
  auto pool = ToArrowPool(ctx);

  COMBINE_CHUNKS_RETURN_CYLON_STATUS(ltab, pool);
  COMBINE_CHUNKS_RETURN_CYLON_STATUS(rtab, pool);

  RETURN_CYLON_STATUS_IF_FAILED(VerifyTableSchema(ltab, rtab));

  TwoTableRowIndexHash hash(ltab, rtab);
  TwoTableRowIndexEqualTo equal_to(ltab, rtab);

  const auto buckets_pre_alloc = ltab->num_rows();
  ska::bytell_hash_set<int64_t, TwoTableRowIndexHash, TwoTableRowIndexEqualTo>
	  rows_set(buckets_pre_alloc, hash, equal_to);

  arrow::compute::ExecContext exec_context(pool);

  // create a bitmask
  std::vector<bool> bitmask(ltab->num_rows());

  // insert left table to row_set
  for (int64_t i = 0; i < ltab->num_rows(); i++) {
	const auto &res = rows_set.insert(i);
	bitmask[i] = res.second;
  }

  // let's probe right rows against the rows set
  for (int64_t i = 0; i < rtab->num_rows(); i++) {
	// setting the leading bit to 1 since we are inserting the second table
	const auto &res = rows_set.find(util::SetBit(i));
	if (res != rows_set.end()) {
	  bitmask[*res] = false;
	}
  }

  // convert vector<bool> to BooleanArray
  arrow::BooleanBuilder builder;
  RETURN_CYLON_STATUS_IF_ARROW_FAILED(builder.AppendValues(bitmask));
  std::shared_ptr<arrow::Array> mask;
  RETURN_CYLON_STATUS_IF_ARROW_FAILED(builder.Finish(&mask));

  const arrow::Result<arrow::Datum> &l_res = arrow::compute::Filter(ltab,
																	mask,
																	arrow::compute::FilterOptions::Defaults(),
																	&exec_context);
  RETURN_CYLON_STATUS_IF_ARROW_FAILED(l_res.status());

  // filtered second table
  std::shared_ptr<arrow::Table> intersect_tab = l_res.ValueOrDie().table();

  out = std::make_shared<cylon::Table>(ctx, intersect_tab);
  return Status::OK();
}

Status Intersect(const std::shared_ptr<Table> &first,
				 const std::shared_ptr<Table> &second,
				 std::shared_ptr<Table> &out) {
  std::shared_ptr<arrow::Table> ltab = first->get_table();
  std::shared_ptr<arrow::Table> rtab = second->get_table();

  const auto &ctx = first->GetContext();
  auto pool = ToArrowPool(ctx);

  COMBINE_CHUNKS_RETURN_CYLON_STATUS(ltab, pool);
  COMBINE_CHUNKS_RETURN_CYLON_STATUS(rtab, pool);

  RETURN_CYLON_STATUS_IF_FAILED(VerifyTableSchema(ltab, rtab));

  TwoTableRowIndexHash hash(ltab, rtab);
  TwoTableRowIndexEqualTo equal_to(ltab, rtab);

  const auto buckets_pre_alloc = ltab->num_rows();
  ska::bytell_hash_set<int64_t, TwoTableRowIndexHash, TwoTableRowIndexEqualTo>
	  rows_set(buckets_pre_alloc, hash, equal_to);

  arrow::compute::ExecContext exec_context(pool);

  // insert left table to row_set
  for (int64_t i = 0; i < ltab->num_rows(); i++) {
	rows_set.insert(i);
  }

  // create a bitmask
  std::vector<bool> bitmask(ltab->num_rows(), false);

  // let's probe right rows against the rows set
  for (int64_t i = 0; i < rtab->num_rows(); i++) {
	// setting the leading bit to 1 since we are inserting the second table
	const auto &res = rows_set.find(util::SetBit(i));
	if (res != rows_set.end()) {
	  bitmask[*res] = true;
	}
  }

  // convert vector<bool> to BooleanArray
  arrow::BooleanBuilder builder;
  RETURN_CYLON_STATUS_IF_ARROW_FAILED(builder.AppendValues(bitmask));
  std::shared_ptr<arrow::Array> mask;
  RETURN_CYLON_STATUS_IF_ARROW_FAILED(builder.Finish(&mask));

  const arrow::Result<arrow::Datum> &l_res = arrow::compute::Filter(ltab,
																	mask,
																	arrow::compute::FilterOptions::Defaults(),
																	&exec_context);
  RETURN_CYLON_STATUS_IF_ARROW_FAILED(l_res.status());

  // filtered second table
  std::shared_ptr<arrow::Table> intersect_tab = l_res.ValueOrDie().table();

  out = std::make_shared<cylon::Table>(ctx, intersect_tab);
  return Status::OK();
}

typedef Status (*LocalSetOperation)(const std::shared_ptr<cylon::Table> &,
									const std::shared_ptr<cylon::Table> &,
									std::shared_ptr<cylon::Table> &);

static inline Status do_dist_set_op(LocalSetOperation local_operation,
									std::shared_ptr<cylon::Table> &table_left,
									std::shared_ptr<cylon::Table> &table_right,
									std::shared_ptr<cylon::Table> &out) {
  // extract the tables out
  auto left = table_left->get_table();
  auto right = table_right->get_table();
  const auto &ctx = table_left->GetContext();

  RETURN_CYLON_STATUS_IF_FAILED(VerifyTableSchema(left, right));

  if (ctx->GetWorldSize() == 1) {
	return local_operation(table_left, table_right, out);
  }

  std::vector<int32_t> hash_columns;
  hash_columns.reserve(left->num_columns());
  for (int kI = 0; kI < left->num_columns(); ++kI) {
    hash_columns.push_back(kI);
  }

  std::shared_ptr<arrow::Table> left_final_table;
  std::shared_ptr<arrow::Table> right_final_table;
  RETURN_CYLON_STATUS_IF_FAILED(shuffle_two_tables_by_hashing(ctx, table_left, hash_columns, table_right, hash_columns,
                                                              left_final_table, right_final_table));

  std::shared_ptr<cylon::Table> left_tab = std::make_shared<cylon::Table>(ctx, left_final_table);
  std::shared_ptr<cylon::Table> right_tab = std::make_shared<cylon::Table>(ctx, right_final_table);
  // now do the local union
  std::shared_ptr<arrow::Table> table;
  return local_operation(left_tab, right_tab, out);
}

Status DistributedUnion(std::shared_ptr<Table> &left, std::shared_ptr<Table> &right,
						std::shared_ptr<Table> &out) {
  return do_dist_set_op(&Union, left, right, out);
}

Status DistributedSubtract(std::shared_ptr<Table> &left, std::shared_ptr<Table> &right,
						   std::shared_ptr<Table> &out) {
  return do_dist_set_op(&Subtract, left, right, out);
}

Status DistributedIntersect(std::shared_ptr<Table> &left, std::shared_ptr<Table> &right,
							std::shared_ptr<Table> &out) {
  return do_dist_set_op(&Intersect, left, right, out);
}

void Table::Clear() {}

Table::~Table() { this->Clear(); }

void ReadCSVThread(const std::shared_ptr<CylonContext> &ctx, const std::string &path,
				   std::shared_ptr<cylon::Table> *table,
				   const cylon::io::config::CSVReadOptions &options,
				   const std::shared_ptr<std::promise<Status>> &status_promise) {
//  const std::shared_ptr<CylonContext> &ctx_ = ctx;  // make a copy of the shared ptr
  status_promise->set_value(FromCSV(ctx, path, *table, options));
}

Status FromCSV(const std::shared_ptr<CylonContext> &ctx, const std::vector<std::string> &paths,
               const std::vector<std::shared_ptr<Table> *> &tableOuts,
               const io::config::CSVReadOptions &options) {
  if (options.IsConcurrentFileReads()) {
    std::vector<std::pair<std::future<Status>, std::thread>> futures;
    futures.reserve(paths.size());
    for (uint64_t kI = 0; kI < paths.size(); ++kI) {
      auto read_promise = std::make_shared<std::promise<Status>>();
      //	  auto context = ctx.get();
      futures.emplace_back(
          read_promise->get_future(),
          std::thread(ReadCSVThread,
                      std::cref(ctx),
                      std::cref(paths[kI]),
                      tableOuts[kI],
                      std::cref(options),
                      read_promise));
    }
    bool all_passed = true;
    for (auto &future : futures) {
      auto status = future.first.get();
      all_passed &= status.is_ok();
      future.second.join();
    }
    return all_passed ? Status::OK() : Status(cylon::IOError, "Failed to read the csv files");
  } else {
	auto status = Status::OK();
	for (std::size_t kI = 0; kI < paths.size(); ++kI) {
	  status = FromCSV(ctx, paths[kI], *tableOuts[kI], options);
	  if (!status.is_ok()) {
		return status;
	  }
	}
	return status;
  }
}

Status Project(std::shared_ptr<cylon::Table> &table, const std::vector<int32_t> &project_columns,
			   std::shared_ptr<Table> &out) {
  std::vector<std::shared_ptr<arrow::Field>> schema_vector;
  std::vector<std::shared_ptr<arrow::ChunkedArray>> column_arrays;
  schema_vector.reserve(project_columns.size());
  column_arrays.reserve(project_columns.size());

  auto table_ = table->get_table();
  const auto &ctx = table->GetContext();

  for (auto const &col_index : project_columns) {
	schema_vector.push_back(table_->field(col_index));
	column_arrays.push_back(table_->column(col_index));
  }

  auto schema = std::make_shared<arrow::Schema>(schema_vector);
  std::shared_ptr<arrow::Table> ar_table = arrow::Table::Make(schema, column_arrays);
  out = std::make_shared<cylon::Table>(ctx, ar_table);
  return Status::OK();
}

Status Table::PrintToOStream(int col1, int col2, int row1, int row2, std::ostream &out,
							 char delimiter, bool use_custom_header,
							 const std::vector<std::string> &headers) {
  auto table = table_;
  if (table != NULLPTR) {
	// print the headers
	if (use_custom_header) {
	  // check if the headers are valid
	  if (headers.size() != (uint64_t)table->num_columns()) {
		return Status(
			cylon::Code::IndexError,
			"Provided headers doesn't match with the number of columns of the table. Given " +
				std::to_string(headers.size()) + ", Expected " +
				std::to_string(table->num_columns()));
	  }

	  for (int col = col1; col < col2; col++) {
		out << headers[col];
		if (col != col2 - 1) {
		  out << delimiter;
		} else {
		  out << std::endl;
		}
	  }
	}
	for (int row = row1; row < row2; row++) {
	  for (int col = col1; col < col2; col++) {
		auto column = table->column(col);
		int rowCount = 0;
		for (int chunk = 0; chunk < column->num_chunks(); chunk++) {
		  auto array = column->chunk(chunk);
		  if (rowCount <= row && rowCount + array->length() > row) {
			// print this array
			out << cylon::util::array_to_string(array, row - rowCount);
			if (col != col2 - 1) {
			  out << delimiter;
			}
			break;
		  }
		  rowCount += array->length();
		}
	  }
	  out << std::endl;
	}
  }
  return Status(Code::OK);
}

const std::shared_ptr<arrow::Table> &Table::get_table() const { return table_; }

bool Table::IsRetain() const { return retain_; }

std::shared_ptr<Column> Table::GetColumn(int32_t index) const { return this->columns_.at(index); }

const std::vector<std::shared_ptr<cylon::Column>> &Table::GetColumns() const {
  return columns_;
}

Status Shuffle(std::shared_ptr<cylon::Table> &table, const std::vector<int> &hash_columns,
			   std::shared_ptr<cylon::Table> &output) {
  const auto &ctx_ = table->GetContext();
  std::shared_ptr<arrow::Table> table_out;
  cylon::Status status = shuffle_table_by_hashing(ctx_, table, hash_columns, table_out);

  if (!status.is_ok()) {
	LOG(FATAL) << "table shuffle failed!";
	return status;
  }
  return cylon::Table::FromArrowTable(ctx_, table_out, output);
}

Status Unique(std::shared_ptr<cylon::Table> &in, const std::vector<int> &cols,
			  std::shared_ptr<cylon::Table> &out, bool first) {
#ifdef CYLON_DEBUG
  auto p1 = std::chrono::high_resolution_clock::now();
#endif
  const auto &ctx = in->GetContext();
  auto pool = cylon::ToArrowPool(ctx);
  std::shared_ptr<arrow::Table> out_table, in_table = in->get_table();

  if (in_table->num_rows() > 1) {
	if (in_table->column(0)->num_chunks() > 1) {
	  const arrow::Result<std::shared_ptr<arrow::Table>> &res = in_table->CombineChunks(pool);
	  RETURN_CYLON_STATUS_IF_ARROW_FAILED(res.status());
	  in_table = res.ValueOrDie();
	}

	TableRowIndexEqualTo row_comp(in_table, cols);
	TableRowIndexHash row_hash(in_table, cols);
	const int64_t num_rows = in_table->num_rows();
	ska::bytell_hash_set<int64_t, TableRowIndexHash, TableRowIndexEqualTo> rows_set(num_rows, row_hash, row_comp);

	arrow::BooleanBuilder filter(pool);
	RETURN_CYLON_STATUS_IF_ARROW_FAILED(filter.Reserve(num_rows));
#ifdef CYLON_DEBUG
	auto p2 = std::chrono::high_resolution_clock::now();
#endif
	if (first) {
	  for (int64_t row = 0; row < num_rows; ++row) {
		const auto &res = rows_set.insert(row);
		filter.UnsafeAppend(res.second);
	  }
	} else {
	  for (int64_t row = num_rows - 1; row > 0; --row) {
		const auto &res = rows_set.insert(row);
		filter.UnsafeAppend(res.second);
	  }
	}
#ifdef CYLON_DEBUG
	auto p3 = std::chrono::high_resolution_clock::now();

	//  rows_set.clear();
	auto p4 = std::chrono::high_resolution_clock::now();
#endif
	std::shared_ptr<arrow::BooleanArray> filter_arr;
	RETURN_CYLON_STATUS_IF_ARROW_FAILED(filter.Finish(&filter_arr));

	const arrow::Result<arrow::Datum> &res = arrow::compute::Filter(in_table, filter_arr);
	RETURN_CYLON_STATUS_IF_ARROW_FAILED(res.status());
	out_table = res.ValueOrDie().table();
  } else {
	RETURN_CYLON_STATUS_IF_ARROW_FAILED(util::Duplicate(in_table, pool, out_table));
  }
  out = std::make_shared<Table>(ctx, out_table);
#ifdef CYLON_DEBUG
  auto p5 = std::chrono::high_resolution_clock::now();
  LOG(INFO) << "P1 " << std::chrono::duration_cast<std::chrono::milliseconds>(p2 - p1).count()
			<< " P2 " << std::chrono::duration_cast<std::chrono::milliseconds>(p3 - p2).count()
			<< " P3 " << std::chrono::duration_cast<std::chrono::milliseconds>(p4 - p3).count()
			<< " P4 " << std::chrono::duration_cast<std::chrono::milliseconds>(p5 - p4).count()
			<< " tot " << std::chrono::duration_cast<std::chrono::milliseconds>(p5 - p1).count();
#endif
  return Status::OK();
}

Status DistributedUnique(std::shared_ptr<cylon::Table> &in, const std::vector<int> &cols,
						 std::shared_ptr<cylon::Table> &out) {
  const auto &ctx = in->GetContext();
  if (ctx->GetWorldSize() == 1) {
	return Unique(in, cols, out);
  }

  std::shared_ptr<cylon::Table> shuffle_out;
  RETURN_CYLON_STATUS_IF_FAILED(cylon::Shuffle(in, cols, shuffle_out));

  return Unique(shuffle_out, cols, out);
}

const std::shared_ptr<BaseArrowIndex> &Table::GetArrowIndex() { return base_arrow_index_; }

Status Table::SetArrowIndex(std::shared_ptr<BaseArrowIndex> index, bool drop_index) {
  // move the copy to local index
  base_arrow_index_ = std::move(index);

  // if the index has a valid column id, drop it
  if (drop_index && base_arrow_index_->GetColId() > 0) {
    const auto &res = table_->RemoveColumn(base_arrow_index_->GetColId());
    RETURN_CYLON_STATUS_IF_ARROW_FAILED(res.status());
    table_ = res.ValueOrDie();
  }

  return Status::OK();
}

Status Table::ResetArrowIndex(bool drop) {
  // if the current index is a range index, nothing to do!
  if (base_arrow_index_->GetIndexingType() != Range) {
    // if not drop (i.e. preserve) index, add it as the 0'th column
    if (!drop) {
      // take a copy of the array in the index and set add it as the 0th column
      auto index_arr = base_arrow_index_->GetIndexArray();
      RETURN_CYLON_STATUS_IF_FAILED(AddColumn(0, "index", std::move(index_arr)));
    }

    base_arrow_index_ = BuildRangeIndex(table_, 0, -1, 1, cylon::ToArrowPool(ctx));
  }
  return Status::OK();
}

Status Table::AddColumn(int position, std::string column_name, std::shared_ptr<arrow::Array> input_column) {
  if (input_column->length() != table_->num_rows()) {
    LOG(ERROR) << "New column length must match the number of rows in the table";
    return Status(cylon::Code::CapacityError);
  }
  auto field = std::make_shared<arrow::Field>(std::move(column_name), input_column->type());
  auto chunked_array = std::make_shared<arrow::ChunkedArray>(std::move(input_column));
  const auto &result = table_->AddColumn(position, std::move(field), std::move(chunked_array));
  RETURN_CYLON_STATUS_IF_ARROW_FAILED(result.status());

  table_ = result.ValueOrDie();
  return Status::OK();
}

const std::shared_ptr<cylon::CylonContext> &Table::GetContext() const {
  return ctx;
}

Table::Table(const std::shared_ptr<CylonContext> &ctx, std::shared_ptr<arrow::Table> tab)
    : ctx(ctx), table_(std::move(tab)), columns_({}), base_arrow_index_(BuildRangeIndex(table_)) {
  columns_.reserve(table_->num_columns());
  for (int i = 0; i < table_->num_columns(); i++) {
    const std::shared_ptr<arrow::Field> &field = table_->field(i);
    columns_.emplace_back(Column::Make(field->name(), tarrow::ToCylonType(field->type()), table_->column(i)));
  }
}

Table::Table(const std::shared_ptr<CylonContext> &ctx,
             std::shared_ptr<arrow::Table> tab,
             std::shared_ptr<BaseArrowIndex> index)
    : ctx(ctx), table_(std::move(tab)), base_arrow_index_(std::move(index)) {
  columns_.reserve(table_->num_columns());
  for (int i = 0; i < table_->num_columns(); i++) {
    const std::shared_ptr<arrow::Field> &field = table_->field(i);
    columns_.emplace_back(Column::Make(field->name(), tarrow::ToCylonType(field->type()), table_->column(i)));
  }
}

Table::Table(const std::shared_ptr<cylon::CylonContext> &ctx, std::vector<std::shared_ptr<Column>> cols)
    : ctx(ctx), columns_(std::move(cols)) {
  arrow::SchemaBuilder schema_builder;
  std::vector<std::shared_ptr<arrow::ChunkedArray>> col_arrays;
  col_arrays.reserve(cols.size());

  for (const std::shared_ptr<Column> &col: columns_) {
    const std::shared_ptr<DataType> &data_type = col->GetDataType();
    const std::shared_ptr<arrow::Field>
        &field = arrow::field(col->GetID(), cylon::tarrow::convertToArrowType(data_type));
    const auto &status = schema_builder.AddField(field);
    if (!status.ok()) {
      throw "unable to add field to arrow schema: " + status.message();
    }

    col_arrays.push_back(col->GetColumnData());
  }

  const auto &schema_result = schema_builder.Finish();
  table_ = arrow::Table::Make(schema_result.ValueOrDie(), std::move(col_arrays));

  if (!cylon::tarrow::validateArrowTableTypes(table_)) {
    throw "cylon table created with invalid types";
  }

  base_arrow_index_ = BuildRangeIndex(table_);
}

Status Table::CombineChunks() {
  if (table_->column(0)->num_chunks() > 1) {
    const auto &res = table_->CombineChunks(cylon::ToArrowPool(ctx));
    RETURN_CYLON_STATUS_IF_ARROW_FAILED(res.status());
    table_ = res.ValueOrDie();
  }

  return Status::OK();
}

#ifdef BUILD_CYLON_PARQUET
Status FromParquet(const std::shared_ptr<CylonContext> &ctx, const std::string &path,
                   std::shared_ptr<Table> &tableOut) {
  arrow::Result<std::shared_ptr<arrow::Table>> result = cylon::io::ReadParquet(ctx, path);
  if (result.ok()) {
    std::shared_ptr<arrow::Table> table = result.ValueOrDie();
    LOG(INFO) << "Chunks " << table->column(0)->chunks().size();
    if (table->column(0)->chunks().size() > 1) {
      auto combine_res = table->CombineChunks(ToArrowPool(ctx));
      if (!combine_res.ok()) {
        return Status(static_cast<int>(combine_res.status().code()),
                      combine_res.status().message());
      }
      tableOut = std::make_shared<Table>(ctx, combine_res.ValueOrDie());
    } else {
      tableOut = std::make_shared<Table>(ctx, table);
    }
    return Status::OK();
  }
  return Status(Code::IOError, result.status().message());
}

void ReadParquetThread(const std::shared_ptr<CylonContext> &ctx, const std::string &path,
                       std::shared_ptr<cylon::Table> *table,
                       const std::shared_ptr<std::promise<Status>> &status_promise) {
  status_promise->set_value(FromParquet(ctx, path, *table));
}

Status FromParquet(const std::shared_ptr<CylonContext> &ctx, const std::vector<std::string> &paths,
                   const std::vector<std::shared_ptr<Table> *> &tableOuts,
                   const io::config::ParquetOptions &options) {
  if (options.IsConcurrentFileReads()) {
    std::vector<std::pair<std::future<Status>, std::thread>> futures;
    futures.reserve(paths.size());
    for (uint64_t kI = 0; kI < paths.size(); ++kI) {
      auto read_promise = std::make_shared<std::promise<Status>>();
      futures.emplace_back(
          read_promise->get_future(),
          std::thread(ReadParquetThread, std::cref(ctx), std::cref(paths[kI]), tableOuts[kI], read_promise));
    }
    bool all_passed = true;
    for (auto &future : futures) {
      auto status = future.first.get();
      all_passed &= status.is_ok();
      future.second.join();
    }
    return all_passed ? Status::OK() : Status(cylon::IOError, "Failed to read the parquet files");
  } else {
    auto status = Status::OK();
    for (std::size_t kI = 0; kI < paths.size(); ++kI) {
      status = FromParquet(ctx, paths[kI], *tableOuts[kI]);
      if (!status.is_ok()) {
        return status;
      }
    }
    return status;
  }
}

Status WriteParquet(const std::shared_ptr<cylon::CylonContext> &ctx_,
                    std::shared_ptr<cylon::Table> &table,
                    const std::string &path,
                    const io::config::ParquetOptions &options) {
  arrow::Status writefile_result = cylon::io::WriteParquet(ctx_, table, path, options);
  if (!writefile_result.ok()) {
    return Status(Code::IOError, writefile_result.message());
  }

  return Status(Code::OK);
}
#endif
}  // namespace cylon
