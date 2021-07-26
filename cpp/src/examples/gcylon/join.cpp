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

#include <cudf/table/table.hpp>
#include <cudf/table/table_view.hpp>

#include <examples/gcylon/construct.hpp>
#include <examples/gcylon/print.hpp>

#include <gcylon/gtable.hpp>
#include <cylon/net/mpi/mpi_communicator.hpp>

using std::cout;
using std::endl;
using std::string;
using namespace gcylon;

int main(int argc, char *argv[]) {

    const int COLS = 2;
    const int64_t ROWS = 10;

    auto mpi_config = std::make_shared<cylon::net::MPIConfig>();
    auto ctx = cylon::CylonContext::InitDistributed(mpi_config);
    int myRank = ctx->GetRank();

    cout << "myRank: "  << myRank << ", world size: " << ctx->GetWorldSize() << endl;

    int numberOfGPUs;
    cudaGetDeviceCount(&numberOfGPUs);
    cout << "myRank: "  << myRank << ", number of GPUs: " << numberOfGPUs << endl;

    // set the gpu
    cudaSetDevice(myRank % numberOfGPUs);

    // construct table1
    int64_t start = myRank * 100;
    std::shared_ptr<cudf::table> tbl1 = constructTable(COLS, ROWS, start, true);
    auto tv1 = tbl1->view();
    cout << "myRank: "  << myRank << ", initial dataframe. cols: "<< tv1.num_columns() << ", rows: " << tv1.num_rows() << endl;
    cout << "myRank: "  << myRank << ", initial dataframe................................. " << endl;
    printWholeTable(tv1);

    // construct table1
    std::shared_ptr<cudf::table> tbl2 = constructTable(COLS, ROWS, start + 5, true);
    auto tv2 = tbl2->view();
    cout << "myRank: "  << myRank << ", initial dataframe. cols: "<< tv2.num_columns() << ", rows: " << tv2.num_rows() << endl;
    cout << "myRank: "  << myRank << ", initial dataframe................................. " << endl;
    printWholeTable(tv2);

    // join the tables
    std::unique_ptr<cudf::table> joinedTable;
    auto join_config = cylon::join::config::JoinConfig(cylon::join::config::JoinType::INNER,
                                                       0,
                                                       0,
                                                       cylon::join::config::JoinAlgorithm::HASH);
    cylon::Status status = DistributedJoin(tv1, tv2, join_config, ctx, joinedTable);
    if (!status.is_ok()) {
        cout << "Joining tables failed." << endl;
        ctx->Finalize();
        return 1;
    }
    auto tvj = joinedTable->view();

    if (tvj.num_rows() == 0) {
        cout << myRank << ": joined table is empty" << endl;
    } else {
        cout << myRank << ", joined table. number of columns: " << tvj.num_columns() << ", number of rows: " << tvj.num_rows() << endl;
        printWholeTable(tvj);
    }

    ctx->Finalize();
    return 0;
}
