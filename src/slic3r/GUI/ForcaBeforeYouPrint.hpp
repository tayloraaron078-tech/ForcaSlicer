#pragma once

#include <wx/panel.h>
#include <wx/string.h>

#include <functional>
#include <string>

class wxBoxSizer;
class wxWindow;

namespace Slic3r { namespace GUI {

// Forca: the send dialogs' "Before you print" block (HQ PLAN_forca_academy_part2.md §6b). For the plate about to be
// sent it shows, when there is something to say:
//  - how earlier prints with the same printer and filament preset went (Forca Academy, when it is on), with a link
//    that opens the Print Journal on those prints;
//  - the settings changed and not saved in the process, filament or printer preset (with the saved value);
//  - "Details", collapsed by default (remembered): the values that most often decide a print.
// It never blocks or delays sending. Hidden entirely when switched off in Preferences.
class ForcaBeforeYouPrintPanel : public wxPanel
{
public:
    // `wrap_width` in DIP.
    ForcaBeforeYouPrintPanel(wxWindow* parent, int wrap_width);

    // Rebuilds the contents for a plate (-1 = the current plate). `printer_key` names the printer as Forca Academy
    // records it (for a Bambu printer forca_ai_printer_label(dev_id)); empty = the selected physical printer, else the
    // printer preset. Returns false (and hides the panel) when the block is switched off; otherwise there is always
    // something to show, since Details are always offered.
    bool refresh(int plate_idx = -1, const std::string& printer_key = {});

    // Called after the panel changed size (Details opened or closed), so the dialog can re-fit.
    std::function<void()> on_layout_changed;

private:
    int         m_wrap_width;
    int         m_plate_idx = -1;
    std::string m_printer_key;
    wxBoxSizer* m_sizer = nullptr;
};

// A config option's label as the settings pages show it (translated); the key itself if unknown.
wxString forca_option_label(const std::string& key);
// A serialized option value as the settings pages show it: On/Off, an enum's label, a number with its unit.
wxString forca_option_value(const std::string& key, const std::string& value);

// App config "forca_before_you_print" (default on).
bool forca_before_you_print_enabled();

}} // namespace Slic3r::GUI
