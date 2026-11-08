// Forca: reading Klipper (Moonraker) and stock Flashforge printer status (src/slic3r/GUI/ForcaHostStatus.cpp) -- the
// reply parsers and job matching; no network.
// Same Windows include prologue as test_forca_brand.cpp (wx pulls in <windows.h>).
#include "catch2/matchers/catch_matchers_floating_point.hpp"
#include "catch2/catch_test_macros.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include <libslic3r/Config.hpp>
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

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/ForcaAIHostPrinters.hpp"
#include "slic3r/GUI/ForcaHostStatus.hpp"
#include "slic3r/Utils/PrintHost.hpp"

#include <nlohmann/json.hpp>

#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;
using json = nlohmann::json;
using Catch::Matchers::WithinAbs;

TEST_CASE("Moonraker print_stats states map to Forca's states", "[ForcaHostStatus]")
{
    const auto state = [](const char* s) { return forca_parse_moonraker_status({ { "status", { { "print_stats", { { "state", s } } } } } }).state; };
    CHECK(state("printing") == ForcaHostState::Printing);
    CHECK(state("paused") == ForcaHostState::Paused);
    CHECK(state("complete") == ForcaHostState::Finished);
    CHECK(state("cancelled") == ForcaHostState::Stopped);
    CHECK(state("error") == ForcaHostState::Failed);
    CHECK(state("standby") == ForcaHostState::Idle);
    CHECK(state("something new") == ForcaHostState::Unknown);
    CHECK(forca_parse_moonraker_status(json::object()).state == ForcaHostState::Unknown);
}

TEST_CASE("A Moonraker status reply gives file, layer, progress and temperatures", "[ForcaHostStatus]")
{
    const json result = json::parse(R"({"eventtime": 1.0, "status": {
        "print_stats": {"state": "error", "filename": "gcodes/whistle.gcode", "message": "Heater extruder not heating",
                        "print_duration": 612.7, "info": {"current_layer": 12, "total_layer": 25}},
        "virtual_sdcard": {"progress": 0.40}, "display_status": {"progress": 0.42},
        "extruder": {"temperature": 219.6, "target": 220.0}, "heater_bed": {"temperature": 59.9, "target": 60.0}}})");
    const ForcaHostStatus s = forca_parse_moonraker_status(result);
    CHECK(s.state == ForcaHostState::Failed);
    CHECK(s.file == "gcodes/whistle.gcode");
    CHECK(s.message == "Heater extruder not heating");
    CHECK(s.elapsed_s == 612);
    CHECK(s.layer == 12);
    CHECK(s.total_layers == 25);
    CHECK_THAT(s.progress, WithinAbs(0.42, 0.01)); // the printer screen's own progress wins
    CHECK_THAT(s.nozzle_c, WithinAbs(219.6, 0.01));
    CHECK_THAT(s.bed_target_c, WithinAbs(60.0, 0.01));
}

TEST_CASE("A Flashforge detail reply (Adventurer 5M, firmware 5.1.8) is read", "[ForcaHostStatus]")
{
    // Trimmed from a real reply; identifying fields left out.
    const json detail = json::parse(R"({"status": "printing", "printFileName": "TPU_Insert.3mf", "printLayer": 3,
        "targetPrintLayer": 9, "printProgress": 0.12521888315677643, "printDuration": 810, "estimatedTime": 4705.4476274997,
        "rightTemp": 225.9600067138672, "rightTargetTemp": 225.0, "rightFilamentType": "TPU", "leftTemp": 0,
        "leftTargetTemp": 0, "leftFilamentType": "", "platTemp": 45.060001373291016, "platTargetTemp": 45.0,
        "chamberTemp": 0, "lightStatus": "open", "errorCode": "", "cameraStreamUrl": ""})");
    const ForcaHostStatus s = forca_parse_flashforge_detail(detail);
    CHECK(s.state == ForcaHostState::Printing);
    CHECK(s.file == "TPU_Insert.3mf");
    CHECK(s.layer == 3);
    CHECK(s.total_layers == 9);
    CHECK_THAT(s.progress, WithinAbs(0.1252, 0.001));
    CHECK(s.elapsed_s == 810);
    CHECK(s.remaining_s == 4705);
    CHECK_THAT(s.nozzle_c, WithinAbs(225.96, 0.01));
    CHECK_THAT(s.nozzle_target_c, WithinAbs(225.0, 0.01));
    CHECK(s.filament_type == "TPU");
    CHECK_THAT(s.bed_c, WithinAbs(45.06, 0.01));
    CHECK(s.light_on);
    CHECK(s.message.empty());
}

TEST_CASE("Flashforge status words map to Forca's states", "[ForcaHostStatus]")
{
    const auto state = [](const char* s) { return forca_parse_flashforge_detail({ { "status", s } }).state; };
    CHECK(state("ready") == ForcaHostState::Idle);
    CHECK(state("heating") == ForcaHostState::Preparing);
    CHECK(state("busy") == ForcaHostState::Preparing);
    CHECK(state("calibrate_doing") == ForcaHostState::Preparing);
    CHECK(state("printing") == ForcaHostState::Printing);
    CHECK(state("canceling") == ForcaHostState::Printing);
    CHECK(state("pausing") == ForcaHostState::Paused);
    CHECK(state("pause") == ForcaHostState::Paused);
    CHECK(state("completed") == ForcaHostState::Finished);
    CHECK(state("cancel") == ForcaHostState::Stopped);
    CHECK(state("error") == ForcaHostState::Failed);
    CHECK(forca_parse_flashforge_detail({ { "status", "error" }, { "errorCode", "E0012" } }).message == "E0012");
}

TEST_CASE("A Flashforge printer still leveling before layer 1 is preparing, not printing", "[ForcaHostStatus]")
{
    CHECK(forca_parse_flashforge_detail({ { "status", "printing" }, { "printLayer", 0 } }).state == ForcaHostState::Preparing);
    CHECK(forca_parse_flashforge_detail({ { "status", "printing" }, { "printLayer", 1 } }).state == ForcaHostState::Printing);
    CHECK(forca_parse_flashforge_detail({ { "status", "pause" }, { "printLayer", 0 } }).state == ForcaHostState::Paused);
}

TEST_CASE("A left-only Flashforge nozzle is read from the left fields", "[ForcaHostStatus]")
{
    const ForcaHostStatus s = forca_parse_flashforge_detail(
        { { "status", "printing" }, { "leftTemp", 210.0 }, { "leftTargetTemp", 210.0 }, { "leftFilamentType", "PLA" },
          { "rightTemp", 25.0 }, { "rightTargetTemp", 0 } });
    CHECK_THAT(s.nozzle_target_c, WithinAbs(210.0, 0.01));
    CHECK(s.filament_type == "PLA");
}

TEST_CASE("Moonraker's job history says how a print ended", "[ForcaHostStatus]")
{
    CHECK(forca_moonraker_job_end("completed") == "finished");
    CHECK(forca_moonraker_job_end("cancelled") == "stopped");
    CHECK(forca_moonraker_job_end("error") == "failed");
    CHECK(forca_moonraker_job_end("klippy_shutdown") == "failed");
    CHECK(forca_moonraker_job_end("in_progress") == "");
    CHECK(forca_moonraker_job_end("") == "");
}

TEST_CASE("A camera's still-picture address comes from what Fluidd or Mainsail saved", "[ForcaHostStatus]")
{
    CHECK(forca_camera_snapshot_url("http://100.69.18.103:8080/") == "http://100.69.18.103:8080/?action=snapshot");
    CHECK(forca_camera_snapshot_url("http://cam.local:8080") == "http://cam.local:8080/?action=snapshot");
    CHECK(forca_camera_snapshot_url("/webcam/?action=stream") == "/webcam/?action=snapshot");
    CHECK(forca_camera_snapshot_url("/webcam/?action=snapshot") == "/webcam/?action=snapshot");
    CHECK(forca_camera_snapshot_url("http://cam/image.jpg") == "http://cam/image.jpg");
    CHECK(forca_camera_snapshot_url("") == "");
}

TEST_CASE("A Flashforge camera streams where the printer says, else on port 8080 of the printer", "[ForcaHostStatus]")
{
    const ForcaHostStatus reported = forca_parse_flashforge_detail({ { "cameraStreamUrl", " http://192.168.1.6:8080/?action=stream " } });
    CHECK(reported.camera_url == "http://192.168.1.6:8080/?action=stream");
    CHECK(forca_flashforge_camera_url("192.168.1.9", reported) == "http://192.168.1.6:8080/?action=stream");
    // An Adventurer 5M whose firmware reports no camera: mjpg-streamer on the printer's port 8080.
    const ForcaHostStatus none = forca_parse_flashforge_detail({ { "status", "ready" } });
    CHECK(none.camera_url.empty());
    CHECK(forca_flashforge_camera_url("http://192.168.1.6:8898/", none) == "http://192.168.1.6:8080/?action=stream");
}

TEST_CASE("The first picture of an mjpg stream is read once it has arrived whole", "[ForcaHostStatus]")
{
    // mjpg-streamer's parts: a boundary and headers, then the JPEG (SOI FF D8 FF ... EOI FF D9).
    const std::string frame1("\xFF\xD8\xFF\xE0" "one" "\xFF\xD9", 9);
    const std::string frame2("\xFF\xD8\xFF\xE0" "two" "\xFF\xD9", 9);
    const std::string head = "--boundarydonotcross\r\nContent-Type: image/jpeg\r\nContent-Length: 9\r\n\r\n";
    const std::string stream = head + frame1 + "\r\n" + head + frame2;

    std::string jpeg;
    CHECK(forca_first_mjpeg_frame(stream, jpeg));
    CHECK(jpeg == frame1);
    // Not yet: no picture, or the first one cut off.
    CHECK_FALSE(forca_first_mjpeg_frame(head, jpeg));
    CHECK_FALSE(forca_first_mjpeg_frame(head + frame1.substr(0, 6), jpeg));
    // A single picture (a snapshot address) is its own first frame.
    CHECK(forca_first_mjpeg_frame(frame2, jpeg));
    CHECK(jpeg == frame2);
}

TEST_CASE("Forca's own camera view lets go of the stream while a picture is taken", "[ForcaHostStatus]")
{
    std::vector<bool> calls;
    bool              showing = true;
    forca_set_camera_viewer("192.168.1.6", [&](bool hold) {
        calls.push_back(hold);
        return showing;
    });
    {
        ForcaCameraLease lease("192.168.1.6");
        CHECK(lease.released());
        CHECK(calls == std::vector<bool>{ true });
    }
    CHECK(calls == std::vector<bool>{ true, false });

    // Another printer's camera, or a view that shows no stream: nothing to wait for.
    {
        ForcaCameraLease other("192.168.1.7");
        CHECK_FALSE(other.released());
    }
    CHECK(calls.size() == 2);
    showing = false;
    {
        ForcaCameraLease lease("192.168.1.6");
        CHECK_FALSE(lease.released());
    }
    CHECK(calls.size() == 4);

    // Once the view is gone it is not called any more.
    forca_set_camera_viewer("192.168.1.6", nullptr);
    {
        ForcaCameraLease lease("192.168.1.6");
        CHECK_FALSE(lease.released());
    }
    CHECK(calls.size() == 4);
}

TEST_CASE("A Snapmaker U1 is told by the camera file root its Moonraker lists", "[ForcaHostStatus]")
{
    // As the U1 (firmware 1.5.1.2) answers /server/files/roots.
    const nlohmann::json u1 = nlohmann::json::parse(R"({"result": [
        {"name": "config", "path": "/oem/printer_data/config", "permissions": "r"},
        {"name": "gcodes", "path": "/userdata/gcodes", "permissions": "rw"},
        {"name": "camera", "path": "/oem/printer_data/camera", "permissions": "r"}]})");
    CHECK(forca_u1_camera_root_listed(u1));
    const nlohmann::json klipper = nlohmann::json::parse(R"({"result": [{"name": "config"}, {"name": "gcodes"}]})");
    CHECK_FALSE(forca_u1_camera_root_listed(klipper));
    // An older Moonraker without the endpoint answers an error.
    CHECK_FALSE(forca_u1_camera_root_listed(nlohmann::json::parse(R"({"error": {"code": 404}})")));
}

TEST_CASE("A Fluidd camera on the U1's picture is told from Moonraker's webcams", "[ForcaHostStatus]")
{
    // As the U1 answers /server/webcams/list once the camera is added in Fluidd.
    const nlohmann::json added = nlohmann::json::parse(R"({"result": {"webcams": [{"name": "Main", "service": "mjpegstreamer-adaptive",
        "stream_url": "/webcam/?action=stream", "snapshot_url": "/server/files/camera/monitor.jpg"}]}})");
    CHECK(forca_u1_snapshot_camera_listed(added));
    const nlohmann::json full = nlohmann::json::parse(R"({"result": {"webcams": [
        {"snapshot_url": "http://192.168.57.12/server/files/camera/monitor.jpg"}]}})");
    CHECK(forca_u1_snapshot_camera_listed(full));
    const nlohmann::json other = nlohmann::json::parse(R"({"result": {"webcams": [{"snapshot_url": "/webcam/?action=snapshot"}]}})");
    CHECK_FALSE(forca_u1_snapshot_camera_listed(other));
    CHECK_FALSE(forca_u1_snapshot_camera_listed(nlohmann::json::parse(R"({"result": {"webcams": []}})")));
    CHECK_FALSE(forca_u1_snapshot_camera_listed(nlohmann::json::parse(R"({"error": {"code": 404}})")));
}

TEST_CASE("The U1 camera picture's time comes from the camera file list", "[ForcaHostStatus]")
{
    const nlohmann::json list = nlohmann::json::parse(R"({"result": [
        {"path": "monitor.jpg", "modified": 1792828778.27, "size": 97932},
        {"path": "Round_Plug.mp4", "modified": 1791883052.37}]})");
    CHECK_THAT(forca_u1_monitor_stamp(list), Catch::Matchers::WithinAbs(1792828778.27, 1e-3));
    CHECK(forca_u1_monitor_stamp(nlohmann::json::parse(R"({"result": [{"path": "Round_Plug.mp4"}]})")) < 0);
    CHECK(forca_u1_monitor_stamp(nlohmann::json::parse(R"({"error": {}})")) < 0);
}

TEST_CASE("The U1 camera is started, renewed before the firmware stops it, and stopped once nobody watches", "[ForcaHostStatus]")
{
    // Seconds: a viewer asked at 100.
    CHECK(forca_u1_camera_step(100, 100, -1) == ForcaU1CameraStep::Start);
    CHECK(forca_u1_camera_step(101, 100, 100) == ForcaU1CameraStep::Wait);
    // Still watched: renewed 240 s after the last start, ahead of the firmware's 360 s.
    CHECK(forca_u1_camera_step(339, 330, 100) == ForcaU1CameraStep::Wait);
    CHECK(forca_u1_camera_step(340, 330, 100) == ForcaU1CameraStep::Start);
    // Nobody asked for a minute.
    CHECK(forca_u1_camera_step(159, 100, 100) == ForcaU1CameraStep::Wait);
    CHECK(forca_u1_camera_step(160, 100, 100) == ForcaU1CameraStep::Stop);
}

TEST_CASE("Job files match without folders, case and gcode/3mf endings", "[ForcaHostStatus]")
{
    CHECK(forca_same_job_file("whistle.gcode", "gcodes/whistle.gcode"));
    CHECK(forca_same_job_file("Whistle.gcode", "whistle.3mf"));
    CHECK(forca_same_job_file("sub\\part_A.gcode.3mf", "part_a.gcode"));
    CHECK_FALSE(forca_same_job_file("whistle.gcode", "whistle2.gcode"));
    CHECK_FALSE(forca_same_job_file("", ""));
}

TEST_CASE("Forca reads status from Klipper and Flashforge print hosts only", "[ForcaHostStatus]")
{
    const auto kind = [](PrintHostType type, const char* host) {
        DynamicPrintConfig c;
        c.set_key_value("print_host", new ConfigOptionString(host));
        c.set_key_value("host_type", new ConfigOptionEnum<PrintHostType>(type));
        return forca_host_status_kind(c);
    };
    CHECK(kind(htMoonraker, "192.168.1.6:7125") == "moonraker");
    CHECK(kind(htElegooLink, "192.168.57.22") == "moonraker");
    CHECK(kind(htOctoPrint, "octopi.local") == "moonraker");
    CHECK(kind(htFlashforge, "192.168.1.6") == "flashforge");
    CHECK(kind(htPrusaLink, "192.168.1.9") == "");
    CHECK(kind(htMoonraker, "") == "");
}

TEST_CASE("The Device tab opens a Moonraker printer's web page, not Moonraker's API port", "[ForcaHostStatus]")
{
    const auto webui = [](const char* host, const char* webui_url) {
        DynamicPrintConfig c;
        c.set_key_value("print_host", new ConfigOptionString(host));
        c.set_key_value("print_host_webui", new ConfigOptionString(webui_url));
        c.set_key_value("host_type", new ConfigOptionEnum<PrintHostType>(htMoonraker));
        return PrintHost::get_print_host_webui(&c);
    };
    CHECK(webui("192.168.1.6:7125", "") == "http://192.168.1.6");
    CHECK(webui("http://printer.local:7125/", "") == "http://printer.local/");
    CHECK(webui("192.168.1.6", "") == "http://192.168.1.6");
    CHECK(webui("192.168.1.6:71250", "") == "http://192.168.1.6:71250");
    CHECK(webui("192.168.1.6:7125", "http://192.168.1.6:81") == "http://192.168.1.6:81"); // the user's Device UI wins
}

TEST_CASE("Fluidd and Mainsail pages are told apart by their title", "[ForcaHostStatus]")
{
    CHECK(forca_web_interface_name("<!DOCTYPE html><html><head><title>Fluidd</title></head>") == "Fluidd");
    CHECK(forca_web_interface_name("<html><head><title>Mainsail</title>") == "Mainsail");
    CHECK(forca_web_interface_name("<html><head><title>OctoPrint</title>") == "");
    CHECK(forca_web_interface_name("") == "");
}

// Forca AI on print-host printers (HQ PLAN_forca_ai_print_hosts.md).

TEST_CASE("Forca AI can follow, pause and grant-print Klipper and Flashforge printers only", "[ForcaHostStatus]")
{
    const auto caps = [](PrintHostType type, const char* serial, const char* code, bool use_3mf) {
        DynamicPrintConfig c;
        c.set_key_value("print_host", new ConfigOptionString("192.168.1.6"));
        c.set_key_value("host_type", new ConfigOptionEnum<PrintHostType>(type));
        c.set_key_value("flashforge_serial_number", new ConfigOptionString(serial));
        c.set_key_value("printhost_apikey", new ConfigOptionString(code));
        c.set_key_value("use_3mf", new ConfigOptionBool(use_3mf));
        return forca_host_caps(c);
    };
    const ForcaHostCaps klipper = caps(htMoonraker, "", "", false);
    CHECK((klipper.status && klipper.camera && klipper.pause && klipper.grant));
    const ForcaHostCaps flashforge = caps(htFlashforge, "SNMOMC9900001", "1234abcd", false);
    CHECK((flashforge.status && flashforge.pause && flashforge.grant));
    // Without its serial number and access code only the old protocol answers, which Forca doesn't read.
    const ForcaHostCaps flashforge_no_code = caps(htFlashforge, "SNMOMC9900001", "", false);
    CHECK_FALSE((flashforge_no_code.status || flashforge_no_code.pause || flashforge_no_code.grant));
    const ForcaHostCaps prusalink = caps(htPrusaLink, "", "", false);
    CHECK_FALSE((prusalink.status || prusalink.camera || prusalink.pause || prusalink.grant));
    // A printer that takes .gcode.3mf uploads (the Flashforge profiles) prints under a grant too.
    const ForcaHostCaps bundle = caps(htFlashforge, "SNMOMC9900001", "1234abcd", true);
    CHECK((bundle.status && bundle.pause && bundle.grant));
}

TEST_CASE("Only an idle printer takes a grant print, and only a busy one drops a clear bed", "[ForcaHostStatus]")
{
    using S = ForcaHostState;
    for (S s : { S::Idle, S::Finished, S::Stopped }) {
        CHECK(forca_host_idle(s));
        CHECK_FALSE(forca_host_busy(s));
    }
    for (S s : { S::Printing, S::Paused, S::Preparing, S::Unknown }) {
        CHECK_FALSE(forca_host_idle(s));
        CHECK(forca_host_busy(s));
    }
    CHECK_FALSE(forca_host_idle(S::Failed)); // an error waits for the user...
    CHECK_FALSE(forca_host_busy(S::Failed)); // ...but is no new print on the bed
    CHECK(forca_host_state_name(S::Paused) == "paused");
    CHECK(forca_host_state_name(S::Unknown) == "unknown");
}

TEST_CASE("A printer's job key survives a reset to standby and changes with the next print", "[ForcaHostStatus]")
{
    ForcaHostStatus done;
    done.file = "gcodes/Benchy_PLA_0.2.gcode";
    ForcaHostStatus standby; // Klipper after SDCARD_RESET_FILE: no current file, the history has the last job
    standby.last_job_file = "Benchy_PLA_0.2.gcode";
    CHECK(forca_host_job_key(done) == forca_host_job_key(standby));
    ForcaHostStatus next;
    next.file = "Whistle.gcode";
    CHECK(forca_host_job_key(next) != forca_host_job_key(done));
    ForcaHostStatus flashforge; // Flashforge renames the extension
    flashforge.file = "BENCHY_PLA_0.2.3mf";
    CHECK(forca_host_job_key(flashforge) == forca_host_job_key(done));
}

TEST_CASE("The AI may pause a print only while it is under way", "[ForcaHostStatus]")
{
    ForcaHostStatus s;
    s.state = ForcaHostState::Printing;
    CHECK(forca_host_pause_refusal(s).empty());
    for (ForcaHostState other : { ForcaHostState::Paused, ForcaHostState::Preparing, ForcaHostState::Idle, ForcaHostState::Unknown }) {
        s.state = other;
        CHECK_FALSE(forca_host_pause_refusal(s).empty());
    }
    // A Flashforge printer in its start routine (layer 0) is preparing, so Forca doesn't send a pause it would ignore.
    const json leveling = { { "status", "printing" }, { "printLayer", 0 } };
    CHECK_FALSE(forca_host_pause_refusal(forca_parse_flashforge_detail(leveling)).empty());
}

TEST_CASE("Moonraker's pause and resume addresses come from the print host as typed", "[ForcaHostStatus]")
{
    CHECK(forca_moonraker_print_url("192.168.1.6:7125", "pause") == "http://192.168.1.6:7125/printer/print/pause");
    CHECK(forca_moonraker_print_url("http://voron.local/fluidd/", "resume") == "http://voron.local/printer/print/resume");
    CHECK(forca_same_print_host("192.168.1.6:7125", "http://192.168.1.6:7125/"));
    CHECK(forca_same_print_host("Voron.Local", "http://voron.local"));
    CHECK_FALSE(forca_same_print_host("192.168.1.6", "192.168.1.6:7125"));
    CHECK_FALSE(forca_same_print_host("", "http://"));
}

TEST_CASE("A print-host printer's AI id is its physical printer's name", "[ForcaHostStatus]")
{
    const std::string id = forca_ai_host_id("Voron 2.4 * Voron 2.4 0.4 nozzle");
    CHECK(forca_ai_is_host_id(id));
    CHECK(forca_ai_host_printer_name(id) == "Voron 2.4 * Voron 2.4 0.4 nozzle");
    CHECK_FALSE(forca_ai_is_host_id("01P00A451601106")); // a Bambu device id
    CHECK_FALSE(forca_ai_is_host_id("host:"));
    CHECK(forca_ai_host_printer_name("01P00A451601106").empty());
    CHECK(forca_ai_host_type_label("moonraker") == "Klipper");
    CHECK(forca_ai_host_type_label("something new") == "print host");
}
