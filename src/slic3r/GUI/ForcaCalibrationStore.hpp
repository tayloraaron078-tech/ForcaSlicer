#ifndef slic3r_ForcaCalibrationStore_hpp_
#define slic3r_ForcaCalibrationStore_hpp_

#include <map>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

// Persistent record of calibration progress + results, stored as JSON in the datadir
// (<datadir>/forca/calibration_results.json). It makes the wizard multi-session/resumable: a
// calibration requires an offline print (minutes-hours), so the wizard records a "pending" test when
// a tower is generated and the recorded "done" result when the user applies one. Keyed by
// printer + nozzle + filament + calibration. This is the seed of the calibration results database.
class ForcaCalibrationStore
{
public:
    struct Key {
        std::string printer;
        std::string nozzle;
        std::string filament;
        std::string calibration; // e.g. "temperature"
        bool operator==(const Key& o) const;
    };
    struct Record {
        std::string status;          // "pending" | "done" | "skipped" | ""
        double      start = 0;       // pending tower range (units per calibration)
        double      end   = 0;
        double      value = 0;       // done result value (int-valued for temperature, fractional for MVS etc.)
        int         pass  = 0;       // multi-pass calibrations (flow): passes completed; 0 = not tracked (older files)
        std::string derived_preset;  // preset the result was written to
        std::string updated_at;      // ISO date (YYYY-MM-DD)
    };

    // One calibration run = one target ("calibrated") filament preset on a printer + nozzle. `before` holds the
    // values the run started from (snapshotted from the source preset at the first Apply), for the before/after
    // table; the target preset itself is overwritten as the run progresses.
    struct Run {
        std::string                   printer;
        std::string                   nozzle;
        std::string                   target;      // the run's calibrated filament preset
        std::string                   base;        // the preset the run started from
        std::string                   started_at;  // ISO date
        std::map<std::string, double> before;      // config key -> value (absent = not set / nil)
    };

    ForcaCalibrationStore();         // loads from the datadir

    bool get(const Key& key, Record& out) const;
    void set_pending(const Key& key, double start, double end);
    void set_done(const Key& key, double value, const std::string& derived_preset, int pass = 0);
    void set_skipped(const Key& key, const std::string& derived_preset);
    void clear(const Key& key); // forget a record (e.g. un-skip)
    // Forget every record and the run of a filament preset on a printer/nozzle (a new preset is being created
    // under a name that a deleted preset used before -- its old history must not carry over).
    void forget_filament(const std::string& printer, const std::string& nozzle, const std::string& filament);

    // Runs. start_run() only records the first time for a given printer/nozzle/target (a run's "before" is fixed).
    bool get_run(const std::string& printer, const std::string& nozzle, const std::string& target, Run& out) const;
    void start_run(const Run& run);

    // True if `filament` is a profile a prior calibration wrote its result into (on this printer/nozzle),
    // i.e. it is an existing calibration target that further steps should accumulate into rather than
    // deriving a new profile from.
    bool is_calibration_target(const std::string& printer, const std::string& nozzle, const std::string& filament) const;

private:
    struct Entry { Key key; Record rec; };

    static std::string file_path();
    void   load();
    void   save() const;
    Entry* find(const Key& key);

    std::vector<Entry> m_entries;
    std::vector<Run>   m_runs;
};

}} // namespace Slic3r::GUI

#endif // slic3r_ForcaCalibrationStore_hpp_
