// Forca: what importing a preset file would do, worked out without changing anything (libslic3r/ForcaPresetImport).
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <catch2/catch_all.hpp>

#include "catch2/catch_test_macros.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ForcaPresetImport.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <fstream>
#include <miniz.h>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// A system preset of the collection's type, loaded in memory.
Preset &add_system_preset(PresetCollection &collection, const std::string &name)
{
    Preset &preset   = collection.load_preset(std::string(), name, collection.default_preset().config, /*select=*/false);
    preset.is_system = true;
    return preset;
}

Preset &add_system_filament(PresetBundle &bundle, const std::string &name, const std::string &vendor, const std::string &type,
                            std::vector<std::string> printers)
{
    Preset &preset = add_system_preset(bundle.filaments, name);
    preset.config.option<ConfigOptionStrings>("filament_vendor", true)->values     = {vendor};
    preset.config.option<ConfigOptionStrings>("filament_type", true)->values       = {type};
    preset.config.option<ConfigOptionStrings>("compatible_printers", true)->values = std::move(printers);
    return preset;
}

// A filament preset file: name, version and inherits, then the given settings (JSON members).
std::string write_filament(const fs::path &dir, const std::string &name, const std::string &inherits, const std::string &settings = {})
{
    const fs::path file = dir / (name + ".json");
    fs::create_directories(dir);
    std::ofstream(file.string()) << R"({"type":"filament","name":")" << name << R"(","from":"User","version":"2.7.0.7","inherits":")"
                                 << inherits << R"(","filament_settings_id":[")" << name << R"("])" << (settings.empty() ? "" : ",")
                                 << settings << '}';
    return file.string();
}

bool has_pair(const std::vector<std::pair<std::string, std::string>> &pairs, const std::string &first, const std::string &second)
{
    return std::find(pairs.begin(), pairs.end(), std::make_pair(first, second)) != pairs.end();
}

} // namespace

TEST_CASE("A preset whose parent is found lists only the settings it changes, next to the parent's value", "[ForcaPresetImport]")
{
    ScopedTemporaryDir dir;
    PresetBundle       bundle;
    Preset &parent = add_system_filament(bundle, "Generic PLA @Test", "Generic", "PLA", {});
    parent.config.option<ConfigOptionInts>("nozzle_temperature", true)->values = {210};
    parent.config.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = {12.};
    parent.config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = {"Direct Drive Standard", "Direct Drive High Flow"};

    const PresetImportItem item = forca_analyse_preset_file(
        bundle, write_filament(dir.path(), "Brand PLA", parent.name,
                               R"("nozzle_temperature":["230"],"filament_max_volumetric_speed":["12"],)"
                               R"("filament_extruder_variant":["Direct Drive Standard"])"));

    REQUIRE(item.readable);
    CHECK(item.type == Preset::TYPE_FILAMENT);
    CHECK(item.parent == PresetImportItem::Parent::Found);
    CHECK(item.best_parent == parent.name);
    CHECK(item.source == PresetImportItem::Source::OrcaOrForca);
    // Max volumetric speed is the parent's value already, and the variant list only lays the values out, so only the
    // temperature is a change.
    REQUIRE(item.overrides.size() == 1);
    CHECK(item.overrides[0].key == "nozzle_temperature");
    CHECK(item.overrides[0].value == "230");
    CHECK(item.overrides[0].parent_value == "210");
}

TEST_CASE("Bambu Studio settings Forca renames or doesn't have are reported with their values", "[ForcaPresetImport]")
{
    ScopedTemporaryDir dir;
    PresetBundle       bundle;
    add_system_filament(bundle, "Generic PETG @Test", "Generic", "PETG", {});

    const PresetImportItem item = forca_analyse_preset_file(
        bundle, write_filament(dir.path(), "Brand PETG", "Generic PETG @Test",
                               R"("no_slow_down_for_cooling_on_outwalls":["1"],"hole_coef_2":["-0.025"])"));

    REQUIRE(item.readable);
    CHECK(item.source == PresetImportItem::Source::BambuStudio);
    CHECK(has_pair(item.renamed, "no_slow_down_for_cooling_on_outwalls", "dont_slow_down_outer_wall"));
    CHECK(has_pair(item.unsupported, "hole_coef_2", "-0.025"));
}

TEST_CASE("A Bambu Studio nil for a nozzle variant marks the file as Bambu Studio's", "[ForcaPresetImport]")
{
    ScopedTemporaryDir dir;
    PresetBundle       bundle;
    add_system_filament(bundle, "Generic ABS @Test", "Generic", "ABS", {});

    const PresetImportItem item = forca_analyse_preset_file(
        bundle, write_filament(dir.path(), "Brand ABS", "Generic ABS @Test", R"("nozzle_temperature":["290","nil"])"));

    REQUIRE(item.readable);
    CHECK(item.source == PresetImportItem::Source::BambuStudio);
}

TEST_CASE("A missing parent is matched to the system filament of the same name, vendor or type for the printers", "[ForcaPresetImport]")
{
    PresetBundle bundle;
    add_system_filament(bundle, "Generic PETG @Printer A", "Generic", "PETG", {"Printer A"});
    add_system_filament(bundle, "Brand PETG @Printer A", "Brand", "PETG", {"Printer A"});
    add_system_filament(bundle, "Brand PETG HF @Printer B", "Brand", "PETG", {"Printer B"});
    add_system_filament(bundle, "Generic PLA @Printer A", "Generic", "PLA", {"Printer A"});

    // The same name for another printer first, then the vendor, then Generic, only among filaments for the printers.
    CHECK(forca_best_filament_parent(bundle, "Brand PETG @Printer Z", "Other", "PETG", {"Printer A"}) == "Brand PETG @Printer A");
    CHECK(forca_best_filament_parent(bundle, "Shop PETG @Printer Z", "Brand", "PETG", {"Printer A"}) == "Brand PETG @Printer A");
    CHECK(forca_best_filament_parent(bundle, "Shop PETG @Printer Z", "Shop", "PETG", {"Printer A"}) == "Generic PETG @Printer A");
    CHECK(forca_best_filament_parent(bundle, "Shop PETG @Printer Z", "Shop", "PETG", {"Printer B"}) == "Brand PETG HF @Printer B");
    CHECK(forca_best_filament_parent(bundle, "Shop ASA @Printer A", "Shop", "ASA", {"Printer A"}).empty());
}

TEST_CASE("A file whose parent Forca doesn't have gets a suggested parent", "[ForcaPresetImport]")
{
    ScopedTemporaryDir dir;
    PresetBundle       bundle;
    add_system_filament(bundle, "Generic TPU @Printer A", "Generic", "TPU", {"Printer A"});

    const PresetImportItem item = forca_analyse_preset_file(
        bundle, write_filament(dir.path(), "Shop TPU", "Shop TPU Base",
                               R"("filament_type":["TPU"],"compatible_printers":["Printer A"])"));

    REQUIRE(item.readable);
    CHECK(item.parent == PresetImportItem::Parent::Missing);
    CHECK(item.inherits == "Shop TPU Base");
    CHECK(item.best_parent == "Generic TPU @Printer A");
}

TEST_CASE("A file that relies on its missing parent for the filament type gets the type from its name", "[ForcaPresetImport]")
{
    ScopedTemporaryDir dir;
    PresetBundle       bundle;
    add_system_filament(bundle, "Generic TPU @Printer A", "Generic", "TPU", {"Printer A"});
    add_system_filament(bundle, "Generic PLA @Printer A", "Generic", "PLA", {"Printer A"});

    const PresetImportItem item = forca_analyse_preset_file(
        bundle, write_filament(dir.path(), "Shop High Speed TPU", "Shop TPU Universal", R"("compatible_printers":["Printer A"])"));

    REQUIRE(item.parent == PresetImportItem::Parent::Missing);
    CHECK(item.best_parent == "Generic TPU @Printer A");
    CHECK(item.filament_type == "TPU");
}

TEST_CASE("The parents offered for a filament are the system filaments of its type for its printers", "[ForcaPresetImport]")
{
    PresetBundle bundle;
    add_system_filament(bundle, "Generic PETG @Printer A", "Generic", "PETG", {"Printer A"});
    add_system_filament(bundle, "Brand PETG @Printer B", "Brand", "PETG", {"Printer B"});
    add_system_filament(bundle, "Generic PLA @Printer A", "Generic", "PLA", {"Printer A"});
    add_system_filament(bundle, "Any PETG @System", "Generic", "PETG", {});

    CHECK(forca_filament_parent_candidates(bundle, "PETG", {"Printer A"}) ==
          std::vector<std::string>{"Any PETG @System", "Generic PETG @Printer A"});
    CHECK(forca_filament_parent_candidates(bundle, "", {"Printer A"}).size() == 3);
}

TEST_CASE("A Generic filament made for another printer is matched to the filament library's", "[ForcaPresetImport]")
{
    ScopedTemporaryDir dir;
    PresetBundle       bundle;
    add_system_filament(bundle, "Generic PLA @System", "Generic", "PLA", {});

    const PresetImportItem item = forca_analyse_preset_file(bundle, write_filament(dir.path(), "My PLA", "Generic PLA @BBL X1C"));

    REQUIRE(item.readable);
    CHECK(item.parent == PresetImportItem::Parent::Matched);
    CHECK(item.best_parent == "Generic PLA @System");
}

TEST_CASE("A file that names no parent is a preset of its own", "[ForcaPresetImport]")
{
    ScopedTemporaryDir dir;
    PresetBundle       bundle;

    const PresetImportItem item = forca_analyse_preset_file(bundle, write_filament(dir.path(), "Own PLA", "", R"("nozzle_temperature":["215"])"));

    REQUIRE(item.readable);
    CHECK(item.parent == PresetImportItem::Parent::None);
    CHECK(item.best_parent.empty());
}

TEST_CASE("Printers the file names that aren't installed are reported", "[ForcaPresetImport]")
{
    ScopedTemporaryDir dir;
    PresetBundle       bundle;
    add_system_preset(bundle.printers, "Printer A");
    add_system_preset(bundle.printers, "Printer H").is_visible = false; // a model the user didn't install

    const PresetImportItem item = forca_analyse_preset_file(
        bundle, write_filament(dir.path(), "Own PLA", "", R"("compatible_printers":["Printer A","Printer H","Printer Z"])"));

    REQUIRE(item.readable);
    CHECK(item.printers == std::vector<std::string>{"Printer A", "Printer H", "Printer Z"});
    CHECK(item.missing_printers == std::vector<std::string>{"Printer H", "Printer Z"});
}

TEST_CASE("A name a system or user preset already has is reported", "[ForcaPresetImport]")
{
    ScopedTemporaryDir dir;
    PresetBundle       bundle;
    add_system_filament(bundle, "Generic PLA @Test", "Generic", "PLA", {});
    bundle.filaments.load_preset(std::string(), "My PLA", bundle.filaments.default_preset().config, /*select=*/false);

    const PresetImportItem system_name = forca_analyse_preset_file(bundle, write_filament(dir.path(), "Generic PLA @Test", ""));
    CHECK(system_name.clashes_with_system_preset);
    CHECK_FALSE(system_name.clashes_with_user_preset);

    const PresetImportItem user_name = forca_analyse_preset_file(bundle, write_filament(dir.path(), "My PLA", ""));
    CHECK(user_name.clashes_with_user_preset);
    CHECK_FALSE(user_name.clashes_with_system_preset);
}

TEST_CASE("A file without a version or that isn't JSON can't be read as a preset", "[ForcaPresetImport]")
{
    ScopedTemporaryDir dir;
    PresetBundle       bundle;
    const fs::path     no_version = dir.path() / "no version.json";
    std::ofstream(no_version.string()) << R"({"type":"filament","name":"No Version","filament_settings_id":["No Version"]})";
    const fs::path not_json = dir.path() / "not json.json";
    std::ofstream(not_json.string()) << "this is not a preset";

    CHECK_FALSE(forca_analyse_preset_file(bundle, no_version.string()).readable);
    CHECK_FALSE(forca_analyse_preset_file(bundle, not_json.string()).readable);
}

namespace {

// Imports land in a user preset folder of a temporary data dir.
struct ImportTarget
{
    ScopedTemporaryDir tmp;
    std::string        previous{data_dir()};
    PresetBundle       bundle;

    ImportTarget()
    {
        set_data_dir(tmp.path().string());
        fs::create_directories(tmp.path() / "user");
        bundle.filaments.update_user_presets_directory((tmp.path() / "user").string(), PRESET_FILAMENT_NAME);
    }
    ~ImportTarget() { set_data_dir(previous); }
};

} // namespace

TEST_CASE("An import uses the name, parent and printers the user chose", "[ForcaPresetImport]")
{
    ImportTarget target;
    add_system_filament(target.bundle, "Generic TPU @Printer A", "Generic", "TPU", {"Printer A", "Printer B"});
    const PresetImportItem item = forca_analyse_preset_file(
        target.bundle, write_filament(target.tmp.path() / "download", "Shop TPU", "Shop TPU Base", R"("filament_type":["TPU"])"));
    REQUIRE(item.parent == PresetImportItem::Parent::Missing);

    PresetImportChoice choice;
    choice.name     = "Shop TPU (mine)";
    choice.parent   = item.best_parent;
    choice.printers = {"Printer A"};
    REQUIRE(forca_import_preset(target.bundle, item, choice));

    const Preset *imported = target.bundle.filaments.find_preset("Shop TPU (mine)", false);
    REQUIRE(imported != nullptr);
    CHECK(imported->config.opt_string("inherits") == "Generic TPU @Printer A");
    CHECK(imported->config.option<ConfigOptionStrings>("compatible_printers")->values == std::vector<std::string>{"Printer A"});
    CHECK(target.bundle.filaments.find_preset("Shop TPU", false) == nullptr);
}

TEST_CASE("Settings Forca doesn't have are kept in the imported filament's notes", "[ForcaPresetImport]")
{
    ImportTarget target;
    add_system_filament(target.bundle, "Generic PETG @Test", "Generic", "PETG", {});
    const PresetImportItem item = forca_analyse_preset_file(
        target.bundle, write_filament(target.tmp.path() / "download", "Brand PETG", "Generic PETG @Test",
                                      R"("filament_notes":["Dry before use."],"hole_coef_2":["-0.025"])"));

    PresetImportChoice choice;
    choice.notes_heading = "Not in Forca:";
    REQUIRE(forca_import_preset(target.bundle, item, choice));

    const Preset *imported = target.bundle.filaments.find_preset("Brand PETG", false);
    REQUIRE(imported != nullptr);
    const std::string notes = imported->config.option<ConfigOptionStrings>("filament_notes")->values.front();
    CHECK(notes == "Dry before use.\n\nNot in Forca:\nhole_coef_2 = -0.025");
}

TEST_CASE("A name a preset already has gets a suffix, and an existing user preset is replaced only when chosen", "[ForcaPresetImport]")
{
    ImportTarget target;
    add_system_filament(target.bundle, "Generic PLA @Test", "Generic", "PLA", {});
    const std::string file = write_filament(target.tmp.path() / "download", "My PLA", "Generic PLA @Test", R"("nozzle_temperature":["215"])");
    REQUIRE(forca_import_preset(target.bundle, forca_analyse_preset_file(target.bundle, file), {}));

    CHECK(forca_free_preset_name(target.bundle.filaments, "Brand PLA", "%1% (imported)") == "Brand PLA");
    CHECK(forca_free_preset_name(target.bundle.filaments, "My PLA", "%1% (imported)") == "My PLA (imported)");

    // Importing again under the same name keeps the existing preset unless replacing was chosen.
    const std::string changed = write_filament(target.tmp.path() / "download2", "My PLA", "Generic PLA @Test", R"("nozzle_temperature":["225"])");
    const PresetImportItem again = forca_analyse_preset_file(target.bundle, changed);
    CHECK(again.clashes_with_user_preset);
    CHECK_FALSE(forca_import_preset(target.bundle, again, {}));
    CHECK(target.bundle.filaments.find_preset("My PLA", false)->config.option<ConfigOptionInts>("nozzle_temperature")->values.front() == 215);

    PresetImportChoice replace;
    replace.replace_existing = true;
    REQUIRE(forca_import_preset(target.bundle, again, replace));
    CHECK(target.bundle.filaments.find_preset("My PLA", false)->config.option<ConfigOptionInts>("nozzle_temperature")->values.front() == 225);
}

TEST_CASE("A Bambu Studio bundle's presets are written as preset files by kind, and nothing else", "[ForcaPresetImport]")
{
    ScopedTemporaryDir dir;
    const fs::path     bundle = dir.path() / "Bambu Lab H2S 0.4 nozzle.bbscfg";
    {
        mz_zip_archive zip;
        mz_zip_zero_struct(&zip);
        REQUIRE(open_zip_writer(&zip, bundle.string()));
        const std::vector<std::pair<std::string, std::string>> entries = {
            {"bundle_structure.json", R"({"bundle_type":"printer config bundle"})"},
            {"printer/Bambu Lab H2S 0.4 nozzle.json", "{}"},
            {"process/0.20mm Mine.json", "{}"},
            {"filament/Brand PLA.json", "{}"},
            {"filament/readme.txt", "not a preset"},
            {"other/Elsewhere.json", "{}"},
            {"filament/../../Escape.json", "{}"}};
        for (const auto &[name, content] : entries)
            REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), content.data(), content.size(), MZ_DEFAULT_COMPRESSION));
        REQUIRE(mz_zip_writer_finalize_archive(&zip));
        REQUIRE(close_zip_writer(&zip));
    }
    CHECK(forca_is_bambu_bundle(bundle.string()));
    CHECK(forca_is_bambu_bundle("Brand.BBSFLMT"));
    CHECK_FALSE(forca_is_bambu_bundle("Brand.orca_filament"));

    const fs::path           out   = dir.path() / "out";
    std::vector<std::string> files = forca_extract_bambu_bundle(bundle.string(), out.string());
    std::sort(files.begin(), files.end());

    std::vector<std::string> expected = {(out / "filament" / "Brand PLA.json").string(), (out / "filament" / "Escape.json").string(),
                                         (out / "printer" / "Bambu Lab H2S 0.4 nozzle.json").string(),
                                         (out / "process" / "0.20mm Mine.json").string()};
    std::sort(expected.begin(), expected.end());
    CHECK(files == expected);
    // The entry that tries to climb out stays inside the folder it belongs to.
    CHECK_FALSE(fs::exists(dir.path() / "Escape.json"));
    CHECK(forca_extract_bambu_bundle((dir.path() / "missing.bbscfg").string(), out.string()).empty());
}

TEST_CASE("A user filament whose parent is missing is listed for repair and loads once repaired", "[ForcaPresetImport]")
{
    ImportTarget target;
    add_system_filament(target.bundle, "Generic TPU @Printer A", "Generic", "TPU", {"Printer A"});
    const fs::path user = target.tmp.path() / "user";
    write_filament(user / PRESET_FILAMENT_NAME, "Shop TPU", "Shop TPU Base", R"("filament_type":["TPU"],"nozzle_temperature":["228"])");

    PresetsConfigSubstitutions substitutions;
    target.bundle.filaments.load_presets(user.string(), PRESET_FILAMENT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);
    REQUIRE(target.bundle.filaments.find_preset("Shop TPU", false) == nullptr);
    REQUIRE(target.bundle.filaments.presets_missing_parent().size() == 1);
    const PresetCollection::PresetMissingParent missing = target.bundle.filaments.presets_missing_parent().front();
    CHECK(missing.name == "Shop TPU");
    CHECK(missing.inherits == "Shop TPU Base");

    // The repair is an import of the same file with the parent the user chose.
    const PresetImportItem item = forca_analyse_preset_file(target.bundle, missing.file);
    CHECK(item.best_parent == "Generic TPU @Printer A");
    PresetImportChoice choice;
    choice.parent = item.best_parent;
    REQUIRE(forca_import_preset(target.bundle, item, choice));
    const Preset *repaired = target.bundle.filaments.find_preset("Shop TPU", false);
    REQUIRE(repaired != nullptr);
    CHECK(repaired->config.opt_string("inherits") == "Generic TPU @Printer A");
    CHECK(repaired->config.option<ConfigOptionInts>("nozzle_temperature")->values.front() == 228);

    // At the next start it loads, and is no longer listed.
    PresetBundle next;
    add_system_filament(next, "Generic TPU @Printer A", "Generic", "TPU", {"Printer A"});
    next.filaments.load_presets(user.string(), PRESET_FILAMENT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);
    CHECK(next.filaments.find_preset("Shop TPU", false) != nullptr);
    CHECK(next.filaments.presets_missing_parent().empty());
}
