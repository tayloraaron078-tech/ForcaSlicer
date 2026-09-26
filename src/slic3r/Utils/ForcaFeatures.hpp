#pragma once

namespace Slic3r {

// Forca: Orca Cloud (auth./cloud.orcaslicer.com: login, preset sync, cloud bundles and plugins) is the OrcaSlicer
// team's own service and is switched off in Forca (public-alpha blocker B9). With this false:
//  - OrcaCloudServiceAgent makes no network calls, never restores a login, and never reads, writes or deletes the
//    "OrcaSlicer/Auth" secret that stock OrcaSlicer keeps in the OS credential store;
//  - the GUI hides Orca Cloud entry points (home-page login, Sync Presets, auto-sync, migration prompts);
//  - presets a user kept under an Orca Cloud account folder are merged into user/default once (GUI_App).
// Bambu Cloud (Bambu's own network plugin) is not affected. tools/forca/check_identity.ps1 checks this stays false.
constexpr bool FORCA_ORCA_CLOUD_ENABLED = false;

// Forca (B4): Forca's public GitHub repo (PLAN_public_release D3/D4). Bug reports, release pages and "about Forca"
// links go here, never to OrcaSlicer's repo or tracker. Bug reports use the issue form
// .github/ISSUE_TEMPLATE/bug_report.yml, whose "version" and "os" fields the link pre-fills.
constexpr const char* FORCA_REPO_URL = "https://github.com/tayloraaron078-tech/ForcaSlicer";

// Forca (B2): the app update check never asks OrcaSlicer's update server (it would offer OrcaSlicer releases, and
// Orca's check also sends an install id and OS details). Forca checks its own GitHub Releases instead, through the
// GitHub API URL of the public Forca repo ("https://api.github.com/repos/<owner>/<repo>/releases"); Orca's parser
// already reads that format, including pre-releases (every alpha is one; "/releases/latest" would skip them).
// Empty = no update check at all.
constexpr const char* FORCA_RELEASES_API_URL = "https://api.github.com/repos/tayloraaron078-tech/ForcaSlicer/releases";

// Forca (B2): Orca's online printer-profile updates (check-version.orcaslicer.com/profile) are Orca's server too.
// Off: Forca's printer profiles come from the profiles bundled with each Forca build (which pick up Orca's profile
// changes at every upstream merge). Installing those bundled profiles is not affected.
constexpr bool FORCA_ORCA_PROFILE_UPDATES_ENABLED = false;

} // namespace Slic3r
