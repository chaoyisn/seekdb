#pragma once
#include "sql/parser/parse_node.h"
namespace oceanbase { namespace sql {
struct ObResolverParams;
class AiColumnRetryResolver {
public:
  static int rewrite(ObResolverParams &params, const ParseNode &retry, ParseNode *&update);
};
} }
