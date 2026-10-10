#include "ForcaObnChoice.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/ForcaOBN.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/busyinfo.h>
#include <wx/utils.h>

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

namespace {

// OBN publishes builds for the current plug-in series only (lib/v02.08.01 in 2.2.0).
std::string obn_series() { return get_latest_network_version(); }

AppConfig* cfg() { return wxGetApp().app_config; }

void set_tags(const std::string& tag, const std::string& previous)
{
    cfg()->set("forca_obn_tag", tag);
    cfg()->set("forca_obn_previous_tag", previous);
}

// Select `version` for the next start. No hot reload: the camera player keeps the camera DLL's function addresses
// for the whole session (StaticBambuLib), so unloading that DLL while Forca runs crashes the next live view. The DLLs
// are swapped on disk (in-use ones renamed aside) and load fresh when Forca starts again.
void select_for_restart(const std::string& version)
{
    cfg()->set_network_plugin_version(version);
    cfg()->save();
}

wxString obn_warning()
{
    return _L("Open Bamboo Networking (OBN) is an open-source replacement for Bambu Lab's network plug-in. With it:\n\n"
              "- Your Bambu Lab printers must be in LAN-only mode with Developer Mode on.\n"
              "- Cloud printing and the camera away from home, Go Live, HMS photos and MakerWorld history don't work.\n"
              "- Forca talks only to your printers, and in our tests the crash after sending a print was gone.\n\n"
              "Forca downloads OBN from its GitHub releases and checks the file. You can switch back to Bambu Lab's "
              "plug-in here at any time.");
}

wxString restart_note() { return "\n\n" + _L("Close and reopen Forca to finish switching."); }

// Download a release unless it is already on disk, then put it in place and select it.
bool install_and_select(wxWindow* parent, const ForcaObnRelease* release, const std::string& tag)
{
    std::string err;
    {
        wxBusyCursor busy;
        wxBusyInfo   info(_L("Getting Open Bamboo Networking..."), parent);
        const auto   cached = forca_obn_cached_tags();
        if (release && std::find(cached.begin(), cached.end(), tag) == cached.end() &&
            !forca_obn_download(*release, obn_series(), err)) {
            MessageDialog(parent, _L("Could not download Open Bamboo Networking:") + "\n" + from_u8(err), _L("Network plug-in"),
                          wxOK | wxICON_ERROR).ShowModal();
            return false;
        }
        if (!forca_obn_activate(tag, obn_series(), err)) {
            MessageDialog(parent, _L("Could not install Open Bamboo Networking:") + "\n" + from_u8(err), _L("Network plug-in"),
                          wxOK | wxICON_ERROR).ShowModal();
            return false;
        }
    }
    select_for_restart(forca_obn_version(obn_series()));
    return true;
}

} // namespace

bool forca_obn_active() { return forca_obn_is_obn_version(cfg()->get_network_plugin_version()); }
std::string forca_obn_tag() { return cfg()->get("forca_obn_tag"); }
std::string forca_obn_previous_tag() { return cfg()->get("forca_obn_previous_tag"); }

bool forca_obn_switch(wxWindow* parent, bool to_obn)
{
    if (to_obn == forca_obn_active())
        return true;
    if (!to_obn) {
        std::string err;
        if (!forca_obn_restore_bambu_camera(err))
            MessageDialog(parent, from_u8(err), _L("Network plug-in"), wxOK | wxICON_WARNING).ShowModal();
        select_for_restart(obn_series());
        MessageDialog(parent, _L("Switched to Bambu Lab's network plug-in.") + restart_note(), _L("Network plug-in"),
                      wxOK | wxICON_INFORMATION).ShowModal();
        return true;
    }

    if (MessageDialog(parent, obn_warning(), _L("Switch to Open Bamboo Networking?"), wxYES_NO | wxICON_WARNING).ShowModal() != wxID_YES)
        return false;

    // Reuse the build used before; otherwise get the latest release.
    std::string tag = forca_obn_tag();
    const auto  cached = forca_obn_cached_tags();
    ForcaObnRelease release;
    if (tag.empty() || std::find(cached.begin(), cached.end(), tag) == cached.end()) {
        std::string err;
        wxBusyCursor busy;
        if (!forca_obn_fetch_latest(release, err)) {
            MessageDialog(parent, _L("Could not find the latest Open Bamboo Networking release:") + "\n" + from_u8(err),
                          _L("Network plug-in"), wxOK | wxICON_ERROR).ShowModal();
            return false;
        }
        tag = release.tag;
    }
    const bool ok = install_and_select(parent, release.tag.empty() ? nullptr : &release, tag);
    if (ok) {
        set_tags(tag, forca_obn_previous_tag());
        cfg()->save();
        MessageDialog(parent, wxString::Format(_L("Switched to Open Bamboo Networking %s."), from_u8(tag)) + restart_note(), _L("Network plug-in"),
                      wxOK | wxICON_INFORMATION).ShowModal();
    }
    return ok;
}

bool forca_obn_update(wxWindow* parent)
{
    ForcaObnRelease release;
    std::string     err;
    {
        wxBusyCursor busy;
        if (!forca_obn_fetch_latest(release, err)) {
            MessageDialog(parent, _L("Could not find the latest Open Bamboo Networking release:") + "\n" + from_u8(err),
                          _L("Network plug-in"), wxOK | wxICON_ERROR).ShowModal();
            return false;
        }
    }
    const std::string current = forca_obn_tag();
    if (release.tag == current) {
        MessageDialog(parent, wxString::Format(_L("Open Bamboo Networking %s is the latest release."), from_u8(current)),
                      _L("Network plug-in"), wxOK | wxICON_INFORMATION).ShowModal();
        return true;
    }
    if (MessageDialog(parent,
                      wxString::Format(_L("Update Open Bamboo Networking from %s to %s?\nForca keeps %s, so you can go back to it."),
                                       from_u8(current), from_u8(release.tag), from_u8(current)),
                      _L("Network plug-in"), wxYES_NO | wxICON_QUESTION).ShowModal() != wxID_YES)
        return false;
    if (!install_and_select(parent, &release, release.tag))
        return false;
    set_tags(release.tag, current);
    cfg()->save();
    MessageDialog(parent, wxString::Format(_L("Switched to Open Bamboo Networking %s."), from_u8(release.tag)) + restart_note(), _L("Network plug-in"),
                  wxOK | wxICON_INFORMATION).ShowModal();
    return true;
}

bool forca_obn_rollback(wxWindow* parent)
{
    const std::string current  = forca_obn_tag();
    const std::string previous = forca_obn_previous_tag();
    const auto        cached   = forca_obn_cached_tags();
    if (previous.empty() || std::find(cached.begin(), cached.end(), previous) == cached.end()) {
        MessageDialog(parent, _L("There is no earlier Open Bamboo Networking release to go back to."), _L("Network plug-in"),
                      wxOK | wxICON_INFORMATION).ShowModal();
        return false;
    }
    if (MessageDialog(parent, wxString::Format(_L("Go back from Open Bamboo Networking %s to %s?"), from_u8(current), from_u8(previous)),
                      _L("Network plug-in"), wxYES_NO | wxICON_QUESTION).ShowModal() != wxID_YES)
        return false;
    if (!install_and_select(parent, nullptr, previous))
        return false;
    set_tags(previous, current);
    cfg()->save();
    MessageDialog(parent, wxString::Format(_L("Switched to Open Bamboo Networking %s."), from_u8(previous)) + restart_note(), _L("Network plug-in"),
                  wxOK | wxICON_INFORMATION).ShowModal();
    return true;
}

void forca_obn_note_last_crash()
{
    // The newest crash log; offer OBN once for it if the crash was inside Bambu's network plug-in.
    boost::system::error_code ec;
    const fs::path            log_dir = fs::path(data_dir()) / "log";
    fs::path                  newest;
    std::time_t               newest_time = 0;
    for (fs::directory_iterator it(log_dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind("crash_", 0) != 0 || it->path().extension() != ".log")
            continue;
        const std::time_t t = fs::last_write_time(it->path(), ec);
        if (!ec && t > newest_time) {
            newest_time = t;
            newest      = it->path();
        }
    }
    if (newest.empty() || cfg()->get("forca_obn_crash_seen") == newest.filename().string())
        return;
    cfg()->set("forca_obn_crash_seen", newest.filename().string());
    boost::nowide::ifstream in(newest.string(), std::ios::binary);
    const std::string       text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (forca_obn_crash_in_network_plugin(text)) {
        BOOST_LOG_TRIVIAL(info) << "forca_obn: the last crash (" << newest.filename().string() << ") was in Bambu's network plug-in";
        cfg()->set_bool("forca_obn_offer_pending", true);
    }
}

void forca_obn_check_loaded(wxWindow* parent)
{
    // OBN was selected but didn't load at this start: back to Bambu's plug-in, so the printers keep working.
    if (!forca_obn_active() || !cfg()->get_bool("installed_networking") ||
        network_plugin_series(NetworkAgent::get_version()) == obn_series())
        return;
    BOOST_LOG_TRIVIAL(warning) << "forca_obn: OBN was selected but did not load (" << NetworkAgent::get_version() << ")";
    std::string err;
    forca_obn_restore_bambu_camera(err);
    select_for_restart(obn_series());
    MessageDialog(parent, _L("Open Bamboo Networking did not load, so Forca switched back to Bambu Lab's network plug-in.") +
                              restart_note(),
                  _L("Network plug-in"), wxOK | wxICON_WARNING).ShowModal();
}

void forca_obn_offer_after_crash(wxWindow* parent)
{
    if (!cfg()->get_bool("forca_obn_offer_pending") || cfg()->get_bool("forca_obn_never_offer") || forca_obn_active())
        return;
    cfg()->set_bool("forca_obn_offer_pending", false);
    RichMessageDialog dlg(parent,
                          _L("Forca closed unexpectedly last time, inside Bambu Lab's network plug-in. Open Bamboo Networking, "
                             "an open-source replacement, avoided this crash in our tests. Try it now?"),
                          _L("Network plug-in"), wxYES_NO | wxICON_QUESTION);
    dlg.ShowCheckBox(_L("Don't show again"));
    const int res = dlg.ShowModal();
    if (dlg.IsCheckBoxChecked())
        cfg()->set_bool("forca_obn_never_offer", true);
    cfg()->save();
    if (res == wxID_YES)
        forca_obn_switch(parent, true);
}

}} // namespace Slic3r::GUI
