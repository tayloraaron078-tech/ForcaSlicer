#include "ForcaFeedback.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"

#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/ForcaFeatures.hpp"

#include <wx/event.h>
#include <wx/utils.h>

#include <string>

namespace Slic3r { namespace GUI {

void forca_feedback_note_slice()
{
    AppConfig* config = wxGetApp().app_config;
    Plater*    plater = wxGetApp().plater();
    if (config == nullptr || plater == nullptr || config->get_bool("forca_feedback_asked"))
        return;
    int slices = 0;
    try {
        slices = std::stoi(config->get("forca_feedback_slices"));
    } catch (...) {
        // Not set yet, or not a number: start counting again.
    }
    ++slices;
    config->set("forca_feedback_slices", std::to_string(slices));
    if (slices < FORCA_FEEDBACK_AFTER_SLICES)
        return;
    config->set_bool("forca_feedback_asked", true);
    plater->get_notification_manager()->push_notification(
        NotificationType::CustomNotification, NotificationManager::NotificationLevel::ImportantNotificationLevel,
        // TRN Shown once, after the user's first few slices. "Forca" is the app's name.
        into_u8(_L("Forca is free and made by one person. How is it working for you? A few words help: what works, "
                   "what doesn't and what you miss.")),
        into_u8(_L("Share feedback")), [](wxEvtHandler*) {
            wxLaunchDefaultBrowser(std::string(FORCA_REPO_URL) + "/discussions");
            return true;
        });
}

}} // namespace Slic3r::GUI
