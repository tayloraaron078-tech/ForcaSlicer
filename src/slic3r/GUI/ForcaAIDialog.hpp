#ifndef slic3r_ForcaAIDialog_hpp_
#define slic3r_ForcaAIDialog_hpp_

#include <wx/dialog.h>

#include <string>
#include <vector>

class wxCheckBox;
class wxCheckListBox;
class wxStaticText;
class wxTextCtrl;
class wxListCtrl;

namespace Slic3r { namespace GUI {

// The Forca AI window: turn AI access on/off (opt-in), copy the connection command for Claude Code, and watch the
// AI's activity feed. A single persistent instance owned by MainFrame (closing only hides it).
class ForcaAIDialog : public wxDialog
{
public:
    explicit ForcaAIDialog(wxWindow* parent);
    void refresh(); // state + activity (also called by ForcaAI whenever they change)

private:
    wxCheckBox*   m_enable   { nullptr };
    wxStaticText* m_state    { nullptr };
    wxTextCtrl*   m_command  { nullptr };
    wxListCtrl*   m_activity { nullptr };
    wxCheckListBox*          m_printers { nullptr }; // per-printer opt-in: the AI sees only ticked printers
    wxCheckListBox*          m_pausers  { nullptr }; // per-printer "AI may pause" (only counts on a shared printer)
    std::vector<std::string> m_printer_ids;           // device id per m_printers row
    bool                     m_printers_built { false };
};

}} // namespace Slic3r::GUI

#endif // slic3r_ForcaAIDialog_hpp_
