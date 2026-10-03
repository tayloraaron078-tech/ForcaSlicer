#pragma once

#include "ForcaAcademy.hpp"

#include <wx/dialog.h>

#include <vector>

class wxButton;
class wxCheckBox;
class wxListCtrl;
class wxListEvent;
class wxPanel;
class wxRadioBox;
class wxStaticBitmap;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r { namespace GUI {

// Forca Academy's Print Journal (HQ PLAN_forca_academy.md §4): the recorded prints, newest first; for the selected
// one its picture and details, the outcome (result, quick tags, a note) and its photos. "Add a print" records the
// current plate for prints Forca did not send (e.g. from an SD card). When the journal is off it offers to turn it on.
// Changes to a print are saved when you press Save, pick another print or close the window.
class ForcaPrintJournalDialog : public wxDialog
{
public:
    explicit ForcaPrintJournalDialog(wxWindow* parent);

    void add_photos(const std::vector<boost::filesystem::path>& files); // to the selected print (also drag & drop)

private:
    void reload(const boost::filesystem::path& select = {});
    void show_print(long row);
    void save_if_changed();
    void save();
    void update_photos();
    void update_state();

    std::vector<ForcaAcademyPrint> m_prints;
    long                           m_row     = -1;
    bool                           m_dirty   = false;
    bool                           m_loading = false;

    wxPanel*                 m_off_panel  = nullptr;
    wxStaticText*            m_folder     = nullptr;
    wxListCtrl*              m_list       = nullptr;
    wxPanel*                 m_detail     = nullptr;
    wxStaticBitmap*          m_thumb      = nullptr;
    wxStaticText*            m_title      = nullptr;
    wxStaticText*            m_info       = nullptr;
    wxRadioBox*              m_result     = nullptr;
    std::vector<wxCheckBox*> m_tags;
    wxTextCtrl*              m_note       = nullptr;
    wxStaticText*            m_photos     = nullptr;
    wxButton*                m_save       = nullptr;
    wxButton*                m_add_print  = nullptr;
};

}} // namespace Slic3r::GUI
