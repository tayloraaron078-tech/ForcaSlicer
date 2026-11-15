// forca-mcp: a stdio MCP server that relays to Forca AI's HTTP server (HQ PLAN_forca_ai.md P5, "stdio shim"), for AI
// apps that only start local programs, without Node.js / mcp-remote. Each JSON-RPC line read on stdin is posted to
// http://127.0.0.1:<port>/mcp with the Forca AI key; the answer is written on stdout as one line. Forca must be running
// with Forca AI on; every rule Forca AI applies (R1, R2, the control level) is Forca's, not this program's.
//
//   forca-mcp [--port N] [--key-file PATH]
//   key: the FORCA_AI_KEY environment variable, else --key-file, else forca-ai-key.txt in the user's local app data
//   folder (<user>\AppData\Local\ForcaSlicer), found without the LOCALAPPDATA variable
//   port: --port, else FORCA_AI_PORT, else 13630

#include "libslic3r/ForcaMcpBridge.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/dynamic_body.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/system/system_error.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#ifdef _WIN32
#include <objbase.h> // CoTaskMemFree
#include <shlobj.h>  // SHGetKnownFolderPath; after Boost.Asio, which must include winsock2.h first
#endif

namespace {

namespace beast = boost::beast;
namespace http  = beast::http;
using tcp       = boost::asio::ip::tcp;

std::string trim(std::string s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    const auto last  = s.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
}

// Where Forca keeps the key every Forca of this user shares (ForcaAI::shared_token_path).
std::filesystem::path default_key_file()
{
    std::filesystem::path dir;
#ifdef _WIN32
    // Asked of Windows, not read from LOCALAPPDATA: an AI app may start this program with a stripped environment.
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        dir = local;
    CoTaskMemFree(local);
#endif
    if (dir.empty())
        if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
            dir = local;
    return dir.empty() ? dir : dir / "ForcaSlicer" / "forca-ai-key.txt";
}

std::string read_key(const std::filesystem::path& path)
{
    if (const char* env = std::getenv("FORCA_AI_KEY"); env && *env)
        return trim(env);
    std::ifstream in(path, std::ios::binary);
    std::string   key((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return trim(key);
}

struct Reply
{
    int         status = 0;
    std::string content_type;
    std::string body;
    std::string session;
};

// One POST to Forca AI. Throws when Forca does not answer.
Reply post(const std::string& port, const std::string& key, const std::string& session, const std::string& body)
{
    boost::asio::io_context ioc;
    tcp::resolver           resolver(ioc);
    beast::tcp_stream       stream(ioc);
    stream.expires_after(std::chrono::minutes(10)); // a slice or a print request can take a while
    stream.connect(resolver.resolve("127.0.0.1", port));

    http::request<http::string_body> req{ http::verb::post, "/mcp", 11 };
    req.set(http::field::host, "127.0.0.1:" + port);
    req.set(http::field::authorization, "Bearer " + key);
    req.set(http::field::content_type, "application/json");
    req.set(http::field::accept, "application/json, text/event-stream");
    if (!session.empty())
        req.set("Mcp-Session-Id", session);
    req.body() = body;
    req.prepare_payload();
    http::write(stream, req);

    beast::flat_buffer                 buffer;
    http::response<http::dynamic_body> res;
    http::read(stream, buffer, res);
    Reply r;
    r.status       = int(res.result_int());
    r.content_type = std::string(res[http::field::content_type]);
    r.session      = std::string(res["Mcp-Session-Id"]);
    r.body         = beast::buffers_to_string(res.body().data());
    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    return r;
}

} // namespace

int main(int argc, char** argv)
{
    std::string port = "13630", key_file;
    if (const char* env = std::getenv("FORCA_AI_PORT"); env && *env)
        port = env;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc)
            port = argv[++i];
        else if (a == "--key-file" && i + 1 < argc)
            key_file = argv[++i];
        else {
            std::cerr << "usage: forca-mcp [--port N] [--key-file PATH]\n";
            return 2;
        }
    }
    const std::filesystem::path key_path = key_file.empty() ? default_key_file() : std::filesystem::path(key_file);
    const std::string           key      = read_key(key_path);
    if (key.empty())
        std::cerr << "forca-mcp: no Forca AI key: could not read " << key_path.u8string()
                  << " (turn Forca AI on once so it makes one, or set FORCA_AI_KEY; a sandboxed app may be blocked from "
                     "reading it)\n";

    std::ios::sync_with_stdio(false);
    std::string session, line;
    while (std::getline(std::cin, line)) {
        line = trim(line);
        if (line.empty())
            continue;
        std::string out;
        try {
            const Reply r = post(port, key, session, line);
            if (!r.session.empty())
                session = r.session;
            out = Slic3r::forca_mcp_stdio_output(line, r.status, r.content_type, r.body);
        } catch (const boost::system::system_error& e) {
            out = Slic3r::forca_mcp_stdio_error(line, "Forca is not answering on port " + port +
                                                          ": start Forca and turn Forca AI on (" + e.code().message() + ").");
        } catch (const std::exception& e) {
            out = Slic3r::forca_mcp_stdio_error(line, std::string("forca-mcp failed: ") + e.what());
        }
        if (!out.empty()) {
            std::cout << out << '\n';
            std::cout.flush();
        }
    }
    return 0;
}
