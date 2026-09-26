// Forca AI rule R2 -- no print without the user's say-so, once (HQ PLAN_forca_ai.md §2).
//
// The AI can only REQUEST a print (forca_request_print). Forca then shows the user an approval card: the plate, the
// printer, the filaments, time and weight, and the AI's reason. Only the user's click on "Approve" in that card acts
// on it -- there is no tool, endpoint or setting that approves. An approval:
//   - is single use (the request is closed by it),
//   - is bound to the exact sliced G-code (content hash), plate and printer preset of the request; a re-slice or any
//     change voids it (checked again at the click),
//   - then opens Forca's own print dialog, exactly like the Print button, where the user still picks the printer /
//     AMS slots and presses Send. Nothing is sent to a printer by this code itself.
#include "ForcaAI.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GLCanvas3D.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/ForcaAISafePath.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

#include <boost/filesystem/operations.hpp>

namespace Slic3r { namespace GUI {

using json = nlohmann::json;

namespace {

struct PrintRequest
{
    int         id    = 0;
    std::string state;          // "pending" | "declined" | "approved" | "void"
    std::string detail;         // why it was voided, etc.
    int         plate = -1;     // 0-based
    std::string printer;        // printer preset name at request time
    std::string gcode_hash;     // hash of the plate's sliced G-code at request time
    std::string reason;         // the AI's note to the user
    json        report;         // slice report shown on the card
};

PrintRequest s_request;         // one request at a time; a new one replaces a pending one
int          s_next_id = 1;
wxDialog*    s_card    = nullptr;

// Current identity of a plate's slice: empty if it is not sliced (validly) or its G-code is missing.
std::string plate_gcode_hash(int plate_idx)
{
    PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    if (plate_idx < 0 || plate_idx >= plates.get_plate_count())
        return {};
    PartPlate* plate = plates.get_plate(plate_idx);
    if (!plate->is_slice_result_valid())
        return {};
    const boost::filesystem::path gcode = into_path(from_u8(plate->get_tmp_gcode_path()));
    boost::system::error_code     ec;
    if (!boost::filesystem::is_regular_file(gcode, ec))
        return {};
    return Slic3r::ForcaAI::Ledger::file_hash(gcode);
}

std::string current_printer() { return wxGetApp().preset_bundle->printers.get_edited_preset().name; }

// The reason an approval can't be acted on any more, or empty if it still matches the request.
std::string stale_reason(const PrintRequest& r)
{
    if (current_printer() != r.printer)
        return "the printer preset changed (was '" + r.printer + "')";
    const std::string hash = plate_gcode_hash(r.plate);
    if (hash.empty())
        return "plate " + std::to_string(r.plate + 1) + " is no longer sliced (something changed)";
    if (hash != r.gcode_hash)
        return "the sliced G-code changed since the request";
    return {};
}

void close_card()
{
    if (s_card) {
        s_card->Destroy();
        s_card = nullptr;
    }
}

void finish(const std::string& state, const std::string& detail)
{
    s_request.state  = state;
    s_request.detail = detail;
    ForcaAI::instance().log("print request", "Request #" + std::to_string(s_request.id) + " " + state +
                                                 (detail.empty() ? "" : ": " + detail), state == "approved");
    close_card();
}

// The user clicked Approve (the only way a request is ever approved).
void on_user_approve()
{
    if (s_request.state != "pending") // single use
        return;
    if (std::string why = stale_reason(s_request); !why.empty()) {
        finish("void", why + ". Nothing was sent; the AI can slice again and ask again.");
        return;
    }
    Plater* p = wxGetApp().plater();
    if (p->get_partplate_list().get_curr_plate_index() != s_request.plate) {
        p->select_plate(s_request.plate);
        if (std::string why = stale_reason(s_request); !why.empty()) {
            finish("void", why + ". Nothing was sent.");
            return;
        }
    }
    finish("approved", "the user approved it; Forca's print dialog was opened for them to send");
    p->print_current_plate(); // exactly what the Print button does: the user still picks the printer and sends
}

wxBitmap plate_bitmap(int plate_idx, int size)
{
    ThumbnailData    data;
    ThumbnailsParams params{ {}, false, true, true, false, plate_idx };
    wxGetApp().plater()->get_view3D_canvas3D()->render_thumbnail(data, size, size, params, Camera::EType::Ortho,
                                                                  Camera::ViewAngleType::Iso);
    if (!data.is_valid())
        return wxNullBitmap;
    wxImage img(int(data.width), int(data.height));
    img.InitAlpha();
    for (unsigned int y = 0; y < data.height; ++y)
        for (unsigned int x = 0; x < data.width; ++x) {
            const unsigned char* px = &data.pixels[4 * ((data.height - 1 - y) * data.width + x)]; // OpenGL rows are bottom-up
            img.SetRGB(int(x), int(y), px[0], px[1], px[2]);
            img.SetAlpha(int(x), int(y), px[3]);
        }
    return wxBitmap(img);
}

void show_card()
{
    close_card();
    MainFrame* mf = wxGetApp().mainframe;
    auto*      dlg = new wxDialog(mf, wxID_ANY, _L("Forca AI - print request"), wxDefaultPosition, wxDefaultSize,
                                  wxDEFAULT_DIALOG_STYLE);
    s_card = dlg;
    dlg->SetFont(Label::Body_14);
    const int pad  = dlg->FromDIP(12);
    auto*     root = new wxBoxSizer(wxVERTICAL);

    auto* title = new wxStaticText(dlg, wxID_ANY, _L("The AI asks to print this plate"));
    title->SetFont(Label::Head_16);
    root->Add(title, 0, wxALL, pad);

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    const wxBitmap bmp = plate_bitmap(s_request.plate, dlg->FromDIP(220));
    if (bmp.IsOk())
        row->Add(new wxStaticBitmap(dlg, wxID_ANY, bmp), 0, wxRIGHT, pad);

    const json&  r = s_request.report;
    wxString     facts;
    facts << _L("Plate") << ": " << (s_request.plate + 1) << "\n";
    facts << _L("Printer preset") << ": " << wxString::FromUTF8(s_request.printer.c_str()) << "\n";
    facts << _L("Print time") << ": " << wxString::FromUTF8(r.value("print_time", std::string("?")).c_str()) << "\n";
    facts << _L("Filament") << ": " << wxString::Format("%.1f g", r.value("filament_total_g", 0.0)) << "\n";
    const auto& slots = wxGetApp().preset_bundle->filament_presets;
    if (r.contains("per_filament"))
        for (const auto& f : r["per_filament"]) {
            const int slot = f.value("filament", 0);
            facts << "   " << slot << ": "
                  << wxString::FromUTF8((slot >= 1 && size_t(slot) <= slots.size() ? slots[slot - 1] : std::string("?")).c_str())
                  << wxString::Format("  (%.1f g)", f.value("weight_g", 0.0)) << "\n";
        }
    auto* facts_text = new wxStaticText(dlg, wxID_ANY, facts);
    row->Add(facts_text, 1, wxEXPAND);
    root->Add(row, 0, wxLEFT | wxRIGHT | wxEXPAND, pad);

    if (!s_request.reason.empty()) {
        auto* why = new wxStaticText(dlg, wxID_ANY, _L("The AI says") + ": " + wxString::FromUTF8(s_request.reason.c_str()));
        why->Wrap(dlg->FromDIP(520));
        root->Add(why, 0, wxALL, pad);
    }

    auto* note = new wxStaticText(dlg, wxID_ANY,
        _L("Approve opens Forca's print dialog for this plate, where you choose the printer and filament slots and "
           "press Send. The approval works once, and only for this exact slice."));
    note->Wrap(dlg->FromDIP(520));
    note->SetForegroundColour(wxColour("#6B7488"));
    root->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer(1);
    auto* decline = new wxButton(dlg, wxID_ANY, _L("Decline"));
    auto* approve = new wxButton(dlg, wxID_ANY, _L("Approve"));
    buttons->Add(decline, 0, wxRIGHT, dlg->FromDIP(8));
    buttons->Add(approve, 0);
    root->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, pad);

    decline->Bind(wxEVT_BUTTON, [](wxCommandEvent&) { if (s_request.state == "pending") finish("declined", "the user declined it"); });
    approve->Bind(wxEVT_BUTTON, [](wxCommandEvent&) { on_user_approve(); });
    dlg->Bind(wxEVT_CLOSE_WINDOW, [](wxCloseEvent&) { if (s_request.state == "pending") finish("declined", "the user closed the card"); else close_card(); });

    dlg->SetSizerAndFit(root);
    dlg->CenterOnParent();
    dlg->Show(); // modeless: Forca (and the AI) keep working while it waits
    dlg->Raise();
    approve->SetFocus();
}

// ---- tools ------------------------------------------------------------------------------------

ForcaAIResult tool_request_print(const json& args)
{
    Plater* p = wxGetApp().plater();
    if (!p || !wxGetApp().preset_bundle || !wxGetApp().mainframe)
        return ForcaAIResult::error("Forca is still starting up.");
    PartPlateList& plates = p->get_partplate_list();
    const int      plate  = args.value("plate", plates.get_curr_plate_index() + 1) - 1;
    if (plate < 0 || plate >= plates.get_plate_count())
        return ForcaAIResult::error("plate must be between 1 and " + std::to_string(plates.get_plate_count()) + ".");
    std::string err;
    json        report = forca_ai_plate_report(plate, err);
    if (report.empty())
        return ForcaAIResult::error(err + " Slice it with forca_slice first.");
    if (report.value("toolpath_outside_plate", false))
        return ForcaAIResult::error("The slice has toolpaths outside the plate; fix that before asking to print.");
    const std::string hash = plate_gcode_hash(plate);
    if (hash.empty())
        return ForcaAIResult::error("Forca's sliced G-code for that plate is missing. Slice again.");

    if (s_request.state == "pending")
        finish("void", "replaced by a newer request");
    s_request = PrintRequest{ s_next_id++, "pending", {}, plate, current_printer(), hash, args.value("reason", std::string()), report };
    show_card();
    return ForcaAIResult::json({ { "request", s_request.id }, { "state", "pending" },
                                 { "note", "The user now sees an approval card in Forca. Only their click approves it; "
                                           "then Forca's print dialog opens for them to send. Check with "
                                           "forca_print_request_status. Any re-slice or change voids the request." } });
}

ForcaAIResult tool_print_request_status(const json&)
{
    if (s_request.id == 0)
        return ForcaAIResult::text("No print has been requested.");
    json j = { { "request", s_request.id }, { "state", s_request.state }, { "plate", s_request.plate + 1 }, { "printer", s_request.printer } };
    if (!s_request.detail.empty())
        j["detail"] = s_request.detail;
    if (s_request.state == "pending")
        if (std::string why = stale_reason(s_request); !why.empty())
            j["warning"] = "If approved now it would be voided: " + why + ".";
    return ForcaAIResult::json(j);
}

} // namespace

void register_forca_ai_print_tools(ForcaAI& ai)
{
    ai.register_tool({ "forca_request_print", "Ask the user to print",
        "Ask the user to print a sliced plate. Forca shows them an approval card (plate, printer, filaments, time, "
        "your reason). Only their click on Approve acts on it, once, for this exact slice; then Forca's own print "
        "dialog opens for them to choose the printer / slots and send. You cannot approve, send or start a print.",
        { { "type", "object" },
          { "properties", { { "plate", { { "type", "integer" }, { "minimum", 1 } } },
                            { "reason", { { "type", "string" }, { "description", "One or two sentences for the user: what and why." } } } } } },
        tool_request_print });

    ai.register_tool({ "forca_print_request_status", "Print request status",
        "The state of the last print request: pending, declined, approved (Forca's print dialog was opened for the "
        "user) or void (something changed, or it was replaced).",
        { { "type", "object" }, { "properties", json::object() } }, tool_print_request_status });
}

}} // namespace Slic3r::GUI
