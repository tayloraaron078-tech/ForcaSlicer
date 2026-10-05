// Forca: reading Klipper (Moonraker) and stock Flashforge printer status (src/slic3r/GUI/ForcaHostStatus.cpp) -- the
// reply parsers and job matching; no network.
// Same Windows include prologue as test_forca_brand.cpp (wx pulls in <windows.h>).
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
#include "slic3r/GUI/ForcaHostStatus.hpp"
#include "slic3r/Utils/PrintHost.hpp"

#include <nlohmann/json.hpp>

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
