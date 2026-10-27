#include "ForcaOpenScadEditor.hpp"

#include "GUI.hpp" // from_u8 / into_u8
#include "GUI_App.hpp"
#include "GUI_Utils.hpp" // DPIDialog
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "format.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/ForcaOpenScad.hpp"

#include <boost/nowide/fstream.hpp>

#include <wx/busycursor.h>
#include <wx/colour.h>
#include <wx/defs.h>
#include <wx/event.h>
#include <wx/font.h>
#include <wx/gdicmn.h>
#include <wx/sizer.h>
#include <wx/stc/stc.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include <algorithm>
#include <functional>
#include <ios>
#include <iterator>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

namespace {

namespace fs = boost::filesystem;

constexpr int PROBLEM_MARKER     = 1;
constexpr int LINE_BEFORE_MARKER = 2;

// OpenSCAD's words: the language, then its built-in modules and functions.
const char *OPENSCAD_KEYWORDS = "module function if else for let each include use true false undef assert echo intersection_for";
const char *OPENSCAD_BUILTINS =
    "cube sphere cylinder polyhedron square circle polygon text import surface translate rotate scale resize mirror "
    "multmatrix color offset hull minkowski union difference intersection linear_extrude rotate_extrude projection render "
    "children abs sign sin cos tan acos asin atan atan2 floor round ceil ln log pow sqrt exp len min max norm cross concat "
    "lookup str chr ord search version version_num parent_module is_undef is_bool is_num is_string is_list is_function "
    "rands";

class OpenScadEditorDialog : public DPIDialog
{
public:
    OpenScadEditorDialog(wxWindow *parent, const fs::path &file, const std::string &console_output,
                         const std::function<std::string(const fs::path &)> &check)
        // TRN %1% is a file name.
        : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY,
                    format_wxstr(_L("Edit %1%"), file.filename().string()), wxDefaultPosition, wxDefaultSize,
                    wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER | wxMAXIMIZE_BOX)
        , m_file(file)
        , m_check(check)
    {
        SetBackgroundColour(*wxWHITE);
        SetFont(Label::Body_14);
        auto *sizer = new wxBoxSizer(wxVERTICAL);

        m_editor = new wxStyledTextCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(820), FromDIP(460)));
        style_editor();
        boost::nowide::ifstream in(m_file.string(), std::ios::binary);
        const std::string       text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        // New lines end like the file's own.
        m_editor->SetEOLMode(text.find("\r\n") != std::string::npos || text.find('\n') == std::string::npos ? wxSTC_EOL_CRLF : wxSTC_EOL_LF);
        m_editor->SetText(from_u8(text));
        m_editor->EmptyUndoBuffer();
        m_editor->SetSavePoint();
        sizer->Add(m_editor, 3, wxEXPAND | wxALL, FromDIP(10));

        sizer->Add(new Label(this, Label::Head_14, _L("OpenSCAD console")), 0, wxLEFT | wxRIGHT, FromDIP(10));
        m_console = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, FromDIP(130)),
                                   wxTE_MULTILINE | wxTE_READONLY);
        m_console->SetFont(wxFont(wxFontInfo(9).Family(wxFONTFAMILY_TELETYPE)));
        sizer->Add(m_console, 1, wxEXPAND | wxALL, FromDIP(10));

        auto *buttons = new DialogButtons(this, {L("Save and check"), "Close"}, _L("Save and check"));
        buttons->GetFIRST()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { save_and_check(); });
        buttons->GetLAST()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Close(); });
        sizer->Add(buttons, 0, wxEXPAND);
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &event) {
            if (m_editor->GetModify() && event.CanVeto() &&
                MessageDialog(this, _L("Your changes to the file aren't saved. Close without saving?"), GetTitle(),
                              wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION)
                        .ShowModal() != wxID_YES) {
                event.Veto();
                return;
            }
            EndModal(m_saved ? wxID_OK : wxID_CANCEL);
        });

        show_console(console_output);
        SetSizerAndFit(sizer);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
        m_editor->SetFocus();
    }

    bool saved() const { return m_saved; }

protected:
    void on_dpi_changed(const wxRect &) override
    {
        m_editor->SetMinSize(wxSize(FromDIP(820), FromDIP(460)));
        m_console->SetMinSize(wxSize(-1, FromDIP(130)));
        update_margin();
        GetSizer()->SetSizeHints(this);
        Refresh();
    }

private:
    void style_editor()
    {
        const bool     dark       = wxGetApp().dark_mode();
        const wxColour background = dark ? wxColour("#1E1E1E") : wxColour("#FFFFFF");
        const wxColour text       = dark ? wxColour("#D4D4D4") : wxColour("#1F1F1F");

        m_editor->StyleSetFont(wxSTC_STYLE_DEFAULT, wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE)));
        m_editor->StyleSetBackground(wxSTC_STYLE_DEFAULT, background);
        m_editor->StyleSetForeground(wxSTC_STYLE_DEFAULT, text);
        m_editor->StyleClearAll(); // every style starts from the default above
        m_editor->SetLexer(wxSTC_LEX_CPP); // OpenSCAD's comments, strings and numbers are written as in C
        m_editor->SetKeyWords(0, OPENSCAD_KEYWORDS);
        m_editor->SetKeyWords(1, OPENSCAD_BUILTINS);
        const wxColour comment = dark ? wxColour("#6A9955") : wxColour("#008000");
        for (int style : {wxSTC_C_COMMENT, wxSTC_C_COMMENTLINE, wxSTC_C_COMMENTDOC, wxSTC_C_COMMENTLINEDOC})
            m_editor->StyleSetForeground(style, comment);
        m_editor->StyleSetForeground(wxSTC_C_NUMBER, dark ? wxColour("#B5CEA8") : wxColour("#098658"));
        m_editor->StyleSetForeground(wxSTC_C_STRING, dark ? wxColour("#CE9178") : wxColour("#A31515"));
        m_editor->StyleSetForeground(wxSTC_C_WORD, dark ? wxColour("#569CD6") : wxColour("#0000FF"));
        m_editor->StyleSetBold(wxSTC_C_WORD, true);
        m_editor->StyleSetForeground(wxSTC_C_WORD2, dark ? wxColour("#4EC9B0") : wxColour("#267F99"));
        m_editor->StyleSetForeground(wxSTC_C_OPERATOR, text);
        m_editor->StyleSetForeground(wxSTC_STYLE_LINENUMBER, dark ? wxColour("#858585") : wxColour("#6E6E6E"));
        m_editor->StyleSetBackground(wxSTC_STYLE_LINENUMBER, dark ? wxColour("#252526") : wxColour("#F3F3F3"));
        m_editor->SetCaretForeground(text);
        m_editor->SetSelBackground(true, dark ? wxColour("#264F78") : wxColour("#ADD6FF"));

        m_editor->SetTabWidth(4);
        m_editor->SetUseTabs(false);
        m_editor->SetIndent(4);
        m_editor->SetWrapMode(wxSTC_WRAP_NONE);
        m_editor->SetMarginType(0, wxSTC_MARGIN_NUMBER);
        m_editor->SetMarginWidth(1, 0);
        m_editor->MarkerDefine(PROBLEM_MARKER, wxSTC_MARK_BACKGROUND, text, dark ? wxColour("#5A1D1D") : wxColour("#FDDCDC"));
        m_editor->MarkerDefine(LINE_BEFORE_MARKER, wxSTC_MARK_BACKGROUND, text, dark ? wxColour("#4D4419") : wxColour("#FFF4C2"));
        m_editor->Bind(wxEVT_STC_CHANGE, [this](wxStyledTextEvent &event) {
            update_margin();
            event.Skip();
        });
        update_margin();
    }

    // Wide enough for the largest line number.
    void update_margin()
    {
        const std::string digits = std::to_string(std::max(m_editor->GetLineCount(), 99));
        m_editor->SetMarginWidth(0, m_editor->TextWidth(wxSTC_STYLE_LINENUMBER, "_" + digits) + FromDIP(6));
    }

    // The last line above line with code on it (not blank, not only a // comment), or -1.
    int code_line_before(int line) const
    {
        while (--line >= 0) {
            const wxString text = m_editor->GetLine(line).Strip(wxString::both);
            if (!text.empty() && !text.StartsWith("//"))
                return line;
        }
        return -1;
    }

    // Shows OpenSCAD's output and marks the lines it names; the first error (else warning) is selected. OpenSCAD names
    // the line where parsing stopped, so for a syntax error the code line before it is marked too.
    void show_console(const std::string &output)
    {
        wxString console = output.empty() ? _L("No messages.") : from_u8(output);
        m_editor->MarkerDeleteAll(PROBLEM_MARKER);
        m_editor->MarkerDeleteAll(LINE_BEFORE_MARKER);
        int  first          = -1;
        bool first_is_error = false;
        bool syntax         = false;
        for (const OpenScadProblemLine &problem : forca_openscad_problem_lines(output, m_file.filename().string())) {
            const int line = problem.line - 1; // OpenSCAD counts from 1
            if (line < 0 || line >= m_editor->GetLineCount())
                continue;
            m_editor->MarkerAdd(line, PROBLEM_MARKER);
            if (const int before = problem.syntax ? code_line_before(line) : -1; before >= 0) {
                m_editor->MarkerAdd(before, LINE_BEFORE_MARKER);
                syntax = true;
            }
            if (first < 0 || (problem.error && !first_is_error)) {
                first          = line;
                first_is_error = problem.error;
            }
        }
        if (syntax)
            console += "\n" + _L("A syntax error is often at the end of the code line before it, such as a missing ; or "
                                 "bracket. That line is marked in yellow.");
        m_console->ChangeValue(console);
        if (first >= 0) {
            m_editor->GotoLine(first);
            m_editor->SetSelection(m_editor->PositionFromLine(first), m_editor->GetLineEndPosition(first));
            m_editor->EnsureCaretVisible();
        }
    }

    void save_and_check()
    {
        {
            boost::nowide::ofstream out(m_file.string(), std::ios::binary | std::ios::trunc);
            out << into_u8(m_editor->GetText());
            if (!out) {
                // TRN %1% is a file path.
                MessageDialog(this, format_wxstr(_L("Couldn't save %1%."), m_file.string()), GetTitle(), wxOK | wxICON_ERROR).ShowModal();
                return;
            }
        }
        m_editor->SetSavePoint();
        m_saved = true;
        wxBusyCursor busy;
        const std::string output = m_check ? m_check(m_file) : std::string();
        show_console(output);
    }

    fs::path                                   m_file;
    std::function<std::string(const fs::path &)> m_check;
    wxStyledTextCtrl                          *m_editor  = nullptr;
    wxTextCtrl                                *m_console = nullptr;
    bool                                       m_saved   = false;
};

} // namespace

bool forca_openscad_edit_source(wxWindow *parent, const fs::path &file, const std::string &console_output,
                                const std::function<std::string(const fs::path &)> &check)
{
    OpenScadEditorDialog dlg(parent, file, console_output, check);
    dlg.ShowModal();
    return dlg.saved();
}

}} // namespace Slic3r::GUI
