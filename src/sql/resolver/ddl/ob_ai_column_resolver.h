#pragma once

#include "share/schema/ob_ai_column_schema.h"
#include "share/schema/ob_table_schema.h"
#include "sql/parser/parse_node.h"

namespace oceanbase {
namespace sql {
struct ObResolverParams;

class AiColumnResolver
{
public:
  static const ParseNode *find_definition(const ParseNode &column_node);
  static int resolve(ObResolverParams &params, const ParseNode &definition_node,
                     const share::schema::ObTableSchema &table,
                     const share::schema::ObColumnSchemaV2 &column,
                     const common::ObIArray<uint64_t> &ai_column_ids,
                     uint64_t &next_column_id, share::schema::AiColumnSchema &definition);
  static int make_hidden_columns(const share::schema::AiColumnSchema &definition,
                                 common::ObIArray<share::schema::ObColumnSchemaV2> &columns);
};

} // namespace sql
} // namespace oceanbase
