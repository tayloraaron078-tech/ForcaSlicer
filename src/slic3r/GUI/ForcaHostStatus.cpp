#include "ForcaHostStatus.hpp"

#include "GUI_App.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cstring>

namespace Slic3r { namespace GUI {

using json = nlohmann::json;

namespace {

constexpr long TIMEOUT_CONNECT_S = 5;
constexpr long TIMEOUT_MAX_S     = 15;

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
            snapshot         = forca_camera_snapshot_url(str(reply.contains("detail") ? reply["detail"] : reply, "cameraStreamUrl"));
            if (snapshot.empty()) {
                err = "no camera";
                return false;
            }
            return fetch_jpeg(snapshot, jpeg, err);
        }
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
    const auto same = [&](const DynamicPrintConfig& c) {
        return c.has("print_host") && boost::algorithm::iequals(base_url(c.opt_string("print_host")), base_url(host));
    };
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
