#include "doctest.h"
#include "platform/http_client.h"

REGISTER_TEST(http_build_request, "http: build Request", {
    platform::http::Request req;
    req.method = "GET";
    req.url = "https://example.com";
    DOCTEST_CHECK_EQ(req.method, std::string("GET"));
    DOCTEST_CHECK_EQ(req.url, std::string("https://example.com"));
    DOCTEST_CHECK_EQ(req.timeoutMs, 30000);
    DOCTEST_CHECK(req.verifyTls);
    return true;
})

REGISTER_TEST(http_build_response, "http: build Response", {
    platform::http::Response resp;
    resp.statusCode = 200;
    resp.body = "OK";
    DOCTEST_CHECK_EQ(resp.statusCode, 200);
    DOCTEST_CHECK_EQ(resp.body, std::string("OK"));
    DOCTEST_CHECK_EQ(resp.elapsedMs, 0);
    DOCTEST_CHECK_FALSE(resp.networkError);
    return true;
})

REGISTER_TEST(http_chunk_result_enum, "http: ChunkResult enum values", {
    DOCTEST_CHECK(platform::http::ChunkResult::More != platform::http::ChunkResult::End);
    DOCTEST_CHECK(platform::http::ChunkResult::More != platform::http::ChunkResult::Error);
    DOCTEST_CHECK(platform::http::ChunkResult::End != platform::http::ChunkResult::Error);
    return true;
})