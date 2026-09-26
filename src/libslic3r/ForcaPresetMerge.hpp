#pragma once

#include <boost/filesystem/path.hpp>

#include <string>

namespace Slic3r {

// Forca: Orca Cloud is switched off in Forca (slic3r/Utils/ForcaFeatures.hpp), so Forca always loads the local
// "default" user-preset folder. Presets a user kept under an Orca Cloud account folder (user/<account id>) would
// silently disappear from view, so they are merged into user/default once:
//  - the whole user folder is first copied to backup_dir (skipped if backup_dir already exists);
//  - every file of every account folder is copied into default_name, keeping its relative path; on a name clash the
//    newer file wins (an older or equally old account file never replaces the default one);
//  - .info files (Orca's cloud sync markers) are skipped, as Orca's own login migration does;
//  - the account folders themselves are never changed.
// Returns the number of files copied. Throws boost::filesystem_error on I/O failure.
size_t forca_merge_account_presets(const boost::filesystem::path& user_dir,
                                   const boost::filesystem::path& backup_dir,
                                   const std::string&             default_name = "default");

} // namespace Slic3r
