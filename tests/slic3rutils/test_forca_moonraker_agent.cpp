// Forca: the Moonraker printer agent's failure log line (src/slic3r/Utils/MoonrakerPrinterAgent.cpp); no network.
// Same Windows include prologue as test_forca_brand.cpp (wx pulls in <windows.h>).
#include "catch2/matchers/catch_matchers_string.hpp"
#include "catch2/catch_test_macros.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_all.hpp>

#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"

#include <string>

using namespace Slic3r;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("A failed Moonraker request is logged with its URL, the answering server and the reply", "[ForcaMoonrakerAgent]")
{
    const std::string headers = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nServer: Python/3.11 aiohttp/3.14.3\r\n\r\n";
    const std::string line    = moonraker_describe_failed_request("http://192.168.1.12:7125/server/info", " (HTTP 404)", headers,
                                                                  "404: Not Found", true);

    CHECK_THAT(line, ContainsSubstring("(HTTP 404)"));
    CHECK_THAT(line, ContainsSubstring("url=http://192.168.1.12:7125/server/info"));
    CHECK_THAT(line, ContainsSubstring("server=Python/3.11 aiohttp/3.14.3"));
    CHECK_THAT(line, ContainsSubstring("api_key_header=yes"));
    CHECK_THAT(line, ContainsSubstring("reply=404: Not Found"));
}

TEST_CASE("A failed Moonraker request without headers or reply says so", "[ForcaMoonrakerAgent]")
{
    const std::string line = moonraker_describe_failed_request("http://printer:7125/server/info", "Connection failed", "", "", false);

    CHECK_THAT(line, ContainsSubstring("server=-"));
    CHECK_THAT(line, ContainsSubstring("api_key_header=no"));
    CHECK_THAT(line, ContainsSubstring("reply=-"));
}

TEST_CASE("The logged Moonraker reply is one line of at most 200 characters", "[ForcaMoonrakerAgent]")
{
    const std::string body = "first line\r\nsecond line\n" + std::string(500, 'x');
    const std::string line = moonraker_describe_failed_request("http://printer/x", "HTTP error: 500", "", body, false);

    const auto reply_at = line.find("reply=");
    REQUIRE(reply_at != std::string::npos);
    const std::string reply = line.substr(reply_at + 6, line.size() - reply_at - 7); // without the closing ']'
    CHECK(reply.size() == 200);
    CHECK(reply.find('\n') == std::string::npos);
    CHECK(reply.find('\r') == std::string::npos);
}

TEST_CASE("The last Server header wins after a redirect", "[ForcaMoonrakerAgent]")
{
    const std::string headers = "HTTP/1.1 301 Moved\r\nServer: nginx\r\n\r\nHTTP/1.1 404 Not Found\r\nserver: aiohttp\r\n\r\n";
    const std::string line    = moonraker_describe_failed_request("http://printer/x", "(HTTP 404)", headers, "", false);

    CHECK_THAT(line, ContainsSubstring("server=aiohttp"));
}

TEST_CASE("The printer address the agent reports keeps the port", "[ForcaMoonrakerAgent]")
{
    // A KX-Bridge on port 7125: reporting the bare host sent every later request to port 80.
    CHECK(moonraker_device_address("192.168.1.12", "7125") == "192.168.1.12:7125");
    CHECK(moonraker_device_address("192.168.1.12", "") == "192.168.1.12");
    CHECK(moonraker_device_address("192.168.1.12:7125", "7125") == "192.168.1.12:7125");
    CHECK(moonraker_device_address("[fe80::1]", "7125") == "[fe80::1]:7125");
}
