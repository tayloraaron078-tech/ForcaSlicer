// Forca: the Home tab's printer cards (src/slic3r/GUI/ForcaHomePrinters.cpp) -- what a card says; no printers needed.
// Same Windows include prologue as test_forca_brand.cpp (wx pulls in <windows.h>).
#include "catch2/catch_message.hpp"
#include "catch2/catch_test_macros.hpp"
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

#include "slic3r/GUI/ForcaAIHostPrinters.hpp"
#include "slic3r/GUI/ForcaHomePrinters.hpp"
#include "slic3r/GUI/ForcaHostStatus.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>

using namespace Slic3r::GUI;
using json = nlohmann::json;

TEST_CASE("A printer card holds names and states only", "[ForcaHomePrinters]")
{
    ForcaHomeCard card;
    card.id    = "host:Giga";
    card.name  = "Giga";
    card.type  = "Klipper";
    card.state = "idle";
    const json j = forca_home_card_json(card);
    CHECK(j["state"] == "idle");
    CHECK(j["open"] == "");
    // Fields a card doesn't have are left out, so the page shows nothing for them.
    CHECK_FALSE(j.contains("job"));
    CHECK_FALSE(j.contains("progress_pct"));
    CHECK_FALSE(j.contains("remaining_min"));
    for (const auto& [key, value] : j.items()) {
        CAPTURE(key);
        CHECK(key.find("host") == std::string::npos); // no print_host, address or key ever reaches the web page
        CHECK(key.find("key") == std::string::npos);
    }
}

TEST_CASE("A Bambu printer's print status becomes a card state", "[ForcaHomePrinters]")
{
    CHECK(forca_home_bambu_state("RUNNING") == "printing");
    CHECK(forca_home_bambu_state("PAUSE") == "paused");
    CHECK(forca_home_bambu_state("PREPARE") == "preparing");
    CHECK(forca_home_bambu_state("FINISH") == "finished");
    CHECK(forca_home_bambu_state("FAILED") == "failed");
    CHECK(forca_home_bambu_state("IDLE") == "idle");
    CHECK(forca_home_bambu_state("SOMETHING_NEW") == "unknown");
}

TEST_CASE("A host printer card follows the latest status read", "[ForcaHomePrinters]")
{
    ForcaHomeCard card;
    forca_home_fill_host_card(card, false, nullptr);
    CHECK(card.state == "no_status");

    forca_home_fill_host_card(card, true, nullptr);
    CHECK(card.state == "connecting");

    ForcaAIHostSeen seen;
    seen.at = std::chrono::steady_clock::now();
    seen.ok = false;
    forca_home_fill_host_card(card, true, &seen);
    CHECK(card.state == "offline");

    seen.ok                  = true;
    seen.status.state        = ForcaHostState::Printing;
    seen.status.file         = "benchy.gcode";
    seen.status.progress     = 0.424;
    seen.status.remaining_s  = 125 * 60;
    ForcaHomeCard printing;
    forca_home_fill_host_card(printing, true, &seen);
    CHECK(printing.state == "printing");
    CHECK(printing.job == "benchy.gcode");
    CHECK(printing.progress_pct == 42);
    CHECK(printing.remaining_min == 125);

    // An idle printer shows no job or progress, even when it reports the last file.
    seen.status.state = ForcaHostState::Idle;
    ForcaHomeCard idle;
    forca_home_fill_host_card(idle, true, &seen);
    CHECK(idle.state == "idle");
    CHECK(idle.job.empty());
    CHECK(idle.progress_pct == -1);
}
