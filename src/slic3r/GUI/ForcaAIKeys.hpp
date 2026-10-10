#pragma once

// Forca AI: which settings the AI may read and write. One rule for every AI tool (ForcaAITools.cpp reads,
// ForcaAIHands.cpp writes), so the lists can't drift apart. Tests: tests/slic3rutils/test_forca_ai_keys.cpp.

#include <algorithm>
#include <cctype>
#include <string>

namespace Slic3r { namespace GUI {

// Printer secrets are never read or written by the AI (access codes, API keys, passwords, tokens).
inline bool forca_ai_is_secret_key(const std::string& key)
{
    std::string k = key;
    std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* bad : { "password", "apikey", "api_key", "access_code", "token", "secret", "printhost_user", "cookie" })
        if (k.find(bad) != std::string::npos)
            return true;
    return false;
}

// Settings the AI may never write, even at the Advanced control level, because they would let text the AI read
// (a model name, a file, a web page) reach beyond the slicer:
//  - post-processing scripts run as commands on this computer when G-code is exported;
//  - G-code templates (machine_start_gcode, change_filament_gcode, ...) go to the printer inside a print the user
//    approves without seeing them;
//  - the output file name, and the printer connection, which decides where prints and uploads are sent;
//  - a preset's identity (what it inherits, which printer/process it belongs to).
inline bool forca_ai_may_not_write(const std::string& key)
{
    if (forca_ai_is_secret_key(key))
        return true;
    static const char* const blocked[] = { "post_process", "filename_format", "inherits", "print_settings_id",
                                           "filament_settings_id", "printer_settings_id", "compatible_printers",
                                           "compatible_prints", "setting_id", "name", "host_type", "print_host",
                                           "print_host_webui", "bbl_use_printhost" };
    for (const char* b : blocked)
        if (key == b)
            return true;
    const auto ends_with = [&key](const std::string& suffix) {
        return key.size() >= suffix.size() && key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return key.rfind("printhost_", 0) == 0 || ends_with("_gcode");
}

}} // namespace Slic3r::GUI
