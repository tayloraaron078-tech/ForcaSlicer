#include "ForcaPrintJournal.hpp"

#include "GUI.hpp" // from_u8 / into_u8 / from_path / into_path / show_error / desktop_open_any_folder
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"

#include <boost/filesystem/operations.hpp>
#include <nlohmann/json.hpp>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dnd.h>
#include <wx/filedlg.h>
#include <wx/image.h>
#include <wx/listctrl.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <cctype>
#include <ctime>

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;
using nlohmann::json;

namespace {

// The outcome choices, in the radio box's order (0 = not recorded). Keys are what record.json stores.
const char* const RESULT_KEYS[] = { "", "success", "partial", "failed" };

struct Tag
{
    const char* key;
    wxString    label;
};
std::vector<Tag> tags()
{
    return { { "stringing", _L("Stringing") },         { "warping", _L("Warping") },
             { "layer_shift", _L("Layer shift") },     { "spaghetti", _L("Spaghetti") },
             { "bed_adhesion", _L("Bed adhesion") },   { "supports", _L("Supports") },
             { "under_extrusion", _L("Under-extrusion") }, { "over_extrusion", _L("Over-extrusion") },
             { "ringing", _L("Ringing") },             { "layer_separation", _L("Layer separation") } };
}

wxString result_label(const std::string& key)
{
    if (key == "success") return _L("Success");
    if (key == "partial") return _L("Partial");
    if (key == "failed") return _L("Failed");
    return wxEmptyString;
}

// "2026-09-27T14:03:12" -> "2026-09-27 14:03"
wxString when(const std::string& iso) { return from_u8(iso.size() >= 16 ? iso.substr(0, 10) + " " + iso.substr(11, 5) : iso); }

std::string now_iso()
{
    std::time_t now = std::time(nullptr);
    std::tm     tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

bool is_media(const fs::path& p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* e : { ".jpg", ".jpeg", ".png", ".webp", ".heic", ".bmp", ".gif", ".tif", ".tiff", ".mp4", ".mov", ".avi" })
        if (ext == e)
            return true;
    return false;
}

class PhotoDropTarget : public wxFileDropTarget
{
public:
    explicit PhotoDropTarget(ForcaPrintJournalDialog* dlg) : m_dlg(dlg) {}
    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& names) override
    {
        std::vector<fs::path> files;
        for (const wxString& n : names)
            files.push_back(into_path(n));
        m_dlg->add_photos(files);
        return true;
    }

private:
    ForcaPrintJournalDialog* m_dlg;
};

} // namespace

ForcaPrintJournalDialog::ForcaPrintJournalDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _L("Print Journal"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetFont(Label::Body_14);
    const int pad  = FromDIP(10);
    auto*     root = new wxBoxSizer(wxVERTICAL);

    // Off: say so, and offer to turn it on (nothing is written until then).
    m_off_panel   = new wxPanel(this);
    auto* off_row = new wxBoxSizer(wxHORIZONTAL);
    auto* off_txt = new wxStaticText(m_off_panel, wxID_ANY,
        _L("The print journal is off, so Forca is not recording the prints it sends. Turn it on to keep a record of "
           "each print in the Forca Academy folder (you can turn it off again in Preferences)."));
    off_txt->Wrap(FromDIP(560));
    auto* turn_on = new wxButton(m_off_panel, wxID_ANY, _L("Turn on"));
    off_row->Add(off_txt, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, pad);
    off_row->Add(turn_on, 0, wxALIGN_CENTER_VERTICAL);
    m_off_panel->SetSizer(off_row);
    root->Add(m_off_panel, 0, wxEXPAND | wxALL, pad);
    turn_on->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxGetApp().app_config->set_bool("forca_academy_enabled", true);
        std::string err;
        if (!forca_academy_ensure_skeleton(forca_academy_dir(), err))
            show_error(this, from_u8(err));
        update_state();
    });

    auto* folder_row = new wxBoxSizer(wxHORIZONTAL);
    m_folder         = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(520), -1), wxST_ELLIPSIZE_MIDDLE);
    auto* open_btn   = new wxButton(this, wxID_ANY, _L("Open folder"));
    folder_row->Add(m_folder, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, pad);
    folder_row->Add(open_btn, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(folder_row, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    open_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        std::string err;
        if (forca_academy_ensure_skeleton(forca_academy_dir(), err))
            desktop_open_any_folder(forca_academy_dir().string());
        else
            show_error(this, from_u8(err));
    });

    auto* body = new wxBoxSizer(wxHORIZONTAL);
    m_list     = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(470), FromDIP(460)), wxLC_REPORT | wxLC_SINGLE_SEL);
    m_list->AppendColumn(_L("Date"), wxLIST_FORMAT_LEFT, FromDIP(120));
    m_list->AppendColumn(_L("Print"), wxLIST_FORMAT_LEFT, FromDIP(150));
    m_list->AppendColumn(_L("Printer"), wxLIST_FORMAT_LEFT, FromDIP(110));
    m_list->AppendColumn(_L("Result"), wxLIST_FORMAT_LEFT, FromDIP(80));
    body->Add(m_list, 1, wxEXPAND | wxRIGHT, pad);
    m_list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& e) {
        save_if_changed();
        show_print(e.GetIndex());
    });

    // The selected print.
    m_detail    = new wxPanel(this);
    auto* det   = new wxBoxSizer(wxVERTICAL);
    auto* head  = new wxBoxSizer(wxHORIZONTAL);
    m_thumb     = new wxStaticBitmap(m_detail, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(160), FromDIP(160)));
    auto* texts = new wxBoxSizer(wxVERTICAL);
    m_title     = new wxStaticText(m_detail, wxID_ANY, "");
    m_title->SetFont(Label::Head_14);
    m_info = new wxStaticText(m_detail, wxID_ANY, "");
    texts->Add(m_title, 0, wxBOTTOM, FromDIP(4));
    texts->Add(m_info, 0);
    head->Add(m_thumb, 0, wxRIGHT, pad);
    head->Add(texts, 1);
    det->Add(head, 0, wxEXPAND | wxBOTTOM, pad);

    const wxString results[] = { _L("Not recorded"), _L("Success"), _L("Partial"), _L("Failed") };
    m_result = new wxRadioBox(m_detail, wxID_ANY, _L("Result"), wxDefaultPosition, wxDefaultSize, 4, results, 4, wxRA_SPECIFY_COLS);
    det->Add(m_result, 0, wxEXPAND | wxBOTTOM, pad);
    m_result->Bind(wxEVT_RADIOBOX, [this](wxCommandEvent&) { if (!m_loading) m_dirty = true; });

    auto* tag_box = new wxStaticBoxSizer(wxVERTICAL, m_detail, _L("What went wrong (optional)"));
    auto* grid    = new wxGridSizer(2, FromDIP(4), FromDIP(12));
    for (const Tag& t : tags()) {
        auto* cb = new wxCheckBox(tag_box->GetStaticBox(), wxID_ANY, t.label);
        cb->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { if (!m_loading) m_dirty = true; });
        m_tags.push_back(cb);
        grid->Add(cb, 0);
    }
    tag_box->Add(grid, 0, wxALL, FromDIP(4));
    det->Add(tag_box, 0, wxEXPAND | wxBOTTOM, pad);

    det->Add(new wxStaticText(m_detail, wxID_ANY, _L("Note")), 0, wxBOTTOM, FromDIP(3));
    m_note = new wxTextCtrl(m_detail, wxID_ANY, "", wxDefaultPosition, wxSize(-1, FromDIP(70)), wxTE_MULTILINE);
    m_note->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { if (!m_loading) m_dirty = true; });
    det->Add(m_note, 0, wxEXPAND | wxBOTTOM, pad);

    auto* photo_row = new wxBoxSizer(wxHORIZONTAL);
    m_photos        = new wxStaticText(m_detail, wxID_ANY, "");
    auto* add_photo = new wxButton(m_detail, wxID_ANY, _L("Add photos") + dots);
    add_photo->SetToolTip(_L("Copy photos or videos of this print into its folder. You can also drop them on this window."));
    auto* open_print = new wxButton(m_detail, wxID_ANY, _L("Open print folder"));
    photo_row->Add(m_photos, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, pad);
    photo_row->Add(add_photo, 0, wxRIGHT, FromDIP(6));
    photo_row->Add(open_print, 0);
    det->Add(photo_row, 0, wxEXPAND | wxBOTTOM, pad);
    add_photo->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxFileDialog dlg(this, _L("Add photos"), "", "",
                         _L("Photos and videos") + " (*.jpg;*.jpeg;*.png;*.webp;*.heic;*.bmp;*.gif;*.tif;*.tiff;*.mp4;*.mov;*.avi)|"
                         "*.jpg;*.jpeg;*.png;*.webp;*.heic;*.bmp;*.gif;*.tif;*.tiff;*.mp4;*.mov;*.avi;*.JPG;*.JPEG;*.PNG",
                         wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK)
            return;
        wxArrayString paths;
        dlg.GetPaths(paths);
        std::vector<fs::path> files;
        for (const wxString& p : paths)
            files.push_back(into_path(p));
        add_photos(files);
    });
    open_print->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_row >= 0 && size_t(m_row) < m_prints.size())
            desktop_open_any_folder(m_prints[m_row].folder.string());
    });

    m_save = new wxButton(m_detail, wxID_ANY, _L("Save"));
    det->Add(m_save, 0, wxALIGN_RIGHT);
    m_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { save(); });

    m_detail->SetSizer(det);
    body->Add(m_detail, 0, wxEXPAND);
    root->Add(body, 1, wxEXPAND | wxALL, pad);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    m_add_print   = new wxButton(this, wxID_ANY, _L("Add a print"));
    m_add_print->SetToolTip(_L("Record the plate as it is now as a print -- for prints Forca did not send, such as one "
                               "started from an SD card."));
    auto* close = new wxButton(this, wxID_CLOSE, _L("Close"));
    buttons->Add(m_add_print, 0);
    buttons->AddStretchSpacer(1);
    buttons->Add(close, 0);
    root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);
    m_add_print->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        save_if_changed();
        std::string    err;
        const fs::path folder = forca_academy_record_current_plate(err);
        if (folder.empty()) {
            show_error(this, from_u8(err));
            return;
        }
        reload(folder);
    });
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
        save_if_changed();
        e.Skip();
    });

    SetDropTarget(new PhotoDropTarget(this));
    SetSizerAndFit(root);
    update_state();
    wxGetApp().UpdateDlgDarkUI(this);
    CenterOnParent();
}

void ForcaPrintJournalDialog::update_state()
{
    const bool on = forca_academy_enabled();
    m_off_panel->Show(!on);
    m_add_print->Enable(on);
    m_folder->SetLabel(_L("Folder:") + " " + from_path(forca_academy_dir()));
    m_folder->SetToolTip(from_path(forca_academy_dir()));
    reload();
    Layout();
}

void ForcaPrintJournalDialog::reload(const fs::path& select)
{
    m_prints = forca_academy_list_prints(forca_academy_dir());
    m_list->DeleteAllItems();
    long sel = -1;
    for (size_t i = 0; i < m_prints.size(); ++i) {
        const ForcaAcademyPrint& p   = m_prints[i];
        const long               row = m_list->InsertItem(long(i), when(p.time));
        m_list->SetItem(row, 1, from_u8(p.title));
        m_list->SetItem(row, 2, from_u8(p.printer));
        m_list->SetItem(row, 3, result_label(p.result));
        if (!select.empty() && p.folder == select)
            sel = row;
    }
    m_row = -1;
    if (m_prints.empty()) {
        show_print(-1);
        return;
    }
    if (sel < 0)
        sel = 0;
    m_list->SetItemState(sel, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    m_list->EnsureVisible(sel);
    show_print(sel); // SetItemState's selection event may already have shown it; showing again is harmless
}

void ForcaPrintJournalDialog::show_print(long row)
{
    m_loading = true;
    m_row     = (row >= 0 && size_t(row) < m_prints.size()) ? row : -1;
    m_dirty   = false;
    json r;
    const bool have = m_row >= 0 && forca_academy_read_record(m_prints[m_row].folder, r);
    m_detail->Enable(have);

    wxBitmap bmp;
    if (have) {
        const fs::path png = m_prints[m_row].folder / "thumbnail.png";
        wxImage        img;
        if (fs::exists(png) && img.LoadFile(from_path(png), wxBITMAP_TYPE_PNG))
            bmp = wxBitmap(img.Scale(FromDIP(160), FromDIP(160), wxIMAGE_QUALITY_HIGH));
    }
    m_thumb->SetBitmap(bmp);
    m_title->SetLabel(have ? from_u8(r.value("title", std::string())) : _L("No prints recorded yet"));

    wxString info;
    if (have) {
        info = when(r.value("time", std::string()));
        if (!m_prints[m_row].printer.empty())
            info += "\n" + from_u8(m_prints[m_row].printer);
        if (r.contains("process") && r["process"].is_string())
            info += "\n" + from_u8(r["process"].get<std::string>());
        if (r.contains("filaments") && r["filaments"].is_array()) {
            bool any_used = false;
            for (const json& f : r["filaments"])
                any_used |= f.contains("used_g");
            for (const json& f : r["filaments"]) {
                if (any_used && !f.contains("used_g"))
                    continue;
                info += "\n" + wxString::Format(_L("Filament %d: %s"), f.value("slot", 0), from_u8(f.value("preset", std::string())));
                if (f.contains("used_g"))
                    info += wxString::Format(" (%.1f g)", f.value("used_g", 0.0));
                if (f.contains("printed_from") && f["printed_from"].is_object())
                    info += " -- " + from_u8(f["printed_from"].value("tray", std::string()));
            }
        }
        if (r.contains("print_end") && r["print_end"].is_object()) {
            const std::string st  = r["print_end"].value("state", std::string());
            const wxString    how = st == "finished" ? _L("Printer reported: finished") :
                                    st == "failed"   ? _L("Printer reported: failed") :
                                    st == "stopped"  ? _L("Printer reported: stopped") :
                                                       _L("Printer reported: ended (not seen)");
            info += "\n" + how;
            if (r["print_end"].contains("time_from_send_s"))
                info += " " + wxString::Format(_L("(%d min after sending)"), int(r["print_end"].value("time_from_send_s", 0LL) / 60));
        }
        if (r.contains("estimate") && r["estimate"].is_object() && r["estimate"].contains("print_time"))
            info += "\n" + wxString::Format(_L("Estimated time: %s"), from_u8(r["estimate"].value("print_time", std::string())));
    }
    m_info->SetLabel(info);

    const json outcome = have && r.contains("outcome") && r["outcome"].is_object() ? r["outcome"] : json::object();
    const std::string result = outcome.value("result", std::string());
    int               sel    = 0;
    for (int i = 1; i < 4; ++i)
        if (result == RESULT_KEYS[i])
            sel = i;
    m_result->SetSelection(sel);
    const std::vector<Tag> all = tags();
    const json             set = outcome.value("tags", json::array());
    for (size_t i = 0; i < m_tags.size(); ++i)
        m_tags[i]->SetValue(std::find(set.begin(), set.end(), json(all[i].key)) != set.end());
    m_note->ChangeValue(from_u8(outcome.value("note", std::string())));
    update_photos();
    m_loading = false;
    Layout();
}

void ForcaPrintJournalDialog::update_photos()
{
    size_t n = 0;
    if (m_row >= 0) {
        boost::system::error_code ec;
        const fs::path            dir = m_prints[m_row].folder / "photos";
        if (fs::is_directory(dir, ec))
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
                n += fs::is_regular_file(it->path(), ec) ? 1 : 0;
    }
    m_photos->SetLabel(wxString::Format(_L("Photos: %d"), int(n)));
}

void ForcaPrintJournalDialog::save_if_changed()
{
    if (m_dirty)
        save();
}

void ForcaPrintJournalDialog::save()
{
    if (m_row < 0 || size_t(m_row) >= m_prints.size())
        return;
    json                   tag_list = json::array();
    const std::vector<Tag> all      = tags();
    for (size_t i = 0; i < m_tags.size(); ++i)
        if (m_tags[i]->GetValue())
            tag_list.push_back(all[i].key);
    const int   sel     = m_result->GetSelection();
    const json  outcome = { { "result", RESULT_KEYS[sel >= 0 && sel < 4 ? sel : 0] },
                            { "tags", tag_list },
                            { "note", into_u8(m_note->GetValue()) },
                            { "recorded", now_iso() },
                            { "by", "user" } }; // an AI never overwrites an outcome the user recorded
    std::string err;
    if (!forca_academy_set_outcome(m_prints[m_row].folder, outcome, err)) {
        show_error(this, from_u8(err));
        return;
    }
    m_dirty                  = false;
    m_prints[m_row].result   = outcome["result"].get<std::string>();
    m_list->SetItem(m_row, 3, result_label(m_prints[m_row].result));
}

void ForcaPrintJournalDialog::add_photos(const std::vector<fs::path>& files)
{
    if (m_row < 0 || size_t(m_row) >= m_prints.size()) {
        show_info(this, _L("Pick a print in the list first, then add its photos."), _L("Print Journal"));
        return;
    }
    std::vector<fs::path> media;
    for (const fs::path& f : files)
        if (is_media(f))
            media.push_back(f);
    if (media.empty())
        return;
    std::string err;
    forca_academy_add_photos(m_prints[m_row].folder, media, err);
    if (!err.empty())
        show_error(this, from_u8(err));
    update_photos();
}

}} // namespace Slic3r::GUI
