#include "index_utils.hpp"

cylon::Status cylon::IndexUtil::Build(std::shared_ptr<cylon::BaseIndex> &index,
                                      std::shared_ptr<cylon::Table> &input,
                                      int index_column) {

  std::shared_ptr<arrow::Table> arrow_out;

  auto table_ = input->get_table();
  auto ctx = input->GetContext();

  if (table_->column(0)->num_chunks() > 1) {
    const arrow::Result<std::shared_ptr<arrow::Table>> &res = table_->CombineChunks(cylon::ToArrowPool(ctx));
    RETURN_CYLON_STATUS_IF_ARROW_FAILED(res.status())
    table_ = res.ValueOrDie();
  }

  auto pool = cylon::ToArrowPool(ctx);

  std::shared_ptr<cylon::IndexKernel> kernel = CreateIndexKernel(table_, index_column);
  std::shared_ptr<cylon::BaseIndex> bi = kernel->BuildIndex(pool, table_, index_column);
  index = std::move(bi);
  return cylon::Status::OK();
}

cylon::Status cylon::IndexUtil::Find(std::shared_ptr<cylon::BaseIndex> &index,
                                     std::shared_ptr<cylon::Table> &find_table,
                                     void *value,
                                     int index_column,
                                     std::shared_ptr<cylon::Table> &out) {
  std::shared_ptr<arrow::Table> ar_out;
  auto table_ = find_table->get_table();
  auto ctx = find_table->GetContext();
  if (index != nullptr && index->GetColId() == index_column) {
    index->LocationByValue(value, table_, ar_out);
    cylon::Table::FromArrowTable(ctx, ar_out, out);
  } else {
    LOG(ERROR) << "Index column doesn't match the provided column";
  }
  return cylon::Status::OK();
}
cylon::Status cylon::IndexUtil::BuildFromVector(std::shared_ptr<arrow::Array> &index_values,
                                                arrow::MemoryPool *pool,
                                                std::shared_ptr<cylon::BaseIndex> &index) {
  Status s;
  switch (index_values->type()->id()) {

    case arrow::Type::NA:break;
    case arrow::Type::BOOL:break;
    case arrow::Type::UINT8:break;
    case arrow::Type::INT8:break;
    case arrow::Type::UINT16:break;
    case arrow::Type::INT16:break;
    case arrow::Type::UINT32:break;
    case arrow::Type::INT32: return cylon::IndexUtil::BuildIndexFromArrowArray<arrow::Int32Type>(index_values,
                                                                                                 pool,
                                                                                                 index);
    case arrow::Type::UINT64:break;
    case arrow::Type::INT64:break;
    case arrow::Type::HALF_FLOAT:break;
    case arrow::Type::FLOAT:break;
    case arrow::Type::DOUBLE:break;
    case arrow::Type::STRING:break;
    case arrow::Type::BINARY:break;
    case arrow::Type::FIXED_SIZE_BINARY:break;
    case arrow::Type::DATE32:break;
    case arrow::Type::DATE64:break;
    case arrow::Type::TIMESTAMP:break;
    case arrow::Type::TIME32:break;
    case arrow::Type::TIME64:break;
    case arrow::Type::INTERVAL_MONTHS:break;
    case arrow::Type::INTERVAL_DAY_TIME:break;
    case arrow::Type::DECIMAL:break;
    case arrow::Type::LIST:break;
    case arrow::Type::STRUCT:break;
    case arrow::Type::SPARSE_UNION:break;
    case arrow::Type::DENSE_UNION:break;
    case arrow::Type::DICTIONARY:break;
    case arrow::Type::MAP:break;
    case arrow::Type::EXTENSION:break;
    case arrow::Type::FIXED_SIZE_LIST:break;
    case arrow::Type::DURATION:break;
    case arrow::Type::LARGE_STRING:break;
    case arrow::Type::LARGE_BINARY:break;
    case arrow::Type::LARGE_LIST:break;
    case arrow::Type::MAX_ID:break;
  }

  return cylon::Status();
}











