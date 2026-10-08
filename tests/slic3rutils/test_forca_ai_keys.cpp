// Forca AI: which settings the AI may write (src/slic3r/GUI/ForcaAIKeys.hpp) and which callers the bridge accepts
// (ForcaAI.cpp). Same Windows include prologue as test_forca_brand.cpp (wx pulls in <windows.h>).
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

#include "slic3r/GUI/ForcaAI.hpp"
#include "slic3r/GUI/ForcaAIKeys.hpp"

#include <string>

using namespace Slic3r::GUI;

TEST_CASE("The AI never reads or writes printer secrets", "[ForcaAI]")
{
    for (const char* key : { "printhost_apikey", "printhost_password", "printhost_user", "access_code", "Api_Key", "user_token" })
        CHECK(forca_ai_is_secret_key(key));
    for (const char* key : { "layer_height", "sparse_infill_density", "nozzle_temperature", "print_host" })
        CHECK_FALSE(forca_ai_is_secret_key(key));
}

TEST_CASE("The AI may not write commands, G-code templates or where prints go", "[ForcaAI]")
{
    // A post-processing script runs as a command on this computer; G-code templates reach the printer unseen.
    for (const char* key : { "post_process", "machine_start_gcode", "machine_end_gcode", "change_filament_gcode",
                             "layer_change_gcode", "before_layer_change_gcode", "time_lapse_gcode", "template_custom_gcode",
                             "filament_start_gcode", "filament_end_gcode", "machine_pause_gcode", "printing_by_object_gcode" })
        CHECK(forca_ai_may_not_write(key));
    // Where the file and the print go.
    for (const char* key : { "filename_format", "print_host", "print_host_webui", "host_type", "printhost_port",
                             "printhost_cafile", "bbl_use_printhost" })
        CHECK(forca_ai_may_not_write(key));
    // A preset's identity, and secrets.
    for (const char* key : { "inherits", "compatible_printers", "setting_id", "name", "printhost_apikey" })
        CHECK(forca_ai_may_not_write(key));
}

TEST_CASE("The AI may still write ordinary print settings", "[ForcaAI]")
{
    for (const char* key : { "layer_height", "sparse_infill_density", "wall_loops", "enable_support", "support_type",
                             "nozzle_temperature", "filament_flow_ratio", "outer_wall_speed", "brim_type", "seam_position",
                             "support_interface_filament" })
        CHECK_FALSE(forca_ai_may_not_write(key));
}

TEST_CASE("The AI bridge accepts only this computer as origin and host", "[ForcaAI]")
{
    for (const char* origin : { "http://localhost", "http://localhost:3000", "https://127.0.0.1:13630", "http://[::1]:8080",
                                "http://LOCALHOST:1" })
        CHECK(forca_ai_is_local_origin(origin));
    // A prefix match would accept these: a domain that only starts with a local name, or a path after the host.
    for (const char* origin : { "http://localhost.evil.com", "http://127.0.0.1.evil.com", "http://localhost:3000.evil.com",
                                "http://localhost@evil.com", "http://evil.com", "http://localhost/x", "file://localhost",
                                "http://[::1].evil.com", "http://localhostx" })
        CHECK_FALSE(forca_ai_is_local_origin(origin));

    for (const char* host : { "127.0.0.1:13630", "localhost:13630", "localhost", "[::1]:13630" })
        CHECK(forca_ai_is_local_host(host));
    for (const char* host : { "evil.com:13630", "localhost.evil.com:13630", "127.0.0.1.nip.io:13630", "[::2]:13630",
                              "localhost:13630x" })
        CHECK_FALSE(forca_ai_is_local_host(host));
}
