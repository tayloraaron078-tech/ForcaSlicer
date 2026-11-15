// Forca: forca-mcp's stdio side (ForcaMcpBridge): an HTTP reply from Forca AI turned into one stdout line.
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ForcaMcpBridge.hpp"

#include <nlohmann/json.hpp>

#include <string>

using namespace Slic3r;
using json = nlohmann::json;

namespace {

const std::string REQUEST      = R"({"jsonrpc":"2.0","id":7,"method":"tools/list"})";
const std::string NOTIFICATION = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";

} // namespace

TEST_CASE("A JSON answer is written as one line", "[ForcaMcpBridge]")
{
    const std::string out = forca_mcp_stdio_output(REQUEST, 200, "application/json", "{\n  \"jsonrpc\": \"2.0\",\n  \"id\": 7,\n  \"result\": {}\n}");
    CHECK(out.find('\n') == std::string::npos);
    CHECK(json::parse(out)["id"] == 7);
}

TEST_CASE("A notification's empty 202 writes nothing", "[ForcaMcpBridge]")
{
    CHECK(forca_mcp_stdio_output(NOTIFICATION, 202, "", "").empty());
}

TEST_CASE("Server-sent events give one line per event", "[ForcaMcpBridge]")
{
    const std::string body = "event: message\r\ndata: {\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{}}\r\n\r\n"
                             "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}\n\n";
    const std::string out = forca_mcp_stdio_output(REQUEST, 200, "text/event-stream", body);
    const auto        nl  = out.find('\n');
    REQUIRE(nl != std::string::npos);
    CHECK(json::parse(out.substr(0, nl))["id"] == 7);
    CHECK(json::parse(out.substr(nl + 1))["method"] == "notifications/progress");
}

TEST_CASE("A refused key or a failed answer becomes a JSON-RPC error for the request's id", "[ForcaMcpBridge]")
{
    const json refused = json::parse(forca_mcp_stdio_output(REQUEST, 401, "application/json", "{}"));
    CHECK(refused["id"] == 7);
    CHECK(refused["error"]["code"] == -32000);
    CHECK(refused["error"]["message"].get<std::string>().find("key") != std::string::npos);

    CHECK(json::parse(forca_mcp_stdio_output(REQUEST, 500, "", "boom"))["error"]["message"].get<std::string>().find("500") !=
          std::string::npos);
    // A notification gets no error line: nobody waits for its answer.
    CHECK(forca_mcp_stdio_output(NOTIFICATION, 500, "", "boom").empty());
    CHECK(forca_mcp_stdio_error("not json", "x").empty());
}
