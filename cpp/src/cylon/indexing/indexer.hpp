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

#ifndef CYLON_SRC_CYLON_INDEXING_INDEXER_H_
#define CYLON_SRC_CYLON_INDEXING_INDEXER_H_

#include "cylon/indexing/index.hpp"
#include "cylon/table.hpp"

namespace cylon {
namespace indexing {

bool CheckIsIndexValueUnique(const std::shared_ptr<arrow::Scalar> &index_value,
                             const std::shared_ptr<BaseArrowIndex> &index);

/**
* locate values in [start_index:end_index] and take only the given columns.
* @param input_table
* @param start_value
* @param end_value
* @param columns
* @param output
* @return
*/
Status Loc(const std::shared_ptr<Table> &input_table,
           const std::shared_ptr<arrow::Scalar> &start_value,
           const std::shared_ptr<arrow::Scalar> &end_value,
           const std::vector<int> &columns,
           std::shared_ptr<Table> *output);

Status Loc(const std::shared_ptr<Table> &input_table,
           const std::shared_ptr<arrow::Scalar> &start_index,
           const std::shared_ptr<arrow::Scalar> &end_index,
           int start_column,
           int end_column,
           std::shared_ptr<Table> *output);

Status Loc(const std::shared_ptr<Table> &input_table,
           const std::shared_ptr<arrow::Scalar> &start_index,
           const std::shared_ptr<arrow::Scalar> &end_index,
           int column_index,
           std::shared_ptr<Table> *output);

Status Loc(const std::shared_ptr<Table> &input_table,
           const std::shared_ptr<arrow::Array> &values,
           const std::vector<int> &columns,
           std::shared_ptr<Table> *output);

Status Loc(const std::shared_ptr<Table> &input_table,
           const std::shared_ptr<arrow::Array> &indices,
           int column_index,
           std::shared_ptr<Table> *output);

Status Loc(const std::shared_ptr<Table> &input_table,
           const std::shared_ptr<arrow::Array> &indices,
           int start_column, int end_column, std::shared_ptr<Table> *output);

Status iLoc(const std::shared_ptr<Table> &input_table,
            const std::shared_ptr<arrow::Scalar> &start_index,
            const std::shared_ptr<arrow::Scalar> &end_index,
            const std::vector<int> &columns,
            std::shared_ptr<Table> *output);

Status iLoc(const std::shared_ptr<Table> &input_table,
            const std::shared_ptr<arrow::Scalar> &start_index,
            const std::shared_ptr<arrow::Scalar> &end_index,
            int column_index,
            std::shared_ptr<Table> *output);

Status iLoc(const std::shared_ptr<Table> &input_table,
            const std::shared_ptr<arrow::Scalar> &start_index,
            const std::shared_ptr<arrow::Scalar> &end_index,
            int start_column,
            int end_column,
            std::shared_ptr<Table> *output);

Status iLoc(const std::shared_ptr<Table> &input_table,
            const std::shared_ptr<arrow::Array> &indices,
            const std::vector<int> &columns,
            std::shared_ptr<Table> *output);

Status iLoc(const std::shared_ptr<Table> &input_table,
            const std::shared_ptr<arrow::Array> &indices,
            int column_index,
            std::shared_ptr<Table> *output);

Status iLoc(const std::shared_ptr<Table> &input_table,
            const std::shared_ptr<arrow::Array> &indices,
            int start_column, int end_column,
            std::shared_ptr<Table> *output);

} // namespace indexing
} // namespace cylon

#endif //CYLON_SRC_CYLON_INDEXING_INDEXER_H_
