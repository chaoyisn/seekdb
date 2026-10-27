#include <gtest/gtest.h>
#include "lib/allocator/page_arena.h"
#include "observer/virtual_table/ob_ai_column_metadata_table.h"

using namespace oceanbase::common;
using namespace oceanbase::share::schema;
using oceanbase::observer::AiColumnMetadataTable;

namespace {
void init_metadata_column(ObColumnSchemaV2 &column)
{
  column.set_column_id(17);
  ASSERT_EQ(OB_SUCCESS,column.set_column_name("tags"));
  AiColumnSchema definition;
  definition.set_column_id(17);
  definition.set_state_column_id(18);
  definition.set_request_column_id(19);
  definition.set_result_spec_column_id(20);
  definition.set_definition_version(42);
  definition.set_max_retries(3);
  ASSERT_EQ(OB_SUCCESS,definition.set_expression(ObString::make_string("AI_COMPLETE('m',content)")));
  ASSERT_EQ(OB_SUCCESS,definition.set_normalized_expr(ObString::make_string("Return exactly one valid JSON value")));
  ObSEArray<uint64_t,2> dependencies;
  ASSERT_EQ(OB_SUCCESS,dependencies.push_back(16));
  ASSERT_EQ(OB_SUCCESS,dependencies.push_back(21));
  ASSERT_EQ(OB_SUCCESS,definition.set_dependencies(dependencies));
  ASSERT_EQ(OB_SUCCESS,column.set_ai_column_schema(definition));
}

TEST(AiColumnMetadataProjection, ids_names_config_and_dependencies)
{
  ObColumnSchemaV2 column;
  init_metadata_column(column);
  ObArenaAllocator allocator;
  ObSEArray<uint64_t,12> projection;
  for (uint64_t id=AiColumnMetadataTable::TABLE_ID; id<=AiColumnMetadataTable::DEPENDENCY_COLUMN_IDS; ++id) {
    ASSERT_EQ(OB_SUCCESS,projection.push_back(id));
  }
  ObObj cells[12];
  ObNewRow row;
  row.cells_=cells; row.count_=12;
  ASSERT_EQ(OB_SUCCESS,AiColumnMetadataTable::project_column(500,column,projection,allocator,row));
  EXPECT_EQ(500,cells[0].get_int());
  EXPECT_EQ(17,cells[1].get_int());
  EXPECT_EQ(0,cells[2].get_string().compare("tags"));
  EXPECT_EQ(42,cells[3].get_int());
  EXPECT_EQ(0,cells[4].get_string().compare("AI_COMPLETE('m',content)"));
  EXPECT_EQ(0,cells[5].get_string().compare("Return exactly one valid JSON value"));
  EXPECT_EQ(0,cells[6].get_string().compare("async"));
  EXPECT_EQ(3,cells[7].get_int());
  EXPECT_EQ(18,cells[8].get_int());
  EXPECT_EQ(19,cells[9].get_int());
  EXPECT_EQ(20,cells[10].get_int());
  EXPECT_EQ(0,cells[11].get_string().compare("[16,21]"));
}

TEST(AiColumnMetadataProjection, arbitrary_order_and_repeated_columns)
{
  ObColumnSchemaV2 column;
  init_metadata_column(column);
  ObArenaAllocator allocator;
  ObSEArray<uint64_t,3> projection;
  ASSERT_EQ(OB_SUCCESS,projection.push_back(AiColumnMetadataTable::MAX_RETRIES));
  ASSERT_EQ(OB_SUCCESS,projection.push_back(AiColumnMetadataTable::COLUMN_NAME));
  ASSERT_EQ(OB_SUCCESS,projection.push_back(AiColumnMetadataTable::MAX_RETRIES));
  ObObj cells[3];
  ObNewRow row;
  row.cells_=cells; row.count_=3;
  ASSERT_EQ(OB_SUCCESS,AiColumnMetadataTable::project_column(500,column,projection,allocator,row));
  EXPECT_EQ(3,cells[0].get_int());
  EXPECT_EQ(0,cells[1].get_string().compare("tags"));
  EXPECT_EQ(3,cells[2].get_int());
}

TEST(AiColumnMetadataProjection, count_star_does_not_require_projected_cells)
{
  ObColumnSchemaV2 column;
  init_metadata_column(column);
  ObArenaAllocator allocator;
  ObSEArray<uint64_t,1> projection;
  ObNewRow row;
  row.cells_=nullptr; row.count_=0;
  EXPECT_EQ(OB_SUCCESS,AiColumnMetadataTable::project_column(500,column,projection,allocator,row));
}

TEST(AiColumnMetadataProjection, ordinary_column_is_not_a_definition)
{
  ObColumnSchemaV2 column;
  ObArenaAllocator allocator;
  ObSEArray<uint64_t,1> projection;
  ObNewRow row;
  row.cells_=nullptr; row.count_=0;
  EXPECT_EQ(OB_INVALID_ARGUMENT,AiColumnMetadataTable::project_column(500,column,projection,allocator,row));
}

TEST(AiColumnMetadataProjection, invalid_projection_or_iterator_state_rejected)
{
  ObColumnSchemaV2 column;
  init_metadata_column(column);
  ObArenaAllocator allocator;
  ObSEArray<uint64_t,1> projection;
  ASSERT_EQ(OB_SUCCESS,projection.push_back(99999));
  ObObj cell;
  ObNewRow row;
  row.cells_=&cell; row.count_=1;
  EXPECT_EQ(OB_ERR_UNEXPECTED,AiColumnMetadataTable::project_column(500,column,projection,allocator,row));
  row.count_=0;
  EXPECT_EQ(OB_INVALID_ARGUMENT,AiColumnMetadataTable::project_column(500,column,projection,allocator,row));
  AiColumnMetadataTable table;
  EXPECT_EQ(OB_NOT_INIT,table.inner_open());
  ObNewRow *output=nullptr;
  EXPECT_EQ(OB_NOT_INIT,table.inner_get_next_row(output));
}
} // namespace
