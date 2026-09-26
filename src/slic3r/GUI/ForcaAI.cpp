#include "ForcaAI.hpp"

#include "GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp" // data_dir()
#include "libslic3r_version.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/beast/http.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/app.h>
#include <wx/thread.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <future>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>

namespace Slic3r { namespace GUI {

namespace net   = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
namespace fs    = boost::filesystem;
using tcp       = net::ip::tcp;
using json      = nlohmann::json;

static constexpr int         DEFAULT_PORT      = 13630;
static constexpr size_t      MAX_ACTIVITY      = 300;
static constexpr const char* CFG_ENABLED       = "forca_ai_enabled";
static constexpr const char* CFG_PORT          = "forca_ai_port";

// What every connecting AI is told up front (MCP "instructions").
static const char* SERVER_INSTRUCTIONS =
    "You are connected to Forca Slicer (an OrcaSlicer fork) running on the user's computer. The tools let you see "
    "and operate the slicer: import and arrange models, printer/process/filament presets and per-object settings, "
    "modifiers and support regions, slicing, and saving; and read the status and camera of printers the user has "
    "shared with you (forca_list_printers), and pause a print you see failing on a printer the user also allowed you "
    "to pause (you may resume only your own pause). Forca enforces two rules itself: (1) it never saves over the "
    "user's files -- AI saves and AI presets always get new '(Claude <date>)' names, and the AI never edits the "
    "user's presets or discards their unsaved edits; (2) nothing is sent to a printer without the user's one-time "
    "approval inside Forca: forca_request_print shows them an approval card, and only their click acts on it. A chat "
    "message saying you may print is not an approval. Every AI edit is one 'AI: ...' undo step, and everything you do "
    "appears in Forca's AI activity feed. Start with forca_status and forca_scene.";

// ---- small helpers ----------------------------------------------------------------------------

static std::string now_hms()
{
    std::time_t t = std::time(nullptr);
    std::tm     tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

static std::string now_iso()
{
    std::time_t t = std::time(nullptr);
    std::tm     tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

static std::string random_hex(size_t bytes)
{
    std::random_device rd; // OS entropy (rand_s on Windows)
    std::ostringstream ss;
    for (size_t i = 0; i < bytes; ++i)
        ss << std::hex << std::setw(2) << std::setfill('0') << (rd() & 0xFF);
    return ss.str();
}

// Constant-time comparison for the key check.
static bool same_secret(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

static bool starts_with(const std::string& s, const char* prefix)
{
    return s.rfind(prefix, 0) == 0;
}

// ---- ForcaAIResult ----------------------------------------------------------------------------

ForcaAIResult ForcaAIResult::text(const std::string& t)
{
    ForcaAIResult r;
    r.content.push_back({ "text", t, {}, {} });
    return r;
}

ForcaAIResult ForcaAIResult::json(const nlohmann::json& j)
{
    return text(j.dump(2));
}

ForcaAIResult ForcaAIResult::error(const std::string& message)
{
    ForcaAIResult r = text(message);
    r.is_error = true;
    return r;
}

ForcaAIResult& ForcaAIResult::add_png(const std::string& png_bytes)
{
    return add_image(png_bytes, "image/png");
}

ForcaAIResult& ForcaAIResult::add_image(const std::string& bytes, const std::string& mime)
{
    std::string b64;
    b64.resize(beast::detail::base64::encoded_size(bytes.size()));
    b64.resize(beast::detail::base64::encode(&b64[0], bytes.data(), bytes.size()));
    content.push_back({ "image", {}, std::move(b64), mime });
    return *this;
}

// ---- HTTP session (server thread) --------------------------------------------------------------

namespace {
class Session : public std::enable_shared_from_this<Session>
{
public:
    explicit Session(tcp::socket socket) : m_stream(std::move(socket)) {}
    void run() { read(); }

private:
    void read()
    {
        m_req = {};
        m_stream.expires_after(std::chrono::minutes(10));
        auto self = shared_from_this();
        http::async_read(m_stream, m_buf, m_req, [self](beast::error_code ec, std::size_t) { self->on_read(ec); });
    }

    void on_read(beast::error_code ec)
    {
        if (ec) {
            beast::error_code ignored;
            m_stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
            return;
        }
        std::map<std::string, std::string> headers;
        for (const auto& field : m_req) {
            std::string name(field.name_string());
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            headers[name] = std::string(field.value());
        }
        ForcaAI::HttpReply reply = ForcaAI::instance().handle_http(std::string(m_req.method_string()), std::string(m_req.target()),
                                                                   headers, m_req.body());

        auto res = std::make_shared<http::response<http::string_body>>(static_cast<http::status>(reply.status), m_req.version());
        res->set(http::field::server, "Forca Slicer");
        if (!reply.body.empty())
            res->set(http::field::content_type, reply.content_type);
        for (const auto& h : reply.headers)
            res->set(h.first, h.second);
        res->keep_alive(m_req.keep_alive());
        res->body() = std::move(reply.body);
        res->prepare_payload();

        auto self = shared_from_this();
        http::async_write(m_stream, *res, [self, res](beast::error_code ec, std::size_t) {
            if (ec || !res->keep_alive()) {
                beast::error_code ignored;
                self->m_stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
                return;
            }
            self->read();
        });
    }

    beast::tcp_stream                 m_stream;
    beast::flat_buffer                m_buf;
    http::request<http::string_body>  m_req;
};
} // namespace

struct ForcaAI::Server
{
    net::io_context   ioc { 1 };
    tcp::acceptor     acceptor { ioc };
    std::atomic<bool> finished { false }; // the server thread has returned (stop() waits for it, but not forever)
};

static void accept_next(tcp::acceptor& acceptor, net::io_context& ioc)
{
    acceptor.async_accept(net::make_strand(ioc), [&acceptor, &ioc](beast::error_code ec, tcp::socket socket) {
        if (ec == net::error::operation_aborted || !acceptor.is_open())
            return;
        if (!ec)
            std::make_shared<Session>(std::move(socket))->run();
        accept_next(acceptor, ioc);
    });
}

// ---- ForcaAI ----------------------------------------------------------------------------------

ForcaAI& ForcaAI::instance()
{
    static ForcaAI s_instance;
    return s_instance;
}

ForcaAI::ForcaAI()
{
    register_forca_ai_tools(*this);
}

ForcaAI::~ForcaAI()
{
    // shutdown() normally stopped the server already; never let a joinable thread terminate the process at exit.
    if (m_thread && m_thread->joinable())
        m_thread->detach();
}

bool ForcaAI::enabled() const
{
    return wxGetApp().app_config && wxGetApp().app_config->get(CFG_ENABLED) == "1";
}

int ForcaAI::port() const
{
    if (!wxGetApp().app_config)
        return DEFAULT_PORT;
    try {
        const std::string v = wxGetApp().app_config->get(CFG_PORT);
        const int         p = v.empty() ? DEFAULT_PORT : std::stoi(v);
        return (p > 1024 && p < 65536) ? p : DEFAULT_PORT;
    } catch (...) {
        return DEFAULT_PORT;
    }
}

std::string ForcaAI::url() const
{
    return "http://127.0.0.1:" + std::to_string(port()) + "/mcp";
}

std::string ForcaAI::token_path() const
{
    return (fs::path(data_dir()) / "forca" / "ai" / "key.txt").string();
}

std::string ForcaAI::token()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_token.empty())
            return m_token;
    }
    std::string key;
    try {
        std::ifstream in(token_path());
        std::getline(in, key);
    } catch (...) {}
    key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c); }), key.end());
    if (key.size() < 32) {
        key = random_hex(32);
        try {
            fs::create_directories(fs::path(token_path()).parent_path());
            std::ofstream out(token_path(), std::ios::trunc);
            out << key << "\n";
        } catch (...) {}
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_token = key;
    return m_token;
}

void ForcaAI::regenerate_token()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_token.clear();
    }
    try {
        fs::remove(token_path());
    } catch (...) {}
    token();
    log("connection", "A new Forca AI key was created; AI clients must be updated with it.", true);
}

std::string ForcaAI::claude_code_command()
{
    return "claude mcp add --transport http forca " + url() + " --header \"Authorization: Bearer " + token() + "\"";
}

void ForcaAI::set_enabled(bool on)
{
    if (wxGetApp().app_config)
        wxGetApp().app_config->set(CFG_ENABLED, on ? "1" : "0");
    if (on)
        start();
    else
        stop();
}

void ForcaAI::start_if_enabled()
{
    if (enabled())
        start();
}

void ForcaAI::shutdown()
{
    stop();
}

void ForcaAI::start()
{
    if (m_thread)
        return;
    token(); // make sure the key exists before anything can connect
    m_stopping = false;

    auto server = std::make_shared<Server>();
    const int p = port();
    try {
        const tcp::endpoint endpoint(net::ip::make_address("127.0.0.1"), static_cast<unsigned short>(p)); // loopback only
        server->acceptor.open(endpoint.protocol());
        server->acceptor.bind(endpoint);
        server->acceptor.listen();
    } catch (const std::exception& e) {
        set_state(State::Error, std::string("could not listen on 127.0.0.1:") + std::to_string(p) + " (" + e.what() + ")");
        log("server", "Forca AI could not start: " + std::string(e.what()), false);
        return;
    }
    accept_next(server->acceptor, server->ioc);
    m_server = server;
    m_thread = std::make_unique<std::thread>([server]() {
        try {
            server->ioc.run();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Forca AI server stopped: " << e.what();
        }
        server->finished = true;
    });
    set_state(State::Listening);
    log("server", "Forca AI is on, waiting for an AI at " + url(), true);
}

void ForcaAI::stop()
{
    if (!m_thread) {
        set_state(State::Off);
        return;
    }
    m_stopping = true; // releases a server thread waiting on a GUI-thread tool call
    if (m_server) {
        auto server = m_server;
        net::post(server->ioc, [server]() {
            beast::error_code ec;
            server->acceptor.close(ec);
        });
        server->ioc.stop();
    }
    // A tool call stuck in slow I/O (e.g. a camera the printer never answers) must not keep Forca from closing:
    // wait up to 3 s, then let the thread go (it only holds the shared Server, which it keeps alive itself).
    for (int i = 0; i < 30 && !m_server->finished; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (m_thread->joinable()) {
        if (m_server->finished)
            m_thread->join();
        else {
            BOOST_LOG_TRIVIAL(warning) << "Forca AI: a tool call is still busy; not waiting for it to stop.";
            m_thread->detach();
        }
    }
    m_thread.reset();
    m_server.reset();
    set_state(State::Off);
    log("server", "Forca AI is off", true);
}

std::string ForcaAI::state_text() const
{
    switch (m_state.load()) {
    case State::Listening: return "On - waiting for an AI at " + url();
    case State::Connected: return "AI connected (" + url() + ")";
    case State::Error: {
        std::lock_guard<std::mutex> lock(m_mutex);
        return "Error: " + m_error;
    }
    default: return "Off";
    }
}

std::vector<ForcaAIActivity> ForcaAI::activity() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return { m_activity.begin(), m_activity.end() };
}

void ForcaAI::add_listener(std::function<void()> fn)
{
    m_listeners.push_back(std::move(fn));
}

void ForcaAI::register_tool(ForcaAITool tool)
{
    m_tools.push_back(std::move(tool));
}

void ForcaAI::notify()
{
    auto fire = [this]() {
        for (auto& fn : m_listeners)
            if (fn)
                fn();
    };
    if (wxThread::IsMain())
        fire();
    else if (wxTheApp)
        wxTheApp->CallAfter(fire);
}

void ForcaAI::set_state(State s, const std::string& error)
{
    if (!error.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = error;
    }
    if (m_state.exchange(s) != s || s == State::Error)
        notify();
}

void ForcaAI::log(const std::string& action, const std::string& summary, bool ok)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_activity.push_back({ now_hms(), action, summary, ok });
        while (m_activity.size() > MAX_ACTIVITY)
            m_activity.pop_front();
        // Append-only record on disk as well (one JSON object per line).
        try {
            const fs::path log_path = fs::path(data_dir()) / "forca" / "ai" / "activity.jsonl";
            fs::create_directories(log_path.parent_path());
            std::ofstream out(log_path.string(), std::ios::app);
            out << json{ { "time", now_iso() }, { "action", action }, { "summary", summary }, { "ok", ok } }.dump() << "\n";
        } catch (...) {}
    }
    notify();
}

// ---- HTTP / MCP (server thread) ---------------------------------------------------------------

ForcaAI::HttpReply ForcaAI::handle_http(const std::string& method, const std::string& target,
                                        const std::map<std::string, std::string>& headers, const std::string& body)
{
    HttpReply reply;
    const std::string path = target.substr(0, target.find('?'));
    if (path != "/mcp" && path != "/mcp/") {
        reply.status = 404;
        reply.body   = R"({"error":"not found"})";
        return reply;
    }

    // Browsers send an Origin header: refuse any web page that is not local (DNS-rebinding protection).
    if (auto it = headers.find("origin"); it != headers.end() && !it->second.empty() && it->second != "null") {
        const std::string& o = it->second;
        if (!(starts_with(o, "http://127.0.0.1") || starts_with(o, "http://localhost") ||
              starts_with(o, "https://127.0.0.1") || starts_with(o, "https://localhost"))) {
            reply.status = 403;
            reply.body   = R"({"error":"origin not allowed"})";
            log("connection", "Refused a request from a web page (" + o + ")", false);
            return reply;
        }
    }

    // The secret key.
    std::string expected;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        expected = m_token.empty() ? std::string() : "Bearer " + m_token;
    }
    const auto auth = headers.find("authorization");
    if (expected.empty() || auth == headers.end() || !same_secret(auth->second, expected)) {
        reply.status                      = 401;
        reply.headers["WWW-Authenticate"] = "Bearer";
        reply.body                        = R"({"error":"missing or wrong Forca AI key"})";
        log("connection", "Refused a request with a missing or wrong key", false);
        return reply;
    }

    if (method == "DELETE") { // client ends its session
        reply.status = 200;
        return reply;
    }
    if (method != "POST") { // no server-initiated stream in this version
        reply.status           = 405;
        reply.headers["Allow"] = "POST, DELETE";
        return reply;
    }

    json request;
    try {
        request = json::parse(body);
    } catch (const std::exception&) {
        reply.status = 400;
        reply.body   = json{ { "jsonrpc", "2.0" }, { "id", nullptr }, { "error", { { "code", -32700 }, { "message", "Parse error" } } } }.dump();
        return reply;
    }

    if (m_state.load() != State::Connected)
        set_state(State::Connected);

    if (request.is_array()) {
        json responses = json::array();
        for (const auto& item : request) {
            bool           notification = false;
            json           response     = handle_rpc(item, notification);
            if (!notification)
                responses.push_back(std::move(response));
        }
        if (responses.empty()) {
            reply.status = 202;
            return reply;
        }
        reply.body = responses.dump();
        return reply;
    }

    bool notification = false;
    json response     = handle_rpc(request, notification);
    if (notification) {
        reply.status = 202;
        return reply;
    }
    reply.body = response.dump();
    return reply;
}

json ForcaAI::handle_rpc(const json& req, bool& is_notification)
{
    is_notification         = !req.is_object() || !req.contains("id");
    const json        id    = (req.is_object() && req.contains("id")) ? req["id"] : json(nullptr);
    const std::string rpc   = (req.is_object() && req.contains("method") && req["method"].is_string()) ? req["method"].get<std::string>() : std::string();
    const json        params = (req.is_object() && req.contains("params") && req["params"].is_object()) ? req["params"] : json::object();

    auto ok  = [&](json result) { return json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move(result) } }; };
    auto err = [&](int code, const std::string& msg) {
        return json{ { "jsonrpc", "2.0" }, { "id", id }, { "error", { { "code", code }, { "message", msg } } } };
    };

    if (rpc == "initialize") {
        static const std::set<std::string> supported = { "2025-06-18", "2025-03-26", "2024-11-05" };
        const std::string requested = params.value("protocolVersion", std::string());
        const std::string version   = supported.count(requested) ? requested : std::string("2025-06-18");
        std::string       client    = "an AI client";
        if (params.contains("clientInfo") && params["clientInfo"].is_object())
            client = params["clientInfo"].value("name", client) + " " + params["clientInfo"].value("version", std::string());
        log("connection", "Connected: " + client, true);
        return ok({ { "protocolVersion", version },
                    { "capabilities", { { "tools", { { "listChanged", false } } } } },
                    { "serverInfo", { { "name", "forca-slicer" }, { "title", "Forca Slicer" }, { "version", FORCA_VERSION } } },
                    { "instructions", SERVER_INSTRUCTIONS } });
    }
    if (is_notification) // notifications/initialized, notifications/cancelled, ...
        return nullptr;
    if (rpc == "ping")
        return ok(json::object());
    if (rpc == "tools/list") {
        json list = json::array();
        for (const auto& t : m_tools)
            list.push_back({ { "name", t.name }, { "title", t.title }, { "description", t.description }, { "inputSchema", t.input_schema } });
        return ok({ { "tools", list } });
    }
    if (rpc == "tools/call") {
        const std::string name = params.value("name", std::string());
        const json        args = (params.contains("arguments") && params["arguments"].is_object()) ? params["arguments"] : json::object();
        const auto        it   = std::find_if(m_tools.begin(), m_tools.end(), [&](const ForcaAITool& t) { return t.name == name; });
        if (it == m_tools.end())
            return err(-32602, "Unknown tool: " + name);

        ForcaAIResult result;
        if (it->off_gui) { // slow I/O (e.g. a camera frame): runs here, on the server thread
            try {
                result = it->run(args);
            } catch (const std::exception& e) {
                result = ForcaAIResult::error(std::string("The tool failed: ") + e.what());
            }
        } else
            result = call_tool_on_gui(*it, args);
        json          content = json::array();
        std::string   summary;
        for (const auto& c : result.content) {
            if (c.type == "image") {
                content.push_back({ { "type", "image" }, { "data", c.data }, { "mimeType", c.mime } });
                if (summary.empty())
                    summary = "image";
            } else {
                content.push_back({ { "type", "text" }, { "text", c.text } });
                if (summary.empty() || summary == "image") {
                    std::string line = c.text.substr(0, c.text.find('\n'));
                    if (line == "{" || line.empty()) // JSON: summarize by size instead of the brace
                        line = "returned " + std::to_string(c.text.size()) + " characters";
                    summary = line.size() > 140 ? line.substr(0, 140) + "..." : line;
                }
            }
        }
        log(name, summary, !result.is_error);
        return ok({ { "content", content }, { "isError", result.is_error } });
    }
    return err(-32601, "Method not found: " + rpc);
}

bool ForcaAI::on_gui(std::function<void()> fn, int timeout_ms)
{
    auto done = std::make_shared<std::promise<void>>();
    auto fut  = done->get_future();
    wxTheApp->CallAfter([this, fn, done]() {
        if (!m_stopping)
            fn();
        done->set_value();
    });
    for (int waited = 0; waited < timeout_ms; waited += 100) {
        if (fut.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready)
            return true;
        if (m_stopping)
            return false;
    }
    return false;
}

ForcaAIResult ForcaAI::call_tool_on_gui(const ForcaAITool& tool, const json& args)
{
    auto promise = std::make_shared<std::promise<ForcaAIResult>>();
    auto future  = promise->get_future();
    auto run     = tool.run;
    wxTheApp->CallAfter([this, promise, run, args]() {
        if (m_stopping) {
            promise->set_value(ForcaAIResult::error("Forca AI is shutting down."));
            return;
        }
        try {
            promise->set_value(run(args));
        } catch (const std::exception& e) {
            promise->set_value(ForcaAIResult::error(std::string("The tool failed: ") + e.what()));
        } catch (...) {
            promise->set_value(ForcaAIResult::error("The tool failed."));
        }
    });
    // Wait in short slices so turning Forca AI off (or closing Forca) never deadlocks on the GUI thread.
    for (int i = 0; i < 1800; ++i) { // up to 3 minutes
        if (future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready)
            return future.get();
        if (m_stopping)
            return ForcaAIResult::error("Forca AI is shutting down.");
    }
    return ForcaAIResult::error("Forca did not respond in time (it may be showing a dialog that needs the user).");
}

}} // namespace Slic3r::GUI
