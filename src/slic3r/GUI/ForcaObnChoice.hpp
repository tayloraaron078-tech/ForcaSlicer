#pragma once

// Forca: the one-click choice between Bambu Lab's network plug-in and Open Bamboo Networking (OBN), in Preferences
// and as an offer on the Device tab after Bambu's plug-in crashed Forca. Files: slic3r/Utils/ForcaOBN.{hpp,cpp}.
// Design: docs/HLSD/obn-choice.md.

#include <string>

class wxWindow;

namespace Slic3r { namespace GUI {

bool        forca_obn_active();      // the configured plug-in is OBN
std::string forca_obn_tag();         // the OBN release in use or last used ("v2.2.0"), empty if never
std::string forca_obn_previous_tag(); // the release before it, kept for rolling back; empty if none

// Each shows its own confirmation and result; true when the choice is made. The plug-in changes when Forca next
// starts (no hot reload: see ForcaObnChoice.cpp).
bool forca_obn_switch(wxWindow* parent, bool to_obn);
bool forca_obn_update(wxWindow* parent);   // newer OBN release -> download, keep the current one, switch
bool forca_obn_rollback(wxWindow* parent); // back to the previous OBN release

// Start-up: OBN selected but not loaded -> back to Bambu's plug-in, with a message.
void forca_obn_check_loaded(wxWindow* parent);
// Start-up: remember when the last crash happened inside Bambu's network plug-in (Forca's crash logs).
void forca_obn_note_last_crash();
// Device tab: offer OBN once per such crash, unless the user said not to.
void forca_obn_offer_after_crash(wxWindow* parent);

}} // namespace Slic3r::GUI
