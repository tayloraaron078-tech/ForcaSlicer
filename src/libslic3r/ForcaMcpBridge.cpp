#include "ForcaMcpBridge.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <sstream>
#include <string>

namespace Slic3r {

using json = nlohmann::json;

std::string forca_mcp_stdio_error(const std::string& request_line, const std::string& message)
{
    json request;
    try {
        request = json::parse(request_line);
    } catch (const std::exception&) {
        return {};
    }
    if (!request.is_object() || !request.contains("id") || request["id"].is_null())
        return {};
    return json{ { "jsonrpc", "2.0" }, { "id", request["id"] }, { "error", { { "code", -32000 }, { "message", message } } } }.dump();
}

std::string forca_mcp_stdio_output(const std::string& request_line, int status, const std::string& content_type, const std::string& body)
{
    if (status == 401)
        return forca_mcp_stdio_error(request_line, "Forca refused the Forca AI key. Copy the stdio setup again from "
                                                   "Forca's Forca AI window (the key may have been changed).");
    if (status < 200 || status >= 300)
        return forca_mcp_stdio_error(request_line, "Forca AI answered HTTP " + std::to_string(status) + (body.empty() ? "" : ": " + body));
    if (body.empty())
        return {}; // 202: a notification, nothing to answer

    // Server-sent events: each event's data is one message.
    if (content_type.find("text/event-stream") != std::string::npos) {
        std::istringstream in(body);
        std::string        line, data, out;
        auto flush = [&]() {
            if (data.empty())
                return;
            try {
                out += (out.empty() ? "" : "\n") + json::parse(data).dump();
            } catch (const std::exception&) {
            }
            data.clear();
        };
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                flush();
            else if (line.rfind("data:", 0) == 0)
                data += line.substr(line.size() > 5 && line[5] == ' ' ? 6 : 5);
        }
        flush();
        return out;
    }

    // One JSON message (or a batch), written on one line.
    try {
        return json::parse(body).dump();
    } catch (const std::exception&) {
        return forca_mcp_stdio_error(request_line, "Forca AI sent an answer that is not JSON.");
    }
}

} // namespace Slic3r
