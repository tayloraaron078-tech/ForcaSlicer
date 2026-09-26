// Forca AI rule R1 in the GUI: today's date, the data-folder ledger and the default save folder around the tested
// libslic3r implementation (libslic3r/ForcaAISafePath). See ForcaAIFiles.hpp.
#include "ForcaAIFiles.hpp"

#include "GUI.hpp"                // from_u8 / into_path (UTF-8 <-> native paths)
#include "libslic3r/ForcaAISafePath.hpp"
#include "libslic3r/Utils.hpp"   // data_dir()

#include <boost/filesystem/operations.hpp>

#include <wx/stdpaths.h>

#include <ctime>

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;

namespace {

std::string today()
{
    std::time_t t = std::time(nullptr);
    std::tm     tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

Slic3r::ForcaAI::Ledger ledger() { return Slic3r::ForcaAI::Ledger(into_path(from_u8(data_dir())) / "forca" / "ai" / "ledger.json"); }

} // namespace

std::string forca_ai_suffix() { return Slic3r::ForcaAI::suffix(today()); }
std::string forca_ai_strip_suffix(const std::string& name) { return Slic3r::ForcaAI::strip_suffix(name); }
std::string forca_ai_clean_name(const std::string& name) { return Slic3r::ForcaAI::clean_name(name); }

fs::path forca_ai_new_path(const fs::path& dir, const std::string& stem, const std::string& ext)
{
    return Slic3r::ForcaAI::new_path(dir, stem, ext, today());
}

bool forca_ai_may_write(const fs::path& path) { return ledger().may_write(path); }
bool forca_ai_created(const fs::path& path) { return ledger().created(path); }
void forca_ai_record_write(const fs::path& path) { ledger().record_write(path, today()); }

fs::path forca_ai_default_dir()
{
    fs::path dir = into_path(wxStandardPaths::Get().GetDocumentsDir()) / "Forca AI";
    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

}} // namespace Slic3r::GUI
