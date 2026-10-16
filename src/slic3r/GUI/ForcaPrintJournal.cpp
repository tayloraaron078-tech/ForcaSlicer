#include "ForcaPrintJournal.hpp"

#include "GUI.hpp" // from_u8 / into_u8 / from_path / into_path / show_error / desktop_open_any_folder
#include "ForcaBeforeYouPrint.hpp" // forca_option_label / forca_option_value
#include "ForcaReset.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "format.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/SpinInput.hpp"
#include "libslic3r/AppConfig.hpp"

#include <boost/filesystem/operations.hpp>
#include <nlohmann/json.hpp>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dnd.h>
#include <wx/filedlg.h>
#include <wx/image.h>
#include <wx/listctrl.h>
#include <wx/menu.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/utils.h>

#include <algorithm>
#include <utility>
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

wxString forca_journal_tag_label(const std::string& key)
{
    for (const Tag& t : tags())
        if (key == t.key)
            return t.label;
    return from_u8(key);
}

namespace {

// Forca Academy part 2 (HQ PLAN_forca_academy_part2.md): two prints side by side -- the records' facts, then every
// setting that differed (from settings.json; for prints recorded before it existed, the settings each print had
// changed from its presets). Rows that differ are bold. "Save as experiment" writes experiments/<date>_<name>.md.
class ForcaCompareDialog : public wxDialog
{
public:
    ForcaCompareDialog(wxWindow* parent, const ForcaAcademyPrint& a, const ForcaAcademyPrint& b)
        : wxDialog(parent, wxID_ANY, _L("Compare two prints"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_a(a)
        , m_b(b)
    {
        SetFont(Label::Body_14);
        const int pad = FromDIP(10);
        json      ra, rb, sa, sb;
        forca_academy_read_record(a.folder, ra);
        forca_academy_read_record(b.folder, rb);
        forca_academy_read_settings(a.folder, sa);
        forca_academy_read_settings(b.folder, sb);
        m_cmp = forca_academy_compare(ra, sa, rb, sb);

        auto* root = new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(this, wxID_ANY, "A: " + from_u8(a.title) + "  (" + when(a.time) + ")\nB: " + from_u8(b.title) + "  (" +
                                                       when(b.time) + ")"),
                  0, wxALL, pad);

        m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(780), FromDIP(400)), wxLC_REPORT | wxLC_SINGLE_SEL);
        m_list->AppendColumn(_L("Setting"), wxLIST_FORMAT_LEFT, FromDIP(280));
        m_list->AppendColumn("A", wxLIST_FORMAT_LEFT, FromDIP(240));
        m_list->AppendColumn("B", wxLIST_FORMAT_LEFT, FromDIP(240));
        wxFont bold = m_list->GetFont().Bold();
        size_t settings = 0;
        for (const ForcaAcademyDiff& d : m_cmp.rows) {
            const long r = m_list->InsertItem(m_list->GetItemCount(), label(d));
            m_list->SetItem(r, 1, value(d, d.a));
            m_list->SetItem(r, 2, value(d, d.b));
            if (d.differs)
                m_list->SetItemFont(r, bold);
            settings += d.section != "print";
        }
        root->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, pad);

        wxString note = m_cmp.full ? _L("Bold rows differ. Every setting that differed between the two prints is listed.")
                                   : _L("At least one of these prints was recorded before Forca kept all of its settings, so only "
                                        "the settings each print had changed from its presets are compared (\"preset value\": "
                                        "that print used its preset's value).");
        if (settings == 0)
            note += "\n" + _L("No setting differences were recorded.");
        auto* note_txt = new wxStaticText(this, wxID_ANY, note);
        note_txt->Wrap(FromDIP(760));
        root->Add(note_txt, 0, wxALL, pad);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* save    = new wxButton(this, wxID_ANY, _L("Save as experiment") + dots);
        save->SetToolTip(_L("Write a page in the Academy's experiments folder with both prints and what differed, and add the "
                            "result once you know which one came out better."));
        auto* close = new wxButton(this, wxID_CLOSE, _L("Close"));
        buttons->Add(save, 0);
        buttons->AddStretchSpacer(1);
        buttons->Add(close, 0);
        root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);
        save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { save_experiment(); });
        close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); });

        SetSizerAndFit(root);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

private:
    wxString label(const ForcaAcademyDiff& d) const
    {
        if (d.section == "print") {
            if (d.key == "printer") return _L("Printer");
            if (d.key == "process") return _L("Process");
            if (d.key == "filaments") return _L("Filaments");
            if (d.key == "print_time") return _L("Print time (estimate)");
            if (d.key == "filament_g") return _L("Filament used (estimate)");
            if (d.key == "result") return _L("Result");
            if (d.key == "tags") return _L("What went wrong");
            return from_u8(d.key);
        }
        const wxString section = d.section == "process" ? _L("Process") : d.section == "filament" ? _L("Filament") : _L("Printer");
        return forca_option_label(d.key) + " (" + section + ")";
    }

    wxString value(const ForcaAcademyDiff& d, const std::string& v) const
    {
        if (d.section == "print") {
            if (d.key == "result")
                return v.empty() ? _L("Not recorded") : result_label(v);
            if (d.key == "tags") {
                wxString out;
                for (size_t start = 0; start < v.size();) {
                    size_t end = v.find(", ", start);
                    if (end == std::string::npos)
                        end = v.size();
                    out += (out.empty() ? "" : ", ") + forca_journal_tag_label(v.substr(start, end - start));
                    start = end + 2;
                }
                return out;
            }
            return from_u8(v);
        }
        if (v.empty())
            return m_cmp.full ? wxString("-") : _L("preset value");
        return forca_option_value(d.key, v);
    }

    void save_experiment()
    {
        wxTextEntryDialog dlg(this, _L("What are you testing? This names the experiment."), _L("Save as experiment"),
                              from_u8(m_a.title == m_b.title ? m_a.title : m_a.title + " vs " + m_b.title));
        if (dlg.ShowModal() != wxID_OK)
            return;
        wxString name = dlg.GetValue();
        name.Trim().Trim(false);
        if (name.empty())
            return;
        // What differed in the settings (and in printer / process / filament presets); the outcome is in the Prints list.
        std::vector<std::string> lines;
        for (const ForcaAcademyDiff& d : m_cmp.rows)
            if (d.differs && (d.section != "print" || d.key == "printer" || d.key == "process" || d.key == "filaments"))
                lines.push_back(into_u8(label(d) + ": A " + value(d, d.a) + ", B " + value(d, d.b)));
        const fs::path dir = forca_academy_dir();
        std::string    err;
        const fs::path file = forca_academy_write_experiment(dir, into_u8(name), now_iso().substr(0, 10), m_a, m_b, lines, err);
        if (file.empty()) {
            show_error(this, from_u8(err));
            return;
        }
        MessageDialog msg(this,
                          format_wxstr(_L("Saved as %1%. Write the result there once you know which print came out better."),
                                       from_u8(forca_academy_relative(dir, file))),
                          _L("Save as experiment"), wxYES_NO | wxICON_INFORMATION);
        msg.SetButtonLabel(wxID_YES, _L("Open"));
        msg.SetButtonLabel(wxID_NO, _L("Close"));
        if (msg.ShowModal() == wxID_YES)
            wxLaunchDefaultApplication(from_path(file));
    }

    ForcaAcademyPrint      m_a, m_b;
    ForcaAcademyComparison m_cmp;
    wxListCtrl*            m_list = nullptr;
};

// Forca Academy part 2: turn a possible pattern into a lessons.md entry. Forca proposes the text; the user edits it,
// picks how sure they are, and saves (then the pattern stops being shown).
class ForcaLessonDialog : public wxDialog
{
public:
    ForcaLessonDialog(wxWindow* parent, const ForcaAcademyPattern& p)
        : wxDialog(parent, wxID_ANY, _L("Write a lesson"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_pattern(p)
    {
        SetFont(Label::Body_14);
        const int pad  = FromDIP(10);
        auto*     root = new wxBoxSizer(wxVERTICAL);
        auto*     intro = new wxStaticText(this, wxID_ANY,
            _L("Forca drafted this from the journal. Write the rule you take from these prints (what to change or check "
               "next time) and say how sure you are. It is added to the end of lessons.md."));
        intro->Wrap(FromDIP(640));
        root->Add(intro, 0, wxALL, pad);

        const wxString levels[] = { _L("Low"), _L("Medium"), _L("High") };
        m_confidence = new wxRadioBox(this, wxID_ANY, _L("Confidence"), wxDefaultPosition, wxDefaultSize, 3, levels, 3, wxRA_SPECIFY_COLS);
        m_confidence->SetSelection(1);
        root->Add(m_confidence, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);

        m_text = new wxTextCtrl(this, wxID_ANY, from_u8(forca_academy_lesson_draft(forca_academy_dir(), p, "medium", now_iso().substr(0, 10))),
                                wxDefaultPosition, wxSize(FromDIP(660), FromDIP(220)), wxTE_MULTILINE);
        root->Add(m_text, 1, wxEXPAND | wxLEFT | wxRIGHT, pad);
        // The confidence choice rewrites the draft's "- Confidence:" line (the file is in English, like the rest).
        m_confidence->Bind(wxEVT_RADIOBOX, [this](wxCommandEvent&) {
            const char* const words[] = { "low", "medium", "high" };
            wxString          text    = m_text->GetValue();
            const int         at      = text.Find("- Confidence: ");
            if (at == wxNOT_FOUND)
                return;
            const size_t start = size_t(at) + wxString("- Confidence: ").length();
            size_t       end   = text.find('\n', start);
            if (end == wxString::npos)
                end = text.length();
            text.replace(start, end - start, words[std::clamp(m_confidence->GetSelection(), 0, 2)]);
            m_text->SetValue(text);
        });

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* save    = new wxButton(this, wxID_OK, _L("Add to lessons.md"));
        auto* cancel  = new wxButton(this, wxID_CANCEL, _L("Cancel"));
        buttons->AddStretchSpacer(1);
        buttons->Add(save, 0, wxRIGHT, FromDIP(6));
        buttons->Add(cancel, 0);
        root->Add(buttons, 0, wxEXPAND | wxALL, pad);
        save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            const fs::path dir = forca_academy_dir();
            std::string    err;
            if (!forca_academy_append_lesson(dir, into_u8(m_text->GetValue()), err) || !forca_academy_dismiss(dir, m_pattern.id, err)) {
                show_error(this, from_u8(err));
                return;
            }
            forca_academy_refresh_index();
            EndModal(wxID_OK);
        });

        SetSizerAndFit(root);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

private:
    ForcaAcademyPattern m_pattern;
    wxRadioBox*         m_confidence = nullptr;
    wxTextCtrl*         m_text       = nullptr;
};

// The Insights column "Most common problem": the tag, and how many of its prints were otherwise successful.
wxString most_common_problem(const ForcaAcademyHistory& h)
{
    if (h.tags.empty())
        return {};
    const auto& [tag, count] = h.tags.front();
    const auto  ok           = h.tags_on_success.find(tag);
    if (ok == h.tags_on_success.end())
        return forca_journal_tag_label(tag);
    // TRN Insights, "Most common problem": %1% the problem, %2% prints with it, %3% of those otherwise successful.
    // Worded as labels so it needs no plural forms.
    return format_wxstr(_L("%1% (prints: %2%, successful: %3%)"), forca_journal_tag_label(tag), count, ok->second);
}

// Forca Academy part 2 (HQ PLAN_forca_academy_part2.md): how the recorded prints went, grouped by printer, filament
// preset or process preset (double-click opens the journal on that group's prints); possible patterns (the same
// problem on several recent prints with one printer and filament: show the prints, write a lesson, or dismiss); and
// the experiments saved from "Compare".
class ForcaInsightsDialog : public wxDialog
{
public:
    explicit ForcaInsightsDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, _L("Print Journal Insights"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        SetFont(Label::Body_14);
        const int pad  = FromDIP(10);
        auto*     root = new wxBoxSizer(wxVERTICAL);

        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(this, wxID_ANY, _L("Group by")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, pad);
        m_group = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(160), -1), 0, nullptr, wxCB_READONLY);
        m_group->Append(_L("Printer"));
        m_group->Append(_L("Filament"));
        m_group->Append(_L("Process"));
        m_group->SetSelection(1);
        m_group->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { reload_groups(); });
        row->Add(m_group, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(row, 0, wxALL, pad);

        m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(820), FromDIP(220)), wxLC_REPORT | wxLC_SINGLE_SEL);
        const std::pair<wxString, int> columns[] = { { _L("Name"), 220 },       { _L("Prints"), 60 },  { _L("Successful"), 80 },
                                                     { _L("Partial"), 60 },     { _L("Failed"), 60 },  { _L("Not recorded"), 90 },
                                                     { _L("Most common problem"), 150 }, { _L("Last failure"), 100 } };
        for (const auto& [title, width] : columns)
            m_list->AppendColumn(title, wxLIST_FORMAT_LEFT, FromDIP(width));
        m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& e) { open_group(e.GetIndex()); });
        root->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, pad);

        auto* note = new wxStaticText(this, wxID_ANY,
            _L("Only prints with a recorded result count as successful, partial or failed. Double-click a row to see its prints."));
        note->Wrap(FromDIP(800));
        root->Add(note, 0, wxALL, pad);

        // Possible patterns, with their rule (also in Preferences).
        auto* pat_title = new wxStaticText(this, wxID_ANY, _L("Possible patterns"));
        pat_title->SetFont(Label::Head_14);
        root->Add(pat_title, 0, wxLEFT | wxRIGHT, pad);
        const auto [count, window] = forca_academy_pattern_rule();
        // The same two settings (and labels) as in Preferences.
        auto* rule = new wxBoxSizer(wxHORIZONTAL);
        auto  add_rule_text = [this, rule](const wxString& text, const wxString& tip) {
            auto* t = new wxStaticText(this, wxID_ANY, text);
            t->SetToolTip(tip);
            rule->Add(t, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        };
        const wxString count_tip  = _L("The journal's Insights point out a possible pattern when the same problem tag is on this "
                                       "many of the recent prints with one printer and filament.");
        const wxString window_tip = _L("How many of the latest prints with an outcome, per printer and filament, the pattern rule "
                                       "looks at.");
        add_rule_text(_L("Possible pattern after"), count_tip);
        m_count = new SpinInput(this, wxEmptyString, _L("prints"), wxDefaultPosition, wxSize(FromDIP(110), -1), wxSP_ARROW_KEYS, 2, 50, count);
        m_count->SetToolTip(count_tip);
        rule->Add(m_count, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(20));
        add_rule_text(_L("Recent prints to look at"), window_tip);
        m_window = new SpinInput(this, wxEmptyString, _L("prints"), wxDefaultPosition, wxSize(FromDIP(110), -1), wxSP_ARROW_KEYS, 2, 100, window);
        m_window->SetToolTip(window_tip);
        rule->Add(m_window, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(rule, 0, wxALL, pad);
        for (SpinInput* s : { m_count, m_window }) {
            s->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent& e) { rule_changed(); e.Skip(); });
            s->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent& e) { rule_changed(); e.Skip(); });
        }

        m_patterns = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(820), FromDIP(130)), wxLC_REPORT | wxLC_SINGLE_SEL);
        const std::pair<wxString, int> pat_columns[] = { { _L("Problem"), 170 }, { _L("Filament"), 240 }, { _L("Printer"), 220 }, { _L("How often"), 150 } };
        for (const auto& [title, width] : pat_columns)
            m_patterns->AppendColumn(title, wxLIST_FORMAT_LEFT, FromDIP(width));
        root->Add(m_patterns, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);

        auto* pat_buttons = new wxBoxSizer(wxHORIZONTAL);
        m_show_prints     = new wxButton(this, wxID_ANY, _L("Show prints"));
        m_write_lesson    = new wxButton(this, wxID_ANY, _L("Write a lesson") + dots);
        m_dismiss         = new wxButton(this, wxID_ANY, _L("Don't show again"));
        m_dismiss->SetToolTip(_L("Stop showing this pattern. Forca remembers it in the Academy folder."));
        pat_buttons->Add(m_show_prints, 0, wxRIGHT, FromDIP(6));
        pat_buttons->Add(m_write_lesson, 0, wxRIGHT, FromDIP(6));
        pat_buttons->Add(m_dismiss, 0);
        root->Add(pat_buttons, 0, wxALL, pad);
        m_patterns->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { update_pattern_buttons(); });
        m_patterns->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) { update_pattern_buttons(); });
        m_patterns->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { show_pattern_prints(); });
        m_show_prints->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_pattern_prints(); });
        m_write_lesson->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (const ForcaAcademyPattern* p = selected_pattern()) {
                ForcaLessonDialog dlg(this, *p);
                if (dlg.ShowModal() == wxID_OK)
                    reload_patterns();
            }
        });
        m_dismiss->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (const ForcaAcademyPattern* p = selected_pattern()) {
                std::string err;
                if (!forca_academy_dismiss(forca_academy_dir(), p->id, err))
                    show_error(this, from_u8(err));
                forca_academy_refresh_index();
                reload_patterns();
            }
        });

        // Experiments saved from "Compare".
        auto* exp_title = new wxStaticText(this, wxID_ANY, _L("Experiments"));
        exp_title->SetFont(Label::Head_14);
        root->Add(exp_title, 0, wxLEFT | wxRIGHT, pad);
        m_experiments = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(820), FromDIP(110)), wxLC_REPORT | wxLC_SINGLE_SEL);
        m_experiments->AppendColumn(_L("Date"), wxLIST_FORMAT_LEFT, FromDIP(110));
        m_experiments->AppendColumn(_L("Experiment"), wxLIST_FORMAT_LEFT, FromDIP(520));
        m_experiments->AppendColumn(_L("Result"), wxLIST_FORMAT_LEFT, FromDIP(150));
        m_experiments->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& e) {
            if (e.GetIndex() >= 0 && size_t(e.GetIndex()) < m_exps.size())
                wxLaunchDefaultApplication(from_path(m_exps[size_t(e.GetIndex())].file));
        });
        root->Add(m_experiments, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, pad);
        auto* exp_note = new wxStaticText(this, wxID_ANY,
            _L("Select two prints in the journal and press Compare to start one. Double-click an experiment to open it."));
        exp_note->Wrap(FromDIP(800));
        root->Add(exp_note, 0, wxALL, pad);

        SetSizerAndFit(root);
        CenterOnParent();
        reload();
        wxGetApp().UpdateDlgDarkUI(this);
    }

private:
    void reload()
    {
        m_prints = forca_academy_list_prints(forca_academy_dir());
        reload_groups();
        reload_patterns();
        m_exps = forca_academy_list_experiments(forca_academy_dir());
        m_experiments->DeleteAllItems();
        for (size_t i = 0; i < m_exps.size(); ++i) {
            const long r = m_experiments->InsertItem(long(i), from_u8(m_exps[i].date));
            m_experiments->SetItem(r, 1, from_u8(m_exps[i].title));
            m_experiments->SetItem(r, 2, m_exps[i].has_result ? _L("Written") : _L("Not yet"));
        }
    }

    void reload_groups()
    {
        const int sel = m_group->GetSelection();
        const ForcaAcademyGroup by = sel == 0 ? ForcaAcademyGroup::Printer : sel == 2 ? ForcaAcademyGroup::Process : ForcaAcademyGroup::Filament;
        m_rows = forca_academy_insights(m_prints, by);
        m_list->DeleteAllItems();
        for (size_t i = 0; i < m_rows.size(); ++i) {
            const ForcaAcademyInsight& g = m_rows[i];
            const long                 r = m_list->InsertItem(long(i), from_u8(g.name));
            m_list->SetItem(r, 1, wxString::Format("%zu", g.history.folders.size()));
            m_list->SetItem(r, 2, wxString::Format("%d", g.history.success));
            m_list->SetItem(r, 3, wxString::Format("%d", g.history.partial));
            m_list->SetItem(r, 4, wxString::Format("%d", g.history.failed));
            m_list->SetItem(r, 5, wxString::Format("%d", g.history.not_recorded));
            m_list->SetItem(r, 6, most_common_problem(g.history));
            m_list->SetItem(r, 7, g.last_failure.empty() ? wxString() : when(g.last_failure));
        }
    }

    void reload_patterns()
    {
        m_pats = forca_academy_open_patterns(m_prints);
        m_patterns->DeleteAllItems();
        for (size_t i = 0; i < m_pats.size(); ++i) {
            const ForcaAcademyPattern& p = m_pats[i];
            const long                 r = m_patterns->InsertItem(long(i), forca_journal_tag_label(p.tag));
            m_patterns->SetItem(r, 1, from_u8(p.filament));
            m_patterns->SetItem(r, 2, from_u8(p.printer));
            m_patterns->SetItem(r, 3, p.on_success > 0 ?
                // TRN Possible patterns, "How often": the problem was on %1% of the last %2% prints; %3% of those were
                // otherwise successful. Worded as a label so it needs no plural forms.
                format_wxstr(_L("%1% of the last %2% (successful: %3%)"), p.count, p.window, p.on_success) :
                format_wxstr(_L("%1% of the last %2%"), p.count, p.window));
        }
        if (m_pats.empty())
            m_patterns->InsertItem(0, _L("None at the moment."));
        update_pattern_buttons();
    }

    void rule_changed()
    {
        const int count = m_count->GetValue(), window = m_window->GetValue();
        AppConfig* cfg = wxGetApp().app_config;
        if (std::to_string(count) == cfg->get("forca_academy_pattern_count") && std::to_string(window) == cfg->get("forca_academy_pattern_window"))
            return;
        cfg->set("forca_academy_pattern_count", std::to_string(count));
        cfg->set("forca_academy_pattern_window", std::to_string(window));
        forca_academy_refresh_index();
        reload_patterns();
    }

    const ForcaAcademyPattern* selected_pattern() const
    {
        const long r = m_patterns->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        return r >= 0 && size_t(r) < m_pats.size() ? &m_pats[size_t(r)] : nullptr;
    }

    void update_pattern_buttons()
    {
        const bool on = selected_pattern() != nullptr;
        m_show_prints->Enable(on);
        m_write_lesson->Enable(on);
        m_dismiss->Enable(on);
    }

    void show_pattern_prints()
    {
        const ForcaAcademyPattern* p = selected_pattern();
        if (p == nullptr)
            return;
        {
            ForcaPrintJournalDialog dlg(this, p->folders,
                                        format_wxstr(_L("%1% with %2% on %3%"), forca_journal_tag_label(p->tag), from_u8(p->filament),
                                                     from_u8(p->printer)));
            dlg.ShowModal();
        }
        reload(); // outcomes may have changed in the journal
    }

    void open_group(long row)
    {
        if (row < 0 || size_t(row) >= m_rows.size())
            return;
        {
            ForcaPrintJournalDialog dlg(this, m_rows[size_t(row)].history.folders, from_u8(m_rows[size_t(row)].name));
            dlg.ShowModal();
        }
        reload(); // outcomes may have changed in the journal
    }

    ::ComboBox*                         m_group        = nullptr;
    wxListCtrl*                         m_list         = nullptr;
    SpinInput*                          m_count        = nullptr;
    SpinInput*                          m_window       = nullptr;
    wxListCtrl*                         m_patterns     = nullptr;
    wxButton*                           m_show_prints  = nullptr;
    wxButton*                           m_write_lesson = nullptr;
    wxButton*                           m_dismiss      = nullptr;
    wxListCtrl*                         m_experiments  = nullptr;
    std::vector<ForcaAcademyPrint>      m_prints;
    std::vector<ForcaAcademyInsight>    m_rows;
    std::vector<ForcaAcademyPattern>    m_pats;
    std::vector<ForcaAcademyExperiment> m_exps;
};

} // namespace

ForcaPrintJournalDialog::ForcaPrintJournalDialog(wxWindow* parent) : ForcaPrintJournalDialog(parent, {}, {}) {}

ForcaPrintJournalDialog::ForcaPrintJournalDialog(wxWindow* parent, std::vector<fs::path> only, const wxString& filter_label)
    : wxDialog(parent, wxID_ANY, _L("Print Journal"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_only(std::move(only))
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
    // Forca: Insights (how the prints went, per printer / filament / process). Not offered on a filtered journal.
    if (m_only.empty()) {
        auto* insights_btn = new wxButton(this, wxID_ANY, _L("Insights"));
        folder_row->Add(insights_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, pad);
        insights_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            save_if_changed();
            {
                ForcaInsightsDialog dlg(this);
                dlg.ShowModal();
            }
            reload(); // outcomes may have changed in a journal opened from Insights
        });
    }
    root->Add(folder_row, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);

    // Forca: opened on a subset of the prints: say which, and offer the whole journal.
    if (!m_only.empty()) {
        auto* filter_row = new wxBoxSizer(wxHORIZONTAL);
        auto* filter_txt = new wxStaticText(this, wxID_ANY, filter_label);
        auto* show_all   = new wxButton(this, wxID_ANY, _L("Show all"));
        filter_row->Add(filter_txt, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, pad);
        filter_row->Add(show_all, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(filter_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
        show_all->Bind(wxEVT_BUTTON, [this, filter_txt, show_all](wxCommandEvent&) {
            save_if_changed();
            m_only.clear();
            filter_txt->Hide();
            show_all->Hide();
            reload();
            Layout();
        });
    }
    open_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        std::string err;
        if (forca_academy_ensure_skeleton(forca_academy_dir(), err))
            desktop_open_any_folder(forca_academy_dir().string());
        else
            show_error(this, from_u8(err));
    });

    auto* body = new wxBoxSizer(wxHORIZONTAL);
    // Forca: several prints can be selected (Ctrl+click) so two can be compared; the details show the last one clicked.
    m_list     = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(470), FromDIP(460)), wxLC_REPORT);
    m_list->AppendColumn(_L("Date"), wxLIST_FORMAT_LEFT, FromDIP(120));
    m_list->AppendColumn(_L("Print"), wxLIST_FORMAT_LEFT, FromDIP(150));
    m_list->AppendColumn(_L("Printer"), wxLIST_FORMAT_LEFT, FromDIP(110));
    m_list->AppendColumn(_L("Result"), wxLIST_FORMAT_LEFT, FromDIP(80));
    body->Add(m_list, 1, wxEXPAND | wxRIGHT, pad);
    m_list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& e) {
        save_if_changed();
        show_print(e.GetIndex());
        update_compare();
    });
    m_list->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) { update_compare(); });

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
    // Forca: each problem's playbook (playbooks/<tag>.md: causes, checks, the Forca settings and tests for it).
    auto* playbooks = new wxButton(tag_box->GetStaticBox(), wxID_ANY, _L("Playbooks") + dots);
    playbooks->SetToolTip(_L("Open the playbook for a problem: likely causes, checks, and the Forca settings and "
                             "Calibration Wizard tests that address it."));
    tag_box->Add(playbooks, 0, wxALIGN_RIGHT | wxALL, FromDIP(4));
    playbooks->Bind(wxEVT_BUTTON, [this, playbooks](wxCommandEvent&) {
        std::string err;
        if (!forca_academy_ensure_skeleton(forca_academy_dir(), err)) {
            show_error(this, from_u8(err));
            return;
        }
        wxMenu                 menu;
        const std::vector<Tag> all = tags();
        for (size_t i = 0; i < all.size(); ++i)
            menu.Append(int(i + 1), all[i].label);
        const int picked = playbooks->GetPopupMenuSelectionFromUser(menu);
        if (picked < 1 || size_t(picked) > all.size())
            return;
        const fs::path file = forca_academy_dir() / "playbooks" / (std::string(all[size_t(picked - 1)].key) + ".md");
        if (fs::exists(file))
            wxLaunchDefaultApplication(from_path(file));
        else
            desktop_open_any_folder((forca_academy_dir() / "playbooks").string()); // the user deleted that one
    });
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
    m_compare = new wxButton(this, wxID_ANY, _L("Compare"));
    m_compare->SetToolTip(_L("Select two prints (Ctrl+click) to see what differed between them: presets, settings, "
                             "estimates and outcome."));
    auto* close = new wxButton(this, wxID_CLOSE, _L("Close"));
    buttons->Add(m_add_print, 0);
    buttons->Add(m_compare, 0, wxLEFT, FromDIP(6));
    // Forca: "Reset Forca data" (ForcaReset.hpp), also in Preferences and the Forca AI window.
    auto* reset = new wxButton(this, wxID_ANY, _L("Reset Forca data") + dots);
    reset->SetToolTip(_L("Start again from scratch: the print journal, Forca AI, AI-saved projects, Calibration Wizard "
                         "history or all of Forca. Everything moves to a backup folder first."));
    reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        save_if_changed();
        forca_reset_show_dialog(this);
    });
    buttons->Add(reset, 0, wxLEFT, FromDIP(6));
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
    m_compare->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        std::vector<long> rows;
        for (long r = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED); r >= 0;
             r = m_list->GetNextItem(r, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED))
            rows.push_back(r);
        if (rows.size() != 2)
            return;
        save_if_changed();
        // A = the older print, so "A -> B" reads as what changed.
        ForcaCompareDialog dlg(this, m_prints[size_t(rows[1])], m_prints[size_t(rows[0])]);
        dlg.ShowModal();
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

void ForcaPrintJournalDialog::update_compare()
{
    if (m_compare)
        m_compare->Enable(m_list->GetSelectedItemCount() == 2);
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
    if (!m_only.empty())
        m_prints.erase(std::remove_if(m_prints.begin(), m_prints.end(),
                                      [this](const ForcaAcademyPrint& p) {
                                          return std::find(m_only.begin(), m_only.end(), p.folder) == m_only.end();
                                      }),
                       m_prints.end());
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
    update_compare();
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
    m_prints[m_row].tags     = outcome["tags"].get<std::vector<std::string>>();
    m_list->SetItem(m_row, 3, result_label(m_prints[m_row].result));
    forca_academy_refresh_index();
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
