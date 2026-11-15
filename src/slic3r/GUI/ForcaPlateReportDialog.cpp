#include "ForcaPlateReportDialog.hpp"

#include "ForcaBeforeYouPrint.hpp"  // forca_option_label, forca_option_value, forca_unsaved_changes
#include "ForcaPrintRiskDialog.hpp" // forca_plate_risks, forca_show_print_risks
#include "GUI.hpp"                  // from_u8
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ForcaPlateReport.hpp"
#include "libslic3r/ForcaPreflight.hpp"
#include "libslic3r/ForcaPrintRisk.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp" // get_time_dhms, short_time

#include <wx/clipbrd.h>
#include <wx/colour.h>
#include <wx/dataobj.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

namespace {

// A plate (-1: the current one; plate_idx becomes its index), or null.
PartPlate* get_plate(int& plate_idx)
{
    Plater* p = wxGetApp().plater();
    if (!p || p->printer_technology() != ptFFF)
        return nullptr;
    PartPlateList& plates = p->get_partplate_list();
    if (plate_idx < 0)
        plate_idx = plates.get_curr_plate_index();
    return plates.get_plate(plate_idx);
}

// A number from a per-filament (or per-extruder) option, Ints or Floats; 0 when missing.
double number_at(const DynamicPrintConfig& c, const std::string& key, size_t i)
{
    if (const auto* o = c.option<ConfigOptionInts>(key); o && !o->values.empty())
        return double(o->get_at(i));
    if (const auto* o = c.option<ConfigOptionFloats>(key); o && !o->values.empty())
        return o->get_at(i);
    if (const auto* o = c.option<ConfigOptionFloat>(key))
        return o->value;
    return 0;
}

// The plate's filament slots (1-based) and presets.
std::vector<std::pair<int, std::string>> plate_filaments(PartPlate& plate, const PresetBundle& bundle)
{
    std::vector<std::pair<int, std::string>> out;
    for (int id : plate.get_extruders(false))
        if (id >= 1 && size_t(id) <= bundle.filament_presets.size())
            out.emplace_back(id, bundle.filament_presets[size_t(id - 1)]);
    return out;
}

DynamicPrintConfig plate_config(PartPlate& plate, PresetBundle& bundle)
{
    DynamicPrintConfig full = bundle.full_config();
    full.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(plate.get_bed_type(true)));
    return full;
}

ForcaPlateFacts gather_facts(PartPlate& plate, PresetBundle& bundle)
{
    ForcaPlateFacts          f;
    const DynamicPrintConfig full   = plate_config(plate, bundle);
    const Vec3d              origin = plate.get_origin();
    const BoundingBoxf3      volume = plate.get_build_volume();
    if (volume.defined)
        f.bed = BoundingBoxf(Vec2d(volume.min.x() - origin.x(), volume.min.y() - origin.y()),
                             Vec2d(volume.max.x() - origin.x(), volume.max.y() - origin.y()));
    f.max_height = number_at(full, "printable_height", 0);
    if (!plate.empty()) {
        BoundingBoxf3 objects = plate.get_objects_bounding_box();
        if (objects.defined) {
            objects.translate(-origin);
            f.objects = objects;
        }
    }
    if (const GCodeProcessorResult* r = plate.get_slice_result(); r && plate.is_slice_result_valid())
        f.toolpath_outside = r->toolpath_outside;
    f.nozzle_diameter    = number_at(full, "nozzle_diameter", 0);
    f.layer_height       = number_at(full, "layer_height", 0);
    f.first_layer_height = number_at(full, "initial_layer_print_height", 0);
    const std::string bed_key = get_bed_temp_key(plate.get_bed_type(true));
    for (const auto& [slot, name] : plate_filaments(plate, bundle)) {
        const size_t i = size_t(slot - 1);
        f.filaments.push_back({ slot, name, number_at(full, "nozzle_temperature", i),
                                number_at(full, "nozzle_temperature_initial_layer", i),
                                number_at(full, "nozzle_temperature_range_low", i),
                                number_at(full, "nozzle_temperature_range_high", i),
                                bed_key.empty() ? 0. : number_at(full, bed_key, i) });
    }
    return f;
}

// The slicer's own warnings about a sliced plate, as Plater words them, and whether each is only a tip.
std::vector<std::pair<wxString, bool>> slice_warnings(PartPlate& plate)
{
    std::vector<std::pair<wxString, bool>> out;
    GCodeProcessorResult* r = plate.get_slice_result();
    if (!r || !plate.is_slice_result_valid())
        return out;
    for (GCodeProcessorResult::SliceWarning w : r->warnings) // a copy: get_slice_warning_string takes a reference
        out.emplace_back(Plater::get_slice_warning_string(w), w.level == 0);
    return out;
}

wxString deg(double v) { return wxString::Format("%d", int(std::lround(v))); }
wxString num(double v, int decimals = 1) { return wxString::Format("%.*f", decimals, v); }

// A check in words.
wxString check_text(const ForcaPlateCheck& c)
{
    switch (c.id) {
    case ForcaPlateCheckId::OutsideBed:
        return _L("Part of the plate is outside the printable area. Move the objects onto the bed.");
    case ForcaPlateCheckId::TooTall:
        // TRN %1%: the objects' height; %2%: the printer's printable height; both in mm.
        return format_wxstr(_L("The objects are %1% mm tall, taller than the printable height (%2% mm)."), num(c.value), num(c.limit));
    case ForcaPlateCheckId::NearMaxHeight:
        // TRN %1%: the objects' height; %2%: the printer's printable height; both in mm.
        return format_wxstr(_L("The objects are %1% mm tall, close to the printable height (%2% mm)."), num(c.value), num(c.limit));
    case ForcaPlateCheckId::NearEdge:
        // TRN %1%: a distance in mm.
        return format_wxstr(_L("An object is %1% mm from the edge of the bed, where the first layer sticks least on many printers."),
                            num(c.value));
    case ForcaPlateCheckId::LayerTooThick:
        // TRN %1%: the layer height; %2%: the thickest layer the nozzle prints well (0.8 x %3%); %3%: the nozzle; all in mm.
        return format_wxstr(_L("Layer height %1% mm is above %2% mm, the most a %3% mm nozzle prints well: layers may not stick "
                               "to each other."),
                            num(c.value, 2), num(c.limit, 2), num(c.limit / FORCA_LAYER_SHARE, 2));
    case ForcaPlateCheckId::FirstLayerTooThick:
        // TRN %1%: the first layer height; %2%: the thickest layer the nozzle prints well (0.8 x %3%); %3%: the nozzle; all in mm.
        return format_wxstr(_L("First layer height %1% mm is above %2% mm, the most a %3% mm nozzle prints well: it may not "
                               "stick to the bed."),
                            num(c.value, 2), num(c.limit, 2), num(c.limit / FORCA_LAYER_SHARE, 2));
    case ForcaPlateCheckId::NozzleTempOutsideRange:
        // TRN %1%: a filament preset; %2%: a temperature; %3% and %4%: the filament's recommended range; all in °C.
        return format_wxstr(_L("%1%: nozzle temperature %2% °C is outside the filament's recommended %3% to %4% °C."),
                            from_u8(c.filament), deg(c.value), deg(c.low), deg(c.limit));
    case ForcaPlateCheckId::NozzleTempsFarApart:
        // TRN %1%: a temperature difference in °C.
        return format_wxstr(_L("The filaments' nozzle temperatures are %1% °C apart. Check that they can print together."), deg(c.value));
    case ForcaPlateCheckId::BedTempsFarApart:
        // TRN %1%: a temperature difference in °C.
        return format_wxstr(_L("The filaments' bed temperatures on this plate are %1% °C apart; the bed can only be at one of them."),
                            deg(c.value));
    }
    return {};
}

std::vector<ForcaPlateCheck> checks_of(PartPlate& plate)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    return bundle ? forca_plate_checks(gather_facts(plate, *bundle)) : std::vector<ForcaPlateCheck>{};
}

// The risk counts by type, in words.
wxString risk_summary(const std::vector<ForcaPlateRisk>& risks)
{
    std::map<PrintRiskType, size_t> by_type;
    size_t                          open = 0;
    for (const ForcaPlateRisk& r : risks) {
        ++by_type[r.risk.type];
        if (!forca_risk_handled(r.risk))
            ++open;
    }
    wxString out = format_wxstr(_L_PLURAL("%1% risk found", "%1% risks found", risks.size()), risks.size());
    wxString kinds;
    for (const auto& [type, n] : by_type) {
        wxString label;
        switch (type) {
        case PrintRiskType::Overhang: label = _L("Overhang"); break;
        case PrintRiskType::LongBridge: label = _L("Long bridge"); break;
        case PrintRiskType::FloatingIsland: label = _L("Floating island"); break;
        case PrintRiskType::ThinTall: label = _L("Thin and tall"); break;
        case PrintRiskType::SmallFootprint: label = _L("Small footprint"); break;
        case PrintRiskType::ShortLayers: label = _L("Short layers"); break;
        }
        kinds += (kinds.empty() ? "" : ", ") + wxString::Format("%s %d", label, int(n));
    }
    out += ": " + kinds + ". ";
    // TRN Plate report: how many of the Print Risks have no support, brim or raft taking care of them.
    out += format_wxstr(_L_PLURAL("%1% not taken care of.", "%1% not taken care of.", open), open);
    return out;
}

class ForcaPlateReportDialog : public DPIDialog
{
public:
    ForcaPlateReportDialog(wxWindow* parent, int plate_idx)
        : DPIDialog(parent, wxID_ANY, _L("Plate report"), wxDefaultPosition, wxDefaultSize,
                    wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
        , m_plate_idx(plate_idx)
    {
        SetBackgroundColour(*wxWHITE);
        SetFont(Label::Body_14);
        const int pad = FromDIP(10);
        auto*     root = new wxBoxSizer(wxVERTICAL);

        m_page = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(640, 520)), wxVSCROLL);
        m_page->SetScrollRate(0, FromDIP(20));
        m_page->SetBackgroundColour(*wxWHITE);
        m_body = new wxBoxSizer(wxVERTICAL);
        m_page->SetSizer(m_body);
        root->Add(m_page, 1, wxEXPAND | wxALL, pad);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        m_copy        = new Button(this, _L("Copy as text"));
        m_copy->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        m_risks = new Button(this, _L("Print Risks"));
        m_risks->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        m_close = new Button(this, _L("Close"));
        m_close->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        buttons->Add(m_copy, 0, wxRIGHT, FromDIP(8));
        buttons->Add(m_risks, 0);
        buttons->AddStretchSpacer(1);
        buttons->Add(m_close, 0);
        root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);

        m_copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { copy(); });
        m_risks->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            EndModal(wxID_CLOSE);
            // After this modal loop has ended, so the (non-modal) Print Risks window takes input.
            wxGetApp().CallAfter([] { forca_show_print_risks(wxGetApp().mainframe); });
        });
        m_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); });

        build();
        SetSizerAndFit(root);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
        apply_colours();
    }

protected:
    void on_dpi_changed(const wxRect&) override
    {
        m_copy->Rescale();
        m_risks->Rescale();
        m_close->Rescale();
        GetSizer()->SetSizeHints(this);
        Refresh();
    }

private:
    enum class Kind { Heading, Text, Warning, Note };
    struct Line
    {
        Kind     kind;
        wxString text;
    };

    void add(Kind kind, const wxString& text) { m_lines.push_back({ kind, text }); }

    // The report's lines, then a label per line.
    void build()
    {
        int           idx    = m_plate_idx;
        PartPlate*    plate  = get_plate(idx);
        PresetBundle* bundle = wxGetApp().preset_bundle;
        Plater*       p      = wxGetApp().plater();
        if (!plate || !bundle || !p) {
            add(Kind::Text, _L("The plate report is for FFF prints only."));
            return render();
        }
        PartPlateList&           plates = p->get_partplate_list();
        const DynamicPrintConfig full   = plate_config(*plate, *bundle);
        const bool               sliced = plate->is_slice_result_valid() && plate->get_slice_result() != nullptr;

        // TRN Plate report title: %1% is the plate number.
        wxString title = format_wxstr(_L("Plate %1%"), idx + 1);
        if (!plate->get_plate_name().empty())
            title += " - " + from_u8(plate->get_plate_name());
        add(Kind::Heading, title);
        if (plate->empty()) {
            add(Kind::Text, _L("This plate is empty."));
            return render();
        }

        // 1. Checks: Forca's rules, then the slicer's own warnings.
        add(Kind::Heading, _L("Checks"));
        size_t shown = 0;
        for (const ForcaPlateCheck& c : checks_of(*plate)) {
            add(c.warning ? Kind::Warning : Kind::Note, check_text(c));
            ++shown;
        }
        for (const auto& [text, tip] : slice_warnings(*plate)) {
            add(tip ? Kind::Note : Kind::Warning, text);
            ++shown;
        }
        if (shown == 0)
            add(Kind::Text, sliced ? _L("Nothing to point out.") : _L("Nothing to point out. Slice the plate to add the slicer's own checks."));

        // 2. Print Risks.
        add(Kind::Heading, _L("Print Risks"));
        std::vector<ForcaPlateRisk> risks;
        if (!forca_plate_risks(idx, risks))
            add(Kind::Text, _L("Slice the plate to check it for risks."));
        else if (risks.empty())
            add(Kind::Text, _L("No risks found on this plate."));
        else
            add(Kind::Text, risk_summary(risks) + " " + _L("The Print Risks button shows where and what to try."));

        // 3. The print: time, filament, layers.
        add(Kind::Heading, _L("Print"));
        PrintBase*            base   = nullptr;
        GCodeProcessorResult* result = nullptr;
        int                   print_index = -1;
        plate->get_print(&base, &result, &print_index);
        const Print* print = dynamic_cast<const Print*>(base);
        if (!sliced || !print || !result)
            add(Kind::Text, _L("Slice the plate to see its print time and weight."));
        else {
            const PrintStatistics& ps = print->print_statistics();
            add(Kind::Text, _L("Print time:") + " " + from_u8(short_time(get_time_dhms(float(result->print_statistics.modes[0].time)))));
            // TRN Plate report: %1% grams and %2% metres of filament.
            add(Kind::Text, _L("Filament:") + " " + format_wxstr(_L("%1% g, %2% m"), num(ps.total_weight), num(ps.total_used_filament / 1000., 2)));
            if (ps.total_cost > 0)
                add(Kind::Text, _L("Cost:") + " " + num(ps.total_cost, 2));
            size_t layers = 0;
            for (const PrintObject* o : print->objects())
                layers = std::max(layers, o->layers().size());
            add(Kind::Text, _L("Layers:") + " " + wxString::Format("%d", int(layers)));
            if (ps.total_toolchanges > 0)
                add(Kind::Text, _L("Filament changes:") + " " + wxString::Format("%d", ps.total_toolchanges));
        }

        // 4. Where it stands.
        add(Kind::Heading, _L("Plate"));
        const ForcaPlateFacts facts = gather_facts(*plate, *bundle);
        if (facts.objects.defined) {
            const Vec3d size = facts.objects.size();
            // TRN Plate report: the size of everything on the plate, width x depth x height in mm.
            add(Kind::Text, _L("Size:") + " " + format_wxstr(_L("%1% x %2% x %3% mm"), num(size.x()), num(size.y()), num(size.z())));
            const Vec3d centre = facts.objects.center();
            if (facts.bed.defined) {
                const Vec2d bed_centre = facts.bed.center();
                // TRN Plate report: where the objects' centre is, and the bed's centre; X and Y in mm.
                add(Kind::Text, _L("Position:") + " " +
                                    format_wxstr(_L("centre at X %1%, Y %2% (bed centre X %3%, Y %4%)"), num(centre.x()), num(centre.y()),
                                                 num(bed_centre.x()), num(bed_centre.y())));
                const Vec2d bed = facts.bed.size();
                add(Kind::Text, _L("Bed:") + " " + format_wxstr(_L("%1% x %2% mm"), num(bed.x(), 0), num(bed.y(), 0)) + ", " +
                                    forca_option_value("curr_bed_type", full.opt_serialize("curr_bed_type")));
            }
        }
        add(Kind::Text, _L("Objects:") + " " + wxString::Format("%d", int(plate->printable_instance_size())));
        if (plates.get_plate_count() > 1)
            add(Kind::Note, format_wxstr(_L_PLURAL("The project has %1% plate; this report covers one.",
                                                   "The project has %1% plates; this report covers one.", plates.get_plate_count()),
                                         plates.get_plate_count()));

        // 5. Presets and the settings that most often decide a print.
        add(Kind::Heading, _L("Presets"));
        const wxString printer = bundle->physical_printers.has_selection() ?
                                     from_u8(bundle->physical_printers.get_selected_printer_name()) :
                                     from_u8(bundle->printers.get_edited_preset().name);
        add(Kind::Text, _L("Printer:") + " " + printer);
        add(Kind::Text, _L("Process:") + " " + from_u8(bundle->prints.get_edited_preset().name));
        wxString process;
        for (const ForcaPreflightValue& v : forca_preflight_process_details(full))
            process += (process.empty() ? "" : wxString::FromUTF8(" \xC2\xB7 ")) + forca_option_label(v.key) + ": " +
                       forca_option_value(v.key, v.value);
        add(Kind::Text, process);
        for (const auto& [slot, name] : plate_filaments(*plate, *bundle)) {
            // TRN Plate report: %1% is a filament slot number, %2% its filament preset.
            wxString line = format_wxstr(_L("Filament %1%: %2%"), slot, from_u8(name));
            bool     first = true;
            for (const ForcaPreflightValue& v : forca_preflight_filament_details(full, size_t(slot - 1))) {
                line += (first ? " - " : ", ") + forca_option_label(v.key) + " " + forca_option_value(v.key, v.value);
                first = false;
            }
            add(Kind::Text, line);
        }

        // 6. Settings changed and not saved.
        add(Kind::Heading, _L("Changed and not saved"));
        const std::vector<ForcaPreflightValue> changes = forca_unsaved_changes(*bundle);
        if (changes.empty())
            add(Kind::Text, _L("None. The presets are as saved."));
        for (const ForcaPreflightValue& v : changes)
            add(Kind::Note, format_wxstr(_L("%1% %2% (preset %3%)"), forca_option_label(v.key), forca_option_value(v.key, v.value),
                                         forca_option_value(v.key, v.saved)));
        render();
    }

    void render()
    {
        const int wrap = FromDIP(600);
        for (const Line& l : m_lines) {
            wxString text = l.text;
            if (l.kind == Kind::Warning)
                text = wxString::FromUTF8("\xE2\x9A\xA0 ") + text; // warning sign
            else if (l.kind == Kind::Note)
                text = wxString::FromUTF8("\xE2\x80\xA2 ") + text; // bullet
            auto* t = new wxStaticText(m_page, wxID_ANY, text);
            t->SetFont(l.kind == Kind::Heading ? Label::Head_14 : Label::Body_13);
            t->Wrap(wrap);
            m_body->Add(t, 0, wxBOTTOM | (l.kind == Kind::Heading ? wxTOP : 0), FromDIP(l.kind == Kind::Heading ? 6 : 3));
            m_labels.push_back({ t, l.kind });
        }
        m_page->FitInside();
    }

    // The hand-picked colours, after the dark-mode pass that resets every label's.
    void apply_colours()
    {
        for (const auto& [label, kind] : m_labels)
            if (kind == Kind::Warning)
                label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#C25E00")));
            else if (kind == Kind::Note)
                label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B")));
    }

    void copy()
    {
        wxString text;
        for (const Line& l : m_lines) {
            if (l.kind == Kind::Heading && !text.empty())
                text += "\n";
            text += (l.kind == Kind::Warning ? "! " : l.kind == Kind::Note ? "- " : "") + l.text + "\n";
        }
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(text));
            wxTheClipboard->Close();
        }
    }

    int                                     m_plate_idx;
    wxScrolledWindow*                       m_page  = nullptr;
    wxBoxSizer*                             m_body  = nullptr;
    Button*                                 m_copy  = nullptr;
    Button*                                 m_risks = nullptr;
    Button*                                 m_close = nullptr;
    std::vector<Line>                       m_lines;
    std::vector<std::pair<wxStaticText*, Kind>> m_labels;
};

} // namespace

std::vector<ForcaPlateCheck> forca_plate_report_checks(int plate_idx)
{
    PartPlate* plate = get_plate(plate_idx);
    return plate && !plate->empty() ? checks_of(*plate) : std::vector<ForcaPlateCheck>{};
}

ForcaPlateReportSummary forca_plate_report_summary(int plate_idx)
{
    ForcaPlateReportSummary s;
    PartPlate*              plate = get_plate(plate_idx);
    if (!plate || plate->empty())
        return s;
    s.ready = true;
    for (const ForcaPlateCheck& c : checks_of(*plate))
        if (c.warning)
            ++s.warnings;
    for (const auto& [text, tip] : slice_warnings(*plate))
        if (!tip)
            ++s.warnings;
    return s;
}

void forca_show_plate_report(wxWindow* parent, int plate_idx)
{
    ForcaPlateReportDialog dlg(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), plate_idx);
    dlg.ShowModal();
}

}} // namespace Slic3r::GUI
