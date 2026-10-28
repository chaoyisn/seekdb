#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include "lib/allocator/page_arena.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_client.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

TEST(AiColumnTransport, durable_attempt_is_single_and_restores_ordinary_retries)
{
  const char *port = std::getenv("AI_COLUMN_TRANSPORT_TEST_PORT");
  ASSERT_NE(nullptr, port) << "Run the supplemental HTTP fault-service wrapper";
  const std::string base = std::string("http://127.0.0.1:") + port;
  auto call = [&](const char *path) {
    ObArenaAllocator allocator("AiRetryTest");
    ObString json = ObString::make_string("{}");
    ObJsonObject *body = nullptr;
    const int parsed = ObAIFuncJsonUtils::get_json_object_form_str(allocator,json,body);
    if (parsed != OB_SUCCESS) { return parsed; }
    ObArray<ObString> headers;
    const int pushed = headers.push_back(ObString::make_string("Content-Type: application/json"));
    if (pushed != OB_SUCCESS) { return pushed; }
    const std::string url = base + path;
    ObAIFuncClient client;
    ObJsonObject *response = nullptr;
    return client.send_post(allocator,ObString::make_string(url.c_str()),headers,body,response);
  };
  {
    ObAIFuncClient::AiColumnAttemptGuard guard;
    EXPECT_EQ(OB_CURL_ERROR,call("/guarded"));
  }
  EXPECT_EQ(OB_SUCCESS,call("/normal"));
  {
    ObAIFuncClient::AiColumnAttemptGuard outer;
    {
      ObAIFuncClient::AiColumnAttemptGuard inner;
      EXPECT_EQ(OB_CURL_ERROR,call("/nested_a"));
    }
    EXPECT_EQ(OB_CURL_ERROR,call("/nested_b"));
  }
  EXPECT_EQ(OB_SUCCESS,call("/after_scope"));
}
