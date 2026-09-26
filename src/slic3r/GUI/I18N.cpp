#include "I18N.hpp"

#include <wx/wxcrt.h>

namespace Slic3r { namespace GUI {

wxString L_str(const std::string &str)
{
	//! Explicitly specify that the source string is already in UTF-8 encoding
	return I18N::forca_brand(wxString(str.c_str(), wxConvUTF8), wxGetTranslation(wxString(str.c_str(), wxConvUTF8)));
}

namespace I18N {

// Forca: Orca's source strings name the app "OrcaSlicer" / "Orca Slicer" / "Orca". Instead of rewriting ~110 msgids
// (which would drop their translations in every language and conflict with every upstream merge), the product name is
// swapped here, after translation, so it reads "Forca Slicer" in all languages. Left alone on purpose:
//  - Orca's services and named things: Orca Cloud / OrcaCloud, the Orca calibration models (Cube, Badge, Tolerance
//    Test, String Hell, YOLO), Orca Arena, Orca Preset Bundle, "Orca Button" widgets, orcaslicer:// and .orca_* names;
//  - any string whose source already says "Forca" (Forca's own text that credits OrcaSlicer on purpose);
//  - the About-box history of OrcaSlicer itself (KEEP_ORCA below).
wxString forca_brand(const wxString &source, const wxString &translated)
{
    if (!translated.Contains("Orca") || source.Contains("Forca"))
        return translated;
    static const wxString KEEP_ORCA[] = {
        "OrcaSlicer began in that same spirit",
        "Today, OrcaSlicer is the most widely used",
    };
    for (const wxString &keep : KEEP_ORCA)
        if (source.StartsWith(keep))
            return translated;

    static const wxString KEEP_AFTER[] = {"Cloud", " Cloud", " Cube", " Badge", " Tolerance", " String", " YOLO",
                                          " Arena", " Preset Bundle", " Button"};
    wxString out;
    out.reserve(translated.length() + 16);
    const size_t n = translated.length();
    for (size_t i = 0; i < n;) {
        if (translated.compare(i, 4, "Orca") != 0) {
            out += translated[i++];
            continue;
        }
        const bool prev_ok = i == 0 || (!wxIsalnum(translated[i - 1]) && translated[i - 1] != '/' && translated[i - 1] != '.' &&
                                        translated[i - 1] != '_');
        size_t len = 4;                                   // "Orca"
        if (translated.compare(i, 10, "OrcaSlicer") == 0) len = 10;
        else if (translated.compare(i, 11, "Orca Slicer") == 0) len = 11;
        const bool next_ok = i + len >= n || (!wxIsalnum(translated[i + len]) && translated[i + len] != '/' && translated[i + len] != '_');
        bool keep = !prev_ok || !next_ok;
        if (!keep && len == 4)
            for (const wxString &after : KEEP_AFTER)
                if (translated.compare(i + 4, after.length(), after) == 0) { keep = true; break; }
        out += keep ? translated.Mid(i, len) : wxString(len == 4 ? "Forca" : "Forca Slicer");
        i += len;
    }
    return out;
}

} // namespace I18N

} }
