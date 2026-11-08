#include "ForcaHostStatus.hpp"

#include "GUI_App.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/log/trivial.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/beast/websocket/stream_base.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <libslic3r/Config.hpp>
#include <utility>
#include <exception>
#include <vector>
#include <libslic3r/Preset.hpp>

namespace Slic3r { namespace GUI {

using json = nlohmann::json;

namespace {

constexpr long TIMEOUT_CONNECT_S = 5;
constexpr long TIMEOUT_MAX_S     = 15;
constexpr long TIMEOUT_PAUSE_S   = 120;

std::string str(const json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

double num(const json& j, const char* key, double fallback = -1)
{
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : fallback;
}

const json& obj(const json& j, const char* key)
{
    static const json empty = json::object();
    auto it = j.find(key);
    return it != j.end() && it->is_object() ? *it : empty;
}

PrintHostType host_type(const DynamicPrintConfig& config)
{
    const auto* opt = config.option<ConfigOptionEnum<PrintHostType>>("host_type");
    return opt ? opt->value : htOctoPrint;
}

// "http://host[:port]" from a print_host as the user typed it (no path, no trailing slash).
std::string base_url(const std::string& host)
{
    std::string url = boost::algorithm::trim_copy(host);
    if (!boost::algorithm::istarts_with(url, "http://") && !boost::algorithm::istarts_with(url, "https://"))
        url = "http://" + url;
    const size_t path = url.find('/', url.find("//") + 2);
    if (path != std::string::npos)
        url.erase(path);
    return url;
}

// The bare host name or IP (no scheme, port or path).
std::string host_name(const std::string& host)
{
    std::string h = base_url(host);
    h.erase(0, h.find("//") + 2);
    if (const size_t at = h.rfind('@'); at != std::string::npos)
        h.erase(0, at + 1);
    if (const size_t colon = h.find(':'); colon != std::string::npos)
        h.erase(colon);
    return h;
}

std::string job_stem(std::string file)
{
    if (const size_t slash = file.find_last_of("/\\"); slash != std::string::npos)
        file.erase(0, slash + 1);
    boost::algorithm::to_lower(file);
    for (const char* ext : { ".gcode.3mf", ".gcode", ".3mf", ".bgcode" })
        if (boost::algorithm::ends_with(file, ext)) {
            file.erase(file.size() - std::strlen(ext));
            break;
        }
    return file;
}

bool http_get(const std::string& url, const std::string& api_key, std::string& body, std::string& err, size_t max_bytes = 0)
{
    bool ok   = false;
    auto http = Http::get(url);
    http.timeout_connect(TIMEOUT_CONNECT_S).timeout_max(TIMEOUT_MAX_S);
    if (max_bytes > 0)
        http.size_limit(max_bytes);
    if (!api_key.empty())
        http.header("X-Api-Key", api_key);
    http.on_complete([&](std::string b, unsigned) {
            body = std::move(b);
            ok   = true;
        })
        .on_error([&](std::string, std::string error, unsigned status) { err = error + " (HTTP " + std::to_string(status) + ")"; })
        .perform_sync();
    return ok;
}

// One still picture from a camera address; false unless it is a JPEG. The address may be any camera the printer's
// owner set up, not only one on the printer itself (DECISIONS 2026-10-07), so the download is capped instead.
bool fetch_jpeg(const std::string& url, std::string& jpeg, std::string& err)
{
    constexpr size_t max_picture_bytes = 16 * 1024 * 1024; // a 4K webcam frame is a few MB
    if (!http_get(url, {}, jpeg, err, max_picture_bytes))
        return false;
    if (jpeg.size() < 4 || (unsigned char) jpeg[0] != 0xFF || (unsigned char) jpeg[1] != 0xD8) {
        err = "the camera did not send a picture";
        return false;
    }
    return true;
}

// The first picture of an mjpg stream. The Adventurer 5M's mjpg-streamer serves only the stream (?action=snapshot
// never answers), so the picture is read from it and the download stopped there.
bool fetch_stream_frame(const std::string& url, std::string& jpeg, std::string& err)
{
    constexpr size_t max_stream_bytes = 16 * 1024 * 1024;
    bool             got              = false;
    auto             http             = Http::get(url);
    http.timeout_connect(TIMEOUT_CONNECT_S)
        .timeout_max(TIMEOUT_MAX_S)
        .size_limit(max_stream_bytes)
        .on_progress([&](Http::Progress p, bool& cancel) {
            if (!got && forca_first_mjpeg_frame(p.buffer, jpeg))
                got = cancel = true;
        })
        .on_complete([&](std::string body, unsigned) {
            if (!got)
                got = forca_first_mjpeg_frame(body, jpeg);
            if (!got)
                err = "the camera did not send a picture";
        })
        .on_error([&](std::string, std::string error, unsigned status) {
            if (!got)
                err = error + " (HTTP " + std::to_string(status) + ")";
        })
        .perform_sync();
    return got;
}

// POST to a Flashforge printer's local API with its serial number and access code; `body` gets a reply whose code is 0.
bool flashforge_post(const DynamicPrintConfig& config, const std::string& path, const json& extra, std::string& body, std::string& err)
{
    const std::string serial = config.opt_string("flashforge_serial_number");
    const std::string code   = config.opt_string("printhost_apikey");
    if (serial.empty() || code.empty()) {
        err = "the printer's serial number and access code are needed";
        return false;
    }
    json request = { { "serialNumber", serial }, { "checkCode", code } };
    request.update(extra);
    bool ok   = false;
    auto http = Http::post("http://" + host_name(config.opt_string("print_host")) + ":8898/" + path);
    http.timeout_connect(TIMEOUT_CONNECT_S)
        .timeout_max(TIMEOUT_MAX_S)
        .header("Content-Type", "application/json")
        .set_post_body(request.dump())
        .on_complete([&](std::string b, unsigned) {
            body = std::move(b);
            ok   = true;
        })
        .on_error([&](std::string, std::string error, unsigned st) { err = error + " (HTTP " + std::to_string(st) + ")"; })
        .perform_sync();
    if (!ok)
        return false;
    const json reply = json::parse(body, nullptr, false);
    if (reply.is_discarded() || reply.value("code", -1) != 0) {
        err = "the printer refused: " + (reply.is_object() ? reply.value("message", std::string("unknown error")) : body.substr(0, 100));
        return false;
    }
    return true;
}

} // namespace

std::string forca_host_status_kind(const DynamicPrintConfig& config)
{
    if (config.opt_string("print_host").empty())
        return {};
    switch (host_type(config)) {
    case htMoonraker:
    case htElegooLink: // Elegoo's Klipper printers answer Moonraker's API (Centauri Carbon 2 does not: the read fails)
    case htOctoPrint:  // Moonraker also answers OctoPrint's upload API; a real OctoPrint fails the read
        return "moonraker";
    case htFlashforge: return "flashforge";
    default: return {};
    }
}

ForcaHostStatus forca_parse_moonraker_status(const json& result)
{
    const json&     status = result.contains("status") ? obj(result, "status") : result;
    const json&     ps     = obj(status, "print_stats");
    ForcaHostStatus s;
    s.raw_state = str(ps, "state");
    s.file      = str(ps, "filename");
    s.message   = str(ps, "message");
    if (s.raw_state == "printing")
        s.state = ForcaHostState::Printing;
    else if (s.raw_state == "paused")
        s.state = ForcaHostState::Paused;
    else if (s.raw_state == "complete")
        s.state = ForcaHostState::Finished;
    else if (s.raw_state == "cancelled")
        s.state = ForcaHostState::Stopped;
    else if (s.raw_state == "error")
        s.state = ForcaHostState::Failed;
    else if (s.raw_state == "standby")
        s.state = ForcaHostState::Idle;
    if (const double d = num(ps, "print_duration"); d >= 0)
        s.elapsed_s = int(d);
    const json& info = obj(ps, "info");
    s.layer          = int(num(info, "current_layer"));
    s.total_layers   = int(num(info, "total_layer"));
    s.progress       = num(obj(status, "virtual_sdcard"), "progress");
    if (const double p = num(obj(status, "display_status"), "progress"); p >= 0)
        s.progress = p; // what the printer's own screen shows
    const json& extruder = obj(status, "extruder");
    s.nozzle_c           = num(extruder, "temperature");
    s.nozzle_target_c    = num(extruder, "target");
    const json& bed      = obj(status, "heater_bed");
    s.bed_c              = num(bed, "temperature");
    s.bed_target_c       = num(bed, "target");
    return s;
}

ForcaHostStatus forca_parse_flashforge_detail(const json& detail)
{
    ForcaHostStatus s;
    s.raw_state = str(detail, "status");
    s.file      = str(detail, "printFileName");
    const std::string& r = s.raw_state;
    if (r == "printing" || r == "canceling")
        s.state = ForcaHostState::Printing;
    else if (r == "pausing" || r == "pause")
        s.state = ForcaHostState::Paused;
    else if (r == "heating" || r == "busy" || r == "calibrate_doing")
        s.state = ForcaHostState::Preparing;
    else if (r == "completed")
        s.state = ForcaHostState::Finished;
    else if (r == "cancel")
        s.state = ForcaHostState::Stopped;
    else if (r == "error")
        s.state = ForcaHostState::Failed;
    else if (r == "ready")
        s.state = ForcaHostState::Idle;
    s.progress     = num(detail, "printProgress");
    s.layer        = int(num(detail, "printLayer"));
    s.total_layers = int(num(detail, "targetPrintLayer"));
    // The printer says "printing" during its start routine (bed leveling) as well; it ignores Pause until a layer
    // is under way (Adventurer 5M, firmware 5.1.8).
    if (s.state == ForcaHostState::Printing && r == "printing" && s.layer == 0)
        s.state = ForcaHostState::Preparing;
    if (const double d = num(detail, "printDuration"); d >= 0)
        s.elapsed_s = int(d);
    if (const double e = num(detail, "estimatedTime"); e >= 0)
        s.remaining_s = int(e);
    // Single-nozzle printers (Adventurer 5M) report their nozzle as the right one.
    const bool left = num(detail, "rightTargetTemp", 0) <= 0 && num(detail, "leftTargetTemp", 0) > 0;
    s.nozzle_c        = num(detail, left ? "leftTemp" : "rightTemp");
    s.nozzle_target_c = num(detail, left ? "leftTargetTemp" : "rightTargetTemp");
    s.filament_type   = str(detail, left ? "leftFilamentType" : "rightFilamentType");
    s.bed_c           = num(detail, "platTemp");
    s.bed_target_c    = num(detail, "platTargetTemp");
    s.chamber_c       = num(detail, "chamberTemp");
    s.light_on        = str(detail, "lightStatus") == "open";
    s.camera_url      = boost::algorithm::trim_copy(str(detail, "cameraStreamUrl"));
    if (s.state == ForcaHostState::Failed)
        s.message = str(detail, "errorCode");
    return s;
}

std::string forca_camera_snapshot_url(const std::string& camera_url)
{
    std::string url = boost::algorithm::trim_copy(camera_url);
    if (url.empty())
        return url;
    const std::string lower = boost::algorithm::to_lower_copy(url);
    if (const size_t stream = lower.find("action=stream"); stream != std::string::npos)
        return url.replace(stream, std::strlen("action=stream"), "action=snapshot");
    if (lower.find("snapshot") != std::string::npos || boost::algorithm::ends_with(lower, ".jpg") ||
        boost::algorithm::ends_with(lower, ".jpeg") || url.find('?') != std::string::npos)
        return url;
    return (boost::algorithm::ends_with(url, "/") ? url : url + "/") + "?action=snapshot";
}

std::string forca_flashforge_camera_url(const std::string& print_host, const ForcaHostStatus& status)
{
    return !status.camera_url.empty() ? status.camera_url : "http://" + host_name(print_host) + ":8080/?action=stream";
}

bool forca_first_mjpeg_frame(const std::string& data, std::string& jpeg)
{
    static const std::string soi("\xFF\xD8\xFF", 3), eoi("\xFF\xD9", 2);
    const size_t start = data.find(soi);
    if (start == std::string::npos)
        return false;
    const size_t end = data.find(eoi, start + soi.size());
    if (end == std::string::npos)
        return false;
    jpeg = data.substr(start, end + eoi.size() - start);
    return true;
}

std::string forca_moonraker_job_end(const std::string& s)
{
    if (s == "completed")
        return "finished";
    if (s == "cancelled")
        return "stopped";
    if (s == "error" || s == "klippy_shutdown" || s == "klippy_disconnect" || s == "server_exit" || s == "interrupted")
        return "failed";
    return {};
}

bool forca_same_job_file(const std::string& a, const std::string& b)
{
    const std::string sa = job_stem(a), sb = job_stem(b);
    return !sa.empty() && sa == sb;
}

bool forca_fetch_host_status(const DynamicPrintConfig& config, ForcaHostStatus& status, std::string& err)
{
    const std::string kind = forca_host_status_kind(config);
    const std::string host = config.opt_string("print_host");
    std::string       body;
    try {
        if (kind == "moonraker") {
            const std::string url = base_url(host) +
                                    "/printer/objects/query?print_stats&virtual_sdcard&display_status&extruder&heater_bed";
            if (!http_get(url, config.opt_string("printhost_apikey"), body, err))
                return false;
            const json reply = json::parse(body);
            if (!reply.contains("result")) {
                err = "not a Moonraker reply";
                return false;
            }
            status = forca_parse_moonraker_status(reply["result"]);
            std::string history, history_err;
            if (status.state == ForcaHostState::Idle &&
                http_get(base_url(host) + "/server/history/list?limit=1&order=desc", config.opt_string("printhost_apikey"),
                         history, history_err)) {
                const json  h    = json::parse(history, nullptr, false);
                const json& res  = h.is_object() ? obj(h, "result") : obj(json::object(), "result");
                const auto  jobs = res.find("jobs");
                if (jobs != res.end() && jobs->is_array() && !jobs->empty() && (*jobs)[0].is_object()) {
                    status.last_job_file = str((*jobs)[0], "filename");
                    status.last_job_end  = forca_moonraker_job_end(str((*jobs)[0], "status"));
                }
            }
            return true;
        }
        if (kind == "flashforge") {
            if (!flashforge_post(config, "detail", json::object(), body, err))
                return false;
            const json reply = json::parse(body);
            status           = forca_parse_flashforge_detail(reply.contains("detail") ? reply["detail"] : reply);
            return true;
        }
        err = "Forca can't read this printer's status";
    } catch (const std::exception& e) {
        err = std::string("unexpected reply: ") + e.what();
    }
    return false;
}

// ---- Snapmaker U1 camera (stock firmware) ---------------------------------------------------------------------------

bool forca_u1_camera_root_listed(const json& roots_reply)
{
    const auto result = roots_reply.find("result");
    if (result == roots_reply.end() || !result->is_array())
        return false;
    return std::any_of(result->begin(), result->end(), [](const json& root) { return root.is_object() && str(root, "name") == "camera"; });
}

double forca_u1_monitor_stamp(const json& list_reply)
{
    const auto result = list_reply.find("result");
    if (result != list_reply.end() && result->is_array())
        for (const json& file : *result)
            if (file.is_object() && str(file, "path") == "monitor.jpg")
                return num(file, "modified");
    return -1;
}

namespace {
constexpr double U1_RENEW_S = 240; // the firmware stops the monitor 360 s after the last start_monitor
constexpr double U1_IDLE_S  = 60;  // stop once no viewer has asked for this long
} // namespace

ForcaU1CameraStep forca_u1_camera_step(double now, double wanted, double started)
{
    if (now - wanted >= U1_IDLE_S)
        return ForcaU1CameraStep::Stop;
    if (started < 0 || now - started >= U1_RENEW_S)
        return ForcaU1CameraStep::Start;
    return ForcaU1CameraStep::Wait;
}

namespace {

using U1Clock = std::chrono::steady_clock;

double u1_seconds() { return std::chrono::duration<double>(U1Clock::now().time_since_epoch()).count(); }

struct U1Keeper
{
    std::string api_key;
    double      wanted  = 0;
    bool        running = false;
};

// Never destroyed: a keeper thread may still run while the app exits.
std::mutex&                        s_u1_mutex = *new std::mutex;
std::map<std::string, U1Keeper>&   s_u1       = *new std::map<std::string, U1Keeper>; // base_url -> keeper
std::map<std::string, bool>&       s_u1_hosts = *new std::map<std::string, bool>;     // base_url -> is a U1

// The keeper's next step; on Stop it is marked ended, so the next forca_u1_camera_want starts a new one.
ForcaU1CameraStep u1_step(const std::string& url, double started, std::string& api_key)
{
    std::lock_guard<std::mutex> lock(s_u1_mutex);
    U1Keeper&                   k    = s_u1[url];
    const ForcaU1CameraStep     step = forca_u1_camera_step(u1_seconds(), k.wanted, started);
    if (step == ForcaU1CameraStep::Stop)
        k.running = false;
    api_key = k.api_key;
    return step;
}

std::string u1_request(const char* method, int id)
{
    json params = { { "domain", "lan" } };
    if (std::strcmp(method, "camera.start_monitor") == 0)
        params["interval"] = 0;
    return json{ { "jsonrpc", "2.0" }, { "method", method }, { "params", params }, { "id", id } }.dump();
}

// One websocket to Moonraker, held while the camera is wanted: start_monitor on open and every U1_RENEW_S,
// stop_monitor when no viewer asked for U1_IDLE_S. Moonraker's notifications are read and dropped.
void u1_keep_camera(const std::string& url)
{
    namespace beast = boost::beast;
    namespace asio  = boost::asio;
    namespace ws    = beast::websocket;
    std::string api_key;
    for (;;) {
        if (u1_step(url, -1, api_key) == ForcaU1CameraStep::Stop)
            return;
        try {
            std::string host = url.substr(url.find("//") + 2), port = "80";
            if (const size_t colon = host.find(':'); colon != std::string::npos) {
                port = host.substr(colon + 1);
                host.erase(colon);
            }
            asio::io_context              ioc;
            asio::ip::tcp::resolver       resolver(ioc);
            ws::stream<beast::tcp_stream> sock(ioc);
            beast::get_lowest_layer(sock).expires_after(std::chrono::seconds(TIMEOUT_CONNECT_S));
            beast::get_lowest_layer(sock).connect(resolver.resolve(host, port));
            sock.set_option(ws::stream_base::decorator([api_key](ws::request_type& req) {
                if (!api_key.empty())
                    req.set("X-Api-Key", api_key);
            }));
            sock.handshake(host + (port == "80" ? "" : ":" + port), "/websocket");
            beast::get_lowest_layer(sock).expires_never();

            asio::steady_timer timer(ioc);
            bool               stopping = false, lost = false, writing = false;
            beast::flat_buffer in;
            std::function<void()> read_next = [&]() {
                sock.async_read(in, [&](beast::error_code ec, std::size_t) {
                    in.consume(in.size());
                    if (!ec)
                        read_next();
                    else if (!stopping) { // the printer went away: reconnect while the camera is wanted
                        lost = true;
                        timer.cancel();
                    }
                });
            };
            read_next();
            std::string           out;
            int                   id      = 9000;
            double                started = -1;
            std::function<void()> tick    = [&]() {
                const ForcaU1CameraStep step = writing ? ForcaU1CameraStep::Wait : u1_step(url, started, api_key);
                if (step == ForcaU1CameraStep::Stop) {
                    stopping = true;
                    out      = u1_request("camera.stop_monitor", ++id);
                    sock.async_write(asio::buffer(out), [&](beast::error_code, std::size_t) {
                        sock.async_close(ws::close_code::normal, [](beast::error_code) {});
                    });
                    return;
                }
                if (step == ForcaU1CameraStep::Start) {
                    started = u1_seconds();
                    out     = u1_request("camera.start_monitor", ++id);
                    writing = true;
                    sock.async_write(asio::buffer(out), [&](beast::error_code, std::size_t) { writing = false; });
                }
                timer.expires_after(std::chrono::seconds(1));
                timer.async_wait([&](beast::error_code ec) {
                    if (!ec)
                        tick();
                });
            };
            tick();
            ioc.run();
            if (!lost)
                return; // stopped
            BOOST_LOG_TRIVIAL(info) << "Forca: U1 camera connection to " << url << " lost";
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "Forca: U1 camera connection to " << url << " failed: " << e.what();
        }
        std::this_thread::sleep_for(std::chrono::seconds(5)); // the printer is busy or away: try again while wanted
    }
}

} // namespace

bool forca_is_u1_camera_host(const std::string& print_host, const std::string& api_key)
{
    const std::string url = base_url(print_host);
    {
        std::lock_guard<std::mutex> lock(s_u1_mutex);
        if (auto it = s_u1_hosts.find(url); it != s_u1_hosts.end())
            return it->second;
    }
    std::string body, err;
    if (!http_get(url + "/server/files/roots", api_key, body, err))
        return false; // not cached: the printer may be off now
    const bool is_u1 = forca_u1_camera_root_listed(json::parse(body, nullptr, false));
    std::lock_guard<std::mutex> lock(s_u1_mutex);
    s_u1_hosts[url] = is_u1;
    return is_u1;
}

void forca_u1_camera_want(const std::string& print_host, const std::string& api_key)
{
    const std::string url = base_url(print_host);
    std::lock_guard<std::mutex> lock(s_u1_mutex);
    U1Keeper&                   k = s_u1[url];
    k.wanted                      = u1_seconds();
    k.api_key                     = api_key;
    if (!k.running) {
        k.running = true;
        std::thread([url]() { u1_keep_camera(url); }).detach();
    }
}

bool forca_u1_snapshot_camera_listed(const json& webcams_reply)
{
    const auto result = webcams_reply.find("result");
    if (result == webcams_reply.end() || !result->is_object() || !result->contains("webcams") || !(*result)["webcams"].is_array())
        return false;
    const std::string path = FORCA_U1_SNAPSHOT_PATH;
    for (const json& cam : (*result)["webcams"]) { // the address as typed: the path alone, or with the printer's address
        const std::string snapshot = cam.is_object() ? str(cam, "snapshot_url") : std::string();
        if (snapshot.size() >= path.size() && snapshot.compare(snapshot.size() - path.size(), path.size(), path) == 0)
            return true;
    }
    return false;
}

bool forca_fetch_u1_snapshot_camera(const std::string& print_host, const std::string& api_key, bool& saved)
{
    std::string body, err;
    if (!http_get(base_url(print_host) + "/server/webcams/list", api_key, body, err))
        return false;
    saved = forca_u1_snapshot_camera_listed(json::parse(body, nullptr, false));
    return true;
}

namespace {

// A U1 picture taken after this call: the camera is woken (or kept on) and monitor.jpg read once it has changed.
bool fetch_u1_frame(const std::string& print_host, const std::string& api_key, std::string& jpeg, std::string& err)
{
    const std::string url   = base_url(print_host);
    const auto        stamp = [&]() {
        std::string body, e;
        return http_get(url + "/server/files/list?root=camera", api_key, body, e)
                   ? forca_u1_monitor_stamp(json::parse(body, nullptr, false)) : -1.;
    };
    const double before = stamp();
    forca_u1_camera_want(print_host, api_key);
    for (int i = 0; i < 20; ++i) { // the first picture lands about 1 s after the start
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const double now = stamp();
        if (now >= 0 && now != before)
            return fetch_jpeg(url + FORCA_U1_SNAPSHOT_PATH, jpeg, err);
    }
    err = "the U1's camera did not send a new picture within 10 seconds";
    return false;
}

std::mutex                               s_viewers_mutex;
std::map<std::string, ForcaCameraViewer> s_viewers; // print_host -> Forca's live view of its camera
} // namespace

void forca_set_camera_viewer(const std::string& print_host, ForcaCameraViewer viewer)
{
    std::lock_guard<std::mutex> lock(s_viewers_mutex);
    if (viewer)
        s_viewers[print_host] = std::move(viewer);
    else
        s_viewers.erase(print_host);
}

ForcaCameraLease::ForcaCameraLease(const std::string& print_host)
{
    {
        std::lock_guard<std::mutex> lock(s_viewers_mutex);
        if (auto it = s_viewers.find(print_host); it != s_viewers.end())
            m_viewer = it->second;
    }
    if (m_viewer)
        m_released = m_viewer(true);
}

ForcaCameraLease::~ForcaCameraLease()
{
    if (m_viewer)
        m_viewer(false);
}

bool forca_fetch_host_snapshot(const DynamicPrintConfig& config, std::string& jpeg, std::string& err)
{
    const std::string kind = forca_host_status_kind(config);
    if (kind != "moonraker" && kind != "flashforge") {
        err = "no camera";
        return false;
    }
    const std::string host = config.opt_string("print_host");
    std::string       body;
    try {
        std::string snapshot;
        if (kind == "flashforge") { // models with a camera (Adventurer 5M Pro ...) report its mjpg stream in 'detail'
            if (!flashforge_post(config, "detail", json::object(), body, err))
                return false;
            const json reply = json::parse(body);
            const std::string stream =
                forca_flashforge_camera_url(host, forca_parse_flashforge_detail(reply.contains("detail") ? reply["detail"] : reply));
            // Forca's own Device page may be the camera's one viewer: it lets go while the picture is taken.
            ForcaCameraLease lease(host);
            if (lease.released())
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            if (!fetch_stream_frame(stream, jpeg, err)) {
                err = "the camera did not answer: it serves one viewer at a time, so it may be open in Flashforge's app or "
                      "another slicer, or the printer has no camera (" + err + ")";
                return false;
            }
            return true;
        }
        // A U1 first: a Fluidd camera saved on its monitor.jpg would read a stale picture while the camera sleeps.
        if (forca_is_u1_camera_host(host, config.opt_string("printhost_apikey")))
            return fetch_u1_frame(host, config.opt_string("printhost_apikey"), jpeg, err);
        if (!http_get(base_url(host) + "/server/webcams/list", config.opt_string("printhost_apikey"), body, err))
            return false;
        const json reply = json::parse(body);
        const json& result = obj(reply, "result");
        const auto  cams   = result.find("webcams");
        for (const json& cam : cams != result.end() && cams->is_array() ? *cams : json::array())
            if (cam.is_object() && cam.value("enabled", true) && !str(cam, "snapshot_url").empty()) {
                snapshot = str(cam, "snapshot_url");
                break;
            }
        // Older Moonraker builds (Elegoo's, for one) list no webcams: Fluidd keeps its cameras in the database
        // (namespace "fluidd", key "cameras", stream "url"); Mainsail before 2.5 in namespace "webcams".
        const auto from_database = [&](const std::string& query, const char* list_key, const char* url_key) {
            std::string db, db_err;
            if (!snapshot.empty() || !http_get(base_url(host) + query, config.opt_string("printhost_apikey"), db, db_err))
                return;
            const json  d     = json::parse(db, nullptr, false);
            const json& value = d.is_object() ? obj(obj(d, "result"), "value") : obj(json::object(), "value");
            json        list  = list_key ? (value.contains(list_key) ? value[list_key] : json::array()) : value;
            if (list.is_object()) { // Mainsail: {id: camera}
                json a = json::array();
                for (auto& item : list.items())
                    a.push_back(item.value());
                list = a;
            }
            for (const json& cam : list.is_array() ? list : json::array())
                if (cam.is_object() && cam.value("enabled", true)) {
                    snapshot = forca_camera_snapshot_url(str(cam, "urlSnapshot").empty() ? str(cam, url_key) : str(cam, "urlSnapshot"));
                    if (!snapshot.empty())
                        break;
                }
        };
        from_database("/server/database/item?namespace=fluidd&key=cameras", "cameras", "url");
        from_database("/server/database/item?namespace=webcams", nullptr, "urlStream");
        if (snapshot.empty()) {
            err = "no camera";
            return false;
        }
        // A relative snapshot address belongs to the printer's web server (port 80), not to Moonraker's own port.
        if (!boost::algorithm::istarts_with(snapshot, "http"))
            snapshot = "http://" + host_name(host) + (snapshot.front() == '/' ? "" : "/") + snapshot;
        return fetch_jpeg(snapshot, jpeg, err);
    } catch (const std::exception& e) {
        err = std::string("unexpected reply: ") + e.what();
    }
    return false;
}

bool forca_flashforge_control(const DynamicPrintConfig& config, const std::string& cmd, const json& args, std::string& err)
{
    std::string body;
    try {
        return flashforge_post(config, "control", { { "payload", { { "cmd", cmd }, { "args", args } } } }, body, err);
    } catch (const std::exception& e) {
        err = e.what();
    }
    return false;
}

ForcaHostCaps forca_host_caps(const DynamicPrintConfig& config)
{
    ForcaHostCaps     caps;
    const std::string kind = forca_host_status_kind(config);
    if (kind == "flashforge" &&
        (config.opt_string("flashforge_serial_number").empty() || config.opt_string("printhost_apikey").empty()))
        return caps; // without them only the old 8899 protocol answers, which Forca doesn't read
    if (kind.empty())
        return caps;
    caps.status = caps.camera = caps.pause = caps.grant = true;
    return caps;
}

std::string forca_host_state_name(ForcaHostState state)
{
    switch (state) {
    case ForcaHostState::Idle: return "idle";
    case ForcaHostState::Preparing: return "preparing";
    case ForcaHostState::Printing: return "printing";
    case ForcaHostState::Paused: return "paused";
    case ForcaHostState::Finished: return "finished";
    case ForcaHostState::Stopped: return "stopped";
    case ForcaHostState::Failed: return "failed";
    default: return "unknown";
    }
}

bool forca_host_busy(ForcaHostState state) { return !forca_host_idle(state) && state != ForcaHostState::Failed; }

bool forca_host_idle(ForcaHostState state)
{
    return state == ForcaHostState::Idle || state == ForcaHostState::Finished || state == ForcaHostState::Stopped;
}

std::string forca_host_job_key(const ForcaHostStatus& status)
{
    return job_stem(status.file.empty() ? status.last_job_file : status.file);
}

std::string forca_host_pause_refusal(const ForcaHostStatus& status)
{
    switch (status.state) {
    case ForcaHostState::Printing: return {};
    case ForcaHostState::Paused: return "the print is already paused";
    case ForcaHostState::Preparing: return "the printer is still preparing (heating or leveling) and ignores a pause until the first layer";
    default: return "the printer is not printing (" + (status.raw_state.empty() ? std::string("state unknown") : "state " + status.raw_state) + ")";
    }
}

std::string forca_moonraker_print_url(const std::string& print_host, const std::string& action)
{
    return base_url(print_host) + "/printer/print/" + action;
}

bool forca_same_print_host(const std::string& a, const std::string& b)
{
    return !boost::algorithm::trim_copy(a).empty() && boost::algorithm::iequals(base_url(a), base_url(b));
}

bool forca_host_pause_print(const DynamicPrintConfig& config, bool pause, std::string& err)
{
    const std::string kind = forca_host_status_kind(config);
    if (kind == "flashforge")
        return forca_flashforge_control(config, "jobCtl_cmd", { { "jobID", "" }, { "action", pause ? "pause" : "continue" } }, err);
    if (kind != "moonraker") {
        err = "Forca can't pause this printer";
        return false;
    }
    bool              ok  = false;
    const std::string key = config.opt_string("printhost_apikey");
    auto              http = Http::post(forca_moonraker_print_url(config.opt_string("print_host"), pause ? "pause" : "resume"));
    // Moonraker answers when Klipper's PAUSE / RESUME macro ends, and a RESUME that reheats the nozzle (the Elegoo
    // Giga's) runs longer than a status read may: 15 s timed out while the printer went on to resume.
    http.timeout_connect(TIMEOUT_CONNECT_S).timeout_max(TIMEOUT_PAUSE_S);
    if (!key.empty())
        http.header("X-Api-Key", key);
    http.on_complete([&](std::string, unsigned) { ok = true; })
        .on_error([&](std::string body, std::string error, unsigned status) {
            err = error + " (HTTP " + std::to_string(status) + ")";
            const json reply = json::parse(body, nullptr, false);
            if (reply.is_object() && reply.contains("error") && reply["error"].is_object())
                err = reply["error"].value("message", err);
        })
        .perform_sync();
    return ok;
}

std::string forca_web_interface_name(const std::string& html)
{
    const std::string page = boost::algorithm::to_lower_copy(html.substr(0, 4096)); // the <head> is enough
    const size_t      title = page.find("<title");
    const std::string head  = title == std::string::npos ? page : page.substr(title, 200);
    if (head.find("fluidd") != std::string::npos)
        return "Fluidd";
    if (head.find("mainsail") != std::string::npos)
        return "Mainsail";
    return {};
}

std::vector<std::pair<std::string, std::string>> forca_find_web_interfaces(const std::string& host)
{
    std::vector<std::pair<std::string, std::string>> found;
    const std::string                                ip = "http://" + host_name(host);
    for (const std::string& url : { ip, ip + ":81", ip + ":4408", ip + ":4409", ip + "/fluidd/", ip + "/mainsail/" }) {
        std::string body;
        auto        http = Http::get(url);
        http.timeout_connect(2)
            .timeout_max(4)
            .on_complete([&](std::string b, unsigned) { body = std::move(b); })
            .on_error([](std::string, std::string, unsigned) {})
            .perform_sync();
        const std::string name = forca_web_interface_name(body);
        if (!name.empty() && std::none_of(found.begin(), found.end(), [&](const auto& f) { return f.first == name; }))
            found.emplace_back(name, url);
    }
    return found;
}

bool forca_find_host_config(const std::string& host, DynamicPrintConfig& config)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (!bundle || host.empty())
        return false;
    const auto same = [&](const DynamicPrintConfig& c) { return c.has("print_host") && forca_same_print_host(c.opt_string("print_host"), host); };
    if (const DynamicPrintConfig* selected = bundle->physical_printers.get_selected_printer_config(); selected && same(*selected)) {
        config = *selected;
        return true;
    }
    if (same(bundle->printers.get_edited_preset().config)) {
        config = bundle->printers.get_edited_preset().config;
        return true;
    }
    for (const PhysicalPrinter& printer : bundle->physical_printers)
        if (same(printer.config)) {
            config = printer.config;
            return true;
        }
    for (const Preset& preset : bundle->printers)
        if (same(preset.config)) {
            config = preset.config;
            return true;
        }
    return false;
}

}} // namespace Slic3r::GUI
