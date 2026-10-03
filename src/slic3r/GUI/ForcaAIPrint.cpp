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
// At the Advanced control level (ForcaAI::level(), chosen by the user in the Forca AI window) the card is skipped: a
// request opens the print dialog straight away, after the same checks. The user still presses Send.
// With a print grant the user gave (ForcaAIAutonomy.cpp), a request with start: true is sent by Forca itself after
// the grant's checks and a visible countdown in that dialog.
#include "ForcaAI.hpp"
#include "ForcaAcademy.hpp"

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
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

using json = nlohmann::json;

namespace {

struct PrintRequest
{
    int         id    = 0;
    std::string state;          // "pending" | "declined" | "approved" | "void"; grant prints: "sending" | "sent" | "cancelled"
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
                                                 (detail.empty() ? "" : ": " + detail), state == "approved" || state == "sent");
    close_card();
}

// Puts the request's plate in front. Why the request can no longer be acted on, or empty.
std::string select_request_plate()
{
    if (std::string why = stale_reason(s_request); !why.empty())
        return why;
    Plater* p = wxGetApp().plater();
    if (p->get_partplate_list().get_curr_plate_index() == s_request.plate)
        return {};
    p->select_plate(s_request.plate);
    return stale_reason(s_request);
}

// Opens Forca's print dialog for the pending request's plate, unless the slice changed since the request.
void open_print_dialog(const std::string& how, const std::string& academy_source)
{
    if (std::string why = select_request_plate(); !why.empty()) {
        finish("void", why + ". Nothing was sent; the AI can slice again and ask again.");
        return;
    }
    finish("approved", how);
    forca_academy_mark_source(academy_source);
    wxGetApp().plater()->print_current_plate(); // exactly what the Print button does: the user still picks the printer and sends
}

// The user clicked Approve (the only way a request is ever approved at the Guarded level).
void on_user_approve()
{
    if (s_request.state == "pending") // single use
        open_print_dialog("the user approved it; Forca's print dialog was opened for them to send",
                          "forca_ai: approved print request");
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
    if (s_request.state == "sending")
        return ForcaAIResult::error("Forca is still sending the previous print of the grant; see forca_print_request_status.");

    // A print grant: Forca sends it itself, after the grant's checks (ForcaAIAutonomy.cpp).
    // Lenient: an MCP client holding an older copy of this tool's schema sends unknown arguments as text.
    const json& start_arg = args.contains("start") ? args["start"] : json(false);
    const bool  start     = start_arg.is_boolean() ? start_arg.get<bool>() : start_arg.is_string() && start_arg.get<std::string>() == "true";
    const json& check_arg = args.contains("bed_check") ? args["bed_check"] : json(0);
    int         bed_check = check_arg.is_number_integer() ? check_arg.get<int>() : 0;
    if (check_arg.is_string())
        try { bed_check = std::stoi(check_arg.get<std::string>()); } catch (...) {}
    if (start) {
        const int        print_time = report.value("print_time_s", 0);
        std::vector<int> used;
        for (const auto& f : report.value("per_filament", json::array()))
            used.push_back(f.value("filament", 0));
        std::map<int, std::string> trays; // {"1": "Ext"}; tolerant of numbers given as text
        if (args.contains("trays") && args["trays"].is_object())
            for (const auto& t : args["trays"].items())
                try {
                    trays[std::stoi(t.key())] = t.value().is_string() ? t.value().get<std::string>() : t.value().dump();
                } catch (...) {}
        std::string dev_id;
        ForcaAIGrantPrint job;
        job.plate        = plate;
        job.print_time_s = print_time;
        job.bed_check    = bed_check;
        job.trays        = trays;
        job.used         = used;
        if (std::string why = forca_ai_grant_check(job, args.value("printer", std::string()), dev_id); !why.empty()) {
            BOOST_LOG_TRIVIAL(info) << "[forca-grant] plate " << plate + 1 << " refused: " << why;
            return ForcaAIResult::error(why);
        }
        BOOST_LOG_TRIVIAL(info) << "[forca-grant] plate " << plate + 1 << " passed the grant check";
        if (s_request.state == "pending")
            finish("void", "replaced by a newer request");
        s_request = PrintRequest{ s_next_id++, "sending", {}, plate, current_printer(), hash, args.value("reason", std::string()), report };
        const int id = s_request.id;
        job.request = s_request.id;
        wxGetApp().CallAfter([job, dev_id]() mutable { // after this tool returns: the dialog is modal
            const int id = job.request;
            BOOST_LOG_TRIVIAL(info) << "[forca-grant] request " << id << ": send starts";
            if (s_request.id != id || s_request.state != "sending")
                return;
            if (std::string why = select_request_plate(); !why.empty()) {
                finish("void", why + ". Nothing was sent.");
                return;
            }
            job.dev_id = dev_id;
            job.stale  = [] { return stale_reason(s_request); };
            job.done   = [id](const std::string& state, const std::string& detail) {
                if (s_request.id == id && s_request.state == "sending")
                    finish(state, detail);
            };
            forca_ai_grant_send(std::move(job));
        });
        return ForcaAIResult::json({ { "request", id }, { "state", "sending" },
                                     { "note", "Under the user's print grant: Forca opens its print dialog, waits until it is "
                                               "ready with nothing to confirm, counts down 10 s (the user can cancel) and "
                                               "presses Send. Check forca_print_request_status for 'sent'." } });
    }

    if (s_request.state == "pending")
        finish("void", "replaced by a newer request");
    s_request = PrintRequest{ s_next_id++, "pending", {}, plate, current_printer(), hash, args.value("reason", std::string()), report };
    if (ForcaAI::instance().level() == ForcaAI::Level::Advanced) {
        // After this tool returns: the print dialog may be modal, and the AI's call must not wait on the user.
        const int id = s_request.id;
        wxGetApp().CallAfter([id]() {
            if (s_request.id == id && s_request.state == "pending")
                open_print_dialog("Advanced control level, so no approval card: Forca's print dialog was opened for the "
                                  "user to send",
                                  "forca_ai: print request (Advanced level, no approval card)");
        });
        return ForcaAIResult::json({ { "request", s_request.id }, { "state", "pending" }, { "control_level", "advanced" },
                                     { "note", "Forca is opening its print dialog for this plate now; the user picks the "
                                               "printer and slots and presses Send. Check with forca_print_request_status." } });
    }
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
        "dialog opens for them to choose the printer / slots and send. At the Advanced control level there is no card: "
        "the print dialog opens at once. You cannot approve a request. Only with a print grant the user gave "
        "(forca_status -> print_grant) may you pass start: true: Forca then sends it itself, on a grant printer that is "
        "idle and whose bed is clear, after a 10 s countdown the user can cancel.",
        { { "type", "object" },
          { "properties", { { "plate", { { "type", "integer" }, { "minimum", 1 } } },
                            { "reason", { { "type", "string" }, { "description", "One or two sentences for the user: what and why." } } },
                            { "start", { { "type", "boolean" }, { "description", "Send it under the user's print grant (see print_grant in forca_status)." } } },
                            { "printer", { { "type", "string" }, { "description", "With start: the grant printer (id or name; default: the grant's only one)." } } },
                            { "bed_check", { { "type", "integer" }, { "description", "With start: a forca_bed_check number, if the grant allows camera bed checks and the user has not marked the bed clear." } } },
                            { "trays", { { "type", "object" }, { "description", "With start: the printer slot each filament the plate uses prints from, e.g. {\"1\": \"Ext\"} or {\"1\": \"A4\", \"2\": \"B1\"} (forca_printer_status -> filament_sources). Required." } } } } } },
        tool_request_print });

    ai.register_tool({ "forca_print_request_status", "Print request status",
        "The state of the last print request: pending, declined, approved (Forca's print dialog was opened for the "
        "user) or void (something changed, or it was replaced). Grant prints: sending, sent (Forca pressed Send), "
        "cancelled (the user stopped it) or void (a check failed; the detail says which).",
        { { "type", "object" }, { "properties", json::object() } }, tool_print_request_status });
}

}} // namespace Slic3r::GUI
