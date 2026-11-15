#pragma once

// Forca: the stdio side of forca-mcp (src/dev-utils/forca_mcp_stdio.cpp), the bridge for AI apps that only start local
// (stdio) MCP servers. Each line the app writes on stdin is one JSON-RPC message; forca-mcp posts it to Forca AI's
// HTTP server and writes Forca's answer back as one line. These helpers turn an HTTP reply into that line. They need
// only the standard library and nlohmann::json, so forca-mcp builds without the rest of Forca.

#include <string>

namespace Slic3r {

// The line to write on stdout for `request_line` after Forca answered with `status`, `content_type` and `body`;
// empty when there is nothing to write (a notification). An answer Forca could not give becomes a JSON-RPC error for
// the request's id.
std::string forca_mcp_stdio_output(const std::string& request_line, int status, const std::string& content_type, const std::string& body);

// A JSON-RPC error line answering `request_line` with `message`; empty for a notification or a line that is not a
// single JSON-RPC request.
std::string forca_mcp_stdio_error(const std::string& request_line, const std::string& message);

} // namespace Slic3r
