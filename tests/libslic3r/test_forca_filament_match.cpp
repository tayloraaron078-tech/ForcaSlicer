#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

// Filament presets as a Kobra X user would have them: Anycubic system presets for the printer, an Elegoo
// one, and a user preset saved from Generic PETG (so it carries Generic PETG's filament_id).
struct FilamentCollection : public PresetCollection
{
    FilamentCollection()
        : PresetCollection(Preset::TYPE_FILAMENT, Preset::filament_options(),
                           static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()))
    {
        add("Anycubic PLA @Anycubic Kobra X 0.4 nozzle", "Anycubic", "ACPLA", true);
        add("Anycubic PLA High Speed @Anycubic Kobra X 0.4 nozzle", "Anycubic", "ACPLAHS", true);
        add("Elegoo Rapid PETG @Anycubic Kobra X 0.4 nozzle", "Elegoo", "ELRPETG", true);
        add("Elegoo PETG @Anycubic Kobra X 0.4 nozzle", "Elegoo", "ELPETG", true);
        add("Sunlu PETG Test", "Generic", "GFG99", false);
    }

    void add(const std::string &name, const std::string &vendor, const std::string &filament_id, bool system)
    {
        DynamicPrintConfig config(default_preset().config);
        config.option<ConfigOptionStrings>("filament_vendor", true)->values = {vendor};
        Preset &preset     = load_preset(std::string(), name, config, /*select=*/false);
        preset.is_system   = system;
        preset.filament_id = filament_id;
    }
};

} // namespace

TEST_CASE("A filament changer slot's name and vendor select the matching preset", "[Preset]")
{
    const auto [name, vendor, type, expected] = GENERATE(table<std::string, std::string, std::string, std::string>({
        // The longest matching name wins over a shorter one that also prefixes the slot name.
        {"Anycubic PLA High Speed OSHG", "Anycubic", "PLA", "ACPLAHS"},
        // A user preset is returned by name: its filament_id is Generic PETG's and would select that.
        {"Sunlu PETG Test", "Sunlu", "PETG", "Sunlu PETG Test"},
        // Vendor + name, ignoring case, symbols and a trailing colour word.
        {"rapid petg (grey)", "Elegoo", "PETG", "ELRPETG"},
        // Vendor + type when the name is unknown.
        {"Mystery", "Elegoo", "PETG", "ELPETG"},
        // A system preset of another vendor is never chosen, even if its name matches.
        {"Anycubic PLA", "Sunlu", "", ""},
        {"PETG-CF", "", "PETG", ""},
    }));
    FilamentCollection filaments;
    INFO("slot '" << name << "' vendor '" << vendor << "' type '" << type << "'");
    CHECK(filaments.filament_id_by_filament_name(name, vendor, type) == expected);
}
