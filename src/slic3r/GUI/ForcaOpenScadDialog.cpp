#include "ForcaOpenScadDialog.hpp"

#include "GUI.hpp" // from_u8 / into_u8
#include "GUI_App.hpp"
#include "GUI_Utils.hpp" // DPIDialog
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "format.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/TextInput.hpp"
#include "libslic3r/ForcaOpenScad.hpp"

#include <wx/colour.h>
#include <wx/defs.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include <algorithm>
#include <cstddef>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

namespace {

// One parameter's control; only the one matching its kind is set.
struct ParamControl
{
    const OpenScadParam *param = nullptr;
    ::CheckBox          *check = nullptr;  // Boolean
    ::ComboBox          *choice = nullptr; // a dropdown (options)
    TextInput           *text = nullptr;   // Number, String, Vector
};

bool is_number(const std::string &text)
{
    static const std::regex number(R"(^\s*[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?\s*$)");
    return std::regex_match(text, number);
}

class OpenScadParamsDialog : public DPIDialog
{
public:
    OpenScadParamsDialog(wxWindow *parent, const std::string &file_name, const std::vector<OpenScadParam> &params,
                         const std::vector<std::pair<std::string, std::string>> &values)
        : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, _L("OpenSCAD parameters"),
                    wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
    {
        SetBackgroundColour(*wxWHITE);
        SetFont(Label::Body_14);
        auto *sizer = new wxBoxSizer(wxVERTICAL);

        auto *title = new Label(this, Label::Head_16, wxEmptyString);
        title->SetLabelText(from_u8(file_name));
        sizer->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        sizer->Add(new Label(this, _L("Values left at the file's default keep following the file when it changes."), LB_AUTO_WRAP), 0,
                   wxEXPAND | wxALL, FromDIP(12));

        m_list = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        m_list->SetBackgroundColour(*wxWHITE);
        m_list->SetScrollRate(0, FromDIP(10));
        auto *list_sizer = new wxBoxSizer(wxVERTICAL);

        // Groups in the order the file lists them.
        std::vector<std::string> groups;
        for (const OpenScadParam &param : params)
            if (std::find(groups.begin(), groups.end(), param.group) == groups.end())
                groups.push_back(param.group);
        for (const std::string &group : groups) {
            auto *head = new Label(m_list, Label::Head_14, wxEmptyString);
            head->SetLabelText(group.empty() ? _L("Parameters") : from_u8(group));
            list_sizer->Add(head, 0, wxTOP | wxBOTTOM, FromDIP(8));
            auto *grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(16));
            grid->AddGrowableCol(0, 1);
            for (const OpenScadParam &param : params)
                if (param.group == group)
                    add_param(grid, param, value_of(values, param));
            list_sizer->Add(grid, 0, wxEXPAND | wxLEFT, FromDIP(8));
        }
        m_list->SetSizer(list_sizer);
        m_list->SetMinSize(wxSize(FromDIP(620), std::min(FromDIP(520), list_sizer->CalcMin().y + FromDIP(10))));
        m_list->FitInside();
        sizer->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

        auto *buttons = new DialogButtons(this, {"Reset", L("Edit source..."), L("Render model"), "Cancel"}, _L("Render model"), 2);
        buttons->GetFIRST()->SetToolTip(_L("Set every parameter back to the file's default."));
        buttons->GetFIRST()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { reset(); });
        buttons->GetButtonFromIndex(1)->SetToolTip(_L("Open the .scad file in a small editor, with OpenSCAD's messages."));
        buttons->GetButtonFromIndex(1)->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_EDIT); });
        buttons->GetButtonFromIndex(2)->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            if (validate())
                EndModal(wxID_OK);
        });
        buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
        sizer->Add(buttons, 0, wxEXPAND | wxTOP, FromDIP(8));

        SetSizerAndFit(sizer);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

    // Every parameter whose value differs from the file's default.
    std::vector<std::pair<std::string, std::string>> values() const
    {
        std::vector<std::pair<std::string, std::string>> out;
        for (const ParamControl &control : m_controls) {
            const std::string value = value_of(control);
            if (value != control.param->value)
                out.emplace_back(control.param->name, value);
        }
        return out;
    }

protected:
    void on_dpi_changed(const wxRect &) override
    {
        for (ParamControl &control : m_controls) {
            if (control.check != nullptr)
                control.check->Rescale();
            if (control.choice != nullptr)
                control.choice->Rescale();
            if (control.text != nullptr)
                control.text->Rescale();
        }
        m_list->SetScrollRate(0, FromDIP(10));
        GetSizer()->SetSizeHints(this);
        Refresh();
    }

private:
    static std::string value_of(const std::vector<std::pair<std::string, std::string>> &values, const OpenScadParam &param)
    {
        for (const auto &[name, value] : values)
            if (name == param.name)
                return value;
        return param.value;
    }

    // A control's value in OpenSCAD syntax.
    static std::string value_of(const ParamControl &control)
    {
        if (control.check != nullptr)
            return control.check->GetValue() ? "true" : "false";
        if (control.choice != nullptr) {
            const int i = control.choice->GetSelection();
            return i >= 0 && size_t(i) < control.param->options.size() ? control.param->options[size_t(i)].second : control.param->value;
        }
        const std::string text = into_u8(control.text->GetTextCtrl()->GetValue());
        return control.param->type == OpenScadParam::Type::String ? forca_openscad_quote(text) : text;
    }

    void set_value(ParamControl &control, const std::string &value)
    {
        if (control.check != nullptr)
            control.check->SetValue(value == "true");
        else if (control.choice != nullptr) {
            for (size_t i = 0; i < control.param->options.size(); ++i)
                if (control.param->options[i].second == value)
                    control.choice->SetSelection(int(i));
        } else
            control.text->GetTextCtrl()->ChangeValue(
                from_u8(control.param->type == OpenScadParam::Type::String ? forca_openscad_unquote(value) : value));
    }

    void add_param(wxFlexGridSizer *grid, const OpenScadParam &param, const std::string &value)
    {
        // Name, and the file's caption under it.
        auto *label_sizer = new wxBoxSizer(wxVERTICAL);
        auto *name        = new Label(m_list, Label::Body_14, wxEmptyString);
        name->SetLabelText(from_u8(param.name));
        label_sizer->Add(name);
        if (!param.caption.empty()) {
            auto *caption = new Label(m_list, Label::Body_12, wxEmptyString);
            caption->SetLabelText(from_u8(param.caption));
            caption->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#5C5C5C")));
            caption->Wrap(FromDIP(330));
            label_sizer->Add(caption);
        }
        grid->Add(label_sizer, 1, wxEXPAND);

        ParamControl control;
        control.param = &param;
        wxWindow *window = nullptr;
        if (!param.options.empty()) {
            control.choice = new ::ComboBox(m_list, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(230), -1), 0, nullptr,
                                            wxCB_READONLY);
            for (const auto &[label, option_value] : param.options)
                control.choice->Append(from_u8(label));
            window = control.choice;
        } else if (param.type == OpenScadParam::Type::Boolean) {
            control.check = new ::CheckBox(m_list);
            window        = control.check;
        } else {
            control.text = new TextInput(m_list, wxEmptyString, "", "", wxDefaultPosition, wxSize(FromDIP(230), -1));
            if (param.min && param.max)
                // TRN The range of an OpenSCAD parameter: %1% and %2% are numbers.
                control.text->SetToolTip(format_wxstr(_L("From %1% to %2%"), *param.min, *param.max));
            window = control.text;
        }
        set_value(control, value);
        grid->Add(window, 0, wxALIGN_CENTER_VERTICAL);
        m_controls.push_back(control);
    }

    void reset()
    {
        for (ParamControl &control : m_controls)
            set_value(control, control.param->value);
    }

    // Numbers must be numbers and lists lists, or OpenSCAD would read them as something else.
    bool validate()
    {
        for (const ParamControl &control : m_controls) {
            if (control.text == nullptr)
                continue;
            const std::string text = into_u8(control.text->GetTextCtrl()->GetValue());
            wxString          problem;
            if (control.param->type == OpenScadParam::Type::Number && !is_number(text))
                // TRN %1% is the name of an OpenSCAD parameter.
                problem = format_wxstr(_L("\"%1%\" needs a number."), control.param->name);
            else if (control.param->type == OpenScadParam::Type::Vector &&
                     (text.find('[') == std::string::npos || text.find(']') == std::string::npos))
                // TRN %1% is the name of an OpenSCAD parameter.
                problem = format_wxstr(_L("\"%1%\" needs a list like [1, 2, 3]."), control.param->name);
            if (!problem.empty()) {
                MessageDialog(this, problem, _L("OpenSCAD parameters"), wxOK | wxICON_WARNING).ShowModal();
                control.text->GetTextCtrl()->SetFocus();
                return false;
            }
        }
        return true;
    }

    wxScrolledWindow         *m_list = nullptr;
    std::vector<ParamControl> m_controls;
};

} // namespace

OpenScadParamsAction forca_openscad_parameters_dialog(wxWindow *parent, const std::string &file_name, const std::vector<OpenScadParam> &params,
                                                      std::vector<std::pair<std::string, std::string>> &values)
{
    OpenScadParamsDialog dlg(parent, file_name, params, values);
    const int            result = dlg.ShowModal();
    if (result != wxID_OK && result != wxID_EDIT)
        return OpenScadParamsAction::Cancel;
    values = dlg.values();
    return result == wxID_OK ? OpenScadParamsAction::Render : OpenScadParamsAction::EditSource;
}

}} // namespace Slic3r::GUI
