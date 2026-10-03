#include "ForcaCalibrationStore.hpp"

#include "libslic3r/Utils.hpp" // Slic3r::data_dir()

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <ctime>

namespace Slic3r { namespace GUI {

using json = nlohmann::json;
namespace fs = boost::filesystem;

bool ForcaCalibrationStore::Key::operator==(const Key& o) const
{
    return printer == o.printer && nozzle == o.nozzle && filament == o.filament && calibration == o.calibration;
}

static std::string today_iso()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return std::string(buf);
}

ForcaCalibrationStore::ForcaCalibrationStore()
{
    load();
}

std::string ForcaCalibrationStore::file_path()
{
    return (fs::path(Slic3r::data_dir()) / "forca" / "calibration_results.json").string();
}

void ForcaCalibrationStore::load()
{
    m_entries.clear();
    m_runs.clear();
    try {
        const std::string p = file_path();
        if (!fs::exists(p))
            return;
        std::ifstream in(p);
        if (!in.good())
            return;
        json j;
        in >> j;
        // "runs" was added after "entries"; files without it (or with no entries) still load.
        if (j.contains("runs") && j["runs"].is_array()) {
            for (const auto& r : j["runs"]) {
                Run run;
                run.printer    = r.value("printer", std::string());
                run.nozzle     = r.value("nozzle", std::string());
                run.target     = r.value("target", std::string());
                run.base       = r.value("base", std::string());
                run.started_at = r.value("started_at", std::string());
                if (r.contains("before") && r["before"].is_object())
                    for (auto it = r["before"].begin(); it != r["before"].end(); ++it)
                        if (it.value().is_number())
                            run.before[it.key()] = it.value().get<double>();
                run.kind = r.value("kind", std::string());
                if (r.contains("before_text") && r["before_text"].is_object())
                    for (auto it = r["before_text"].begin(); it != r["before_text"].end(); ++it)
                        if (it.value().is_string())
                            run.before_text[it.key()] = it.value().get<std::string>();
                m_runs.push_back(std::move(run));
            }
        }
        if (!j.contains("entries") || !j["entries"].is_array())
            return;
        for (const auto& e : j["entries"]) {
            Entry ent;
            ent.key.printer        = e.value("printer", std::string());
            ent.key.nozzle         = e.value("nozzle", std::string());
            ent.key.filament       = e.value("filament", std::string());
            ent.key.calibration    = e.value("calibration", std::string());
            ent.rec.status         = e.value("status", std::string());
            ent.rec.start          = e.value("start", 0.0);
            ent.rec.end            = e.value("end", 0.0);
            ent.rec.value          = e.value("value", 0.0);
            ent.rec.pass           = e.value("pass", 0);
            ent.rec.derived_preset = e.value("derived_preset", std::string());
            ent.rec.updated_at     = e.value("updated_at", std::string());
            if (e.contains("values") && e["values"].is_object())
                for (auto it = e["values"].begin(); it != e["values"].end(); ++it)
                    if (it.value().is_number())
                        ent.rec.values[it.key()] = it.value().get<double>();
            ent.rec.note = e.value("note", std::string());
            m_entries.push_back(std::move(ent));
        }
    } catch (...) {
        // Corrupt / unreadable file -> start from an empty store rather than failing.
        m_entries.clear();
        m_runs.clear();
    }
}

void ForcaCalibrationStore::save() const
{
    try {
        json j;
        j["version"] = 1;
        j["entries"] = json::array();
        for (const auto& ent : m_entries) {
            json e;
            e["printer"]        = ent.key.printer;
            e["nozzle"]         = ent.key.nozzle;
            e["filament"]       = ent.key.filament;
            e["calibration"]    = ent.key.calibration;
            e["status"]         = ent.rec.status;
            e["start"]          = ent.rec.start;
            e["end"]            = ent.rec.end;
            e["value"]          = ent.rec.value;
            e["pass"]           = ent.rec.pass;
            e["derived_preset"] = ent.rec.derived_preset;
            e["updated_at"]     = ent.rec.updated_at;
            if (!ent.rec.values.empty()) {
                e["values"] = json::object();
                for (const auto& kv : ent.rec.values)
                    e["values"][kv.first] = kv.second;
            }
            if (!ent.rec.note.empty())
                e["note"] = ent.rec.note;
            j["entries"].push_back(e);
        }
        j["runs"] = json::array();
        for (const auto& run : m_runs) {
            json r;
            r["printer"]    = run.printer;
            r["nozzle"]     = run.nozzle;
            r["target"]     = run.target;
            r["base"]       = run.base;
            r["started_at"] = run.started_at;
            r["before"]     = json::object();
            for (const auto& kv : run.before)
                r["before"][kv.first] = kv.second;
            if (!run.kind.empty())
                r["kind"] = run.kind;
            if (!run.before_text.empty()) {
                r["before_text"] = json::object();
                for (const auto& kv : run.before_text)
                    r["before_text"][kv.first] = kv.second;
            }
            j["runs"].push_back(r);
        }
        const fs::path dir = fs::path(Slic3r::data_dir()) / "forca";
        boost::system::error_code ec;
        fs::create_directories(dir, ec);
        std::ofstream out((dir / "calibration_results.json").string(), std::ios::trunc);
        out << j.dump(2);
    } catch (...) {
        // Best-effort persistence; a write failure must not disrupt the wizard.
    }
}

ForcaCalibrationStore::Entry* ForcaCalibrationStore::find(const Key& key)
{
    for (auto& e : m_entries)
        if (e.key == key)
            return &e;
    return nullptr;
}

bool ForcaCalibrationStore::get(const Key& key, Record& out) const
{
    for (const auto& e : m_entries)
        if (e.key == key) {
            out = e.rec;
            return true;
        }
    return false;
}

void ForcaCalibrationStore::set_pending(const Key& key, double start, double end)
{
    Entry* e = find(key);
    if (!e) {
        m_entries.push_back(Entry{ key, Record{} });
        e = &m_entries.back();
    }
    e->rec.status     = "pending";
    e->rec.start      = start;
    e->rec.end        = end;
    e->rec.updated_at = today_iso();
    save();
}

bool ForcaCalibrationStore::is_calibration_target(const std::string& printer, const std::string& nozzle, const std::string& filament) const
{
    if (filament.empty())
        return false;
    for (const auto& e : m_entries)
        // Only a DONE result makes a preset a run target (a skip must never turn the user's own preset into one).
        if (e.key.printer == printer && e.key.nozzle == nozzle && e.rec.status == "done" && e.rec.derived_preset == filament)
            return true;
    return false;
}

void ForcaCalibrationStore::set_done(const Key& key, double value, const std::string& derived_preset, int pass)
{
    Entry* e = find(key);
    if (!e) {
        m_entries.push_back(Entry{ key, Record{} });
        e = &m_entries.back();
    }
    e->rec.status         = "done";
    e->rec.value          = value;
    e->rec.pass           = pass;
    e->rec.derived_preset = derived_preset;
    e->rec.updated_at     = today_iso();
    save();
}

void ForcaCalibrationStore::set_skipped(const Key& key, const std::string& derived_preset)
{
    Entry* e = find(key);
    if (!e) {
        m_entries.push_back(Entry{ key, Record{} });
        e = &m_entries.back();
    }
    e->rec.status         = "skipped";
    e->rec.derived_preset = derived_preset;
    e->rec.updated_at     = today_iso();
    save();
}

void ForcaCalibrationStore::clear(const Key& key)
{
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&key](const Entry& e) { return e.key == key; });
    if (it != m_entries.end()) {
        m_entries.erase(it);
        save();
    }
}

void ForcaCalibrationStore::forget_filament(const std::string& printer, const std::string& nozzle, const std::string& filament)
{
    const size_t before = m_entries.size() + m_runs.size();
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), [&](const Entry& e) {
        return e.key.printer == printer && e.key.nozzle == nozzle && e.key.filament == filament;
    }), m_entries.end());
    m_runs.erase(std::remove_if(m_runs.begin(), m_runs.end(), [&](const Run& r) {
        return r.printer == printer && r.nozzle == nozzle && r.target == filament;
    }), m_runs.end());
    if (m_entries.size() + m_runs.size() != before)
        save();
}

bool ForcaCalibrationStore::get_run(const std::string& printer, const std::string& nozzle, const std::string& target, Run& out) const
{
    for (const auto& r : m_runs)
        if (r.kind.empty() && r.printer == printer && r.nozzle == nozzle && r.target == target) { // filament runs only
            out = r;
            return true;
        }
    return false;
}

void ForcaCalibrationStore::start_run(const Run& run)
{
    Run existing;
    if (run.target.empty() || (run.kind.empty() ? get_run(run.printer, run.nozzle, run.target, existing)
                                                : get_printer_run(run.target, existing)))
        return; // a run's "before" is fixed at its first Apply
    Run r = run;
    if (r.started_at.empty())
        r.started_at = today_iso();
    m_runs.push_back(std::move(r));
    save();
}

void ForcaCalibrationStore::set_record(const Key& key, const Record& rec)
{
    Entry* e = find(key);
    if (!e) {
        m_entries.push_back(Entry{ key, Record{} });
        e = &m_entries.back();
    }
    e->rec            = rec;
    e->rec.updated_at = today_iso();
    save();
}

bool ForcaCalibrationStore::get_printer_run(const std::string& printer_preset, Run& out) const
{
    for (const auto& r : m_runs)
        if (r.kind == "printer" && r.target == printer_preset) {
            out = r;
            return true;
        }
    return false;
}

void ForcaCalibrationStore::forget_printer_run(const std::string& printer_preset)
{
    const size_t before = m_runs.size();
    m_runs.erase(std::remove_if(m_runs.begin(), m_runs.end(), [&](const Run& r) {
        return r.kind == "printer" && r.target == printer_preset;
    }), m_runs.end());
    if (m_runs.size() != before)
        save();
}

}} // namespace Slic3r::GUI
