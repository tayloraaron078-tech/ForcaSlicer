#include "ForcaPresetMerge.hpp"

#include <boost/filesystem/operations.hpp>

#include <vector>

namespace Slic3r {

size_t forca_merge_account_presets(const boost::filesystem::path& user_dir,
                                   const boost::filesystem::path& backup_dir,
                                   const std::string&             default_name)
{
    namespace fs = boost::filesystem;
    if (!fs::is_directory(user_dir))
        return 0;

    std::vector<fs::path> accounts;
    for (const fs::directory_entry& entry : fs::directory_iterator(user_dir))
        if (fs::is_directory(entry.path()) && entry.path().filename() != default_name)
            accounts.push_back(entry.path());
    if (accounts.empty())
        return 0;

    if (!fs::exists(backup_dir))
        fs::copy(user_dir, backup_dir, fs::copy_options::recursive);

    const fs::path default_dir = user_dir / default_name;
    size_t         copied      = 0;
    for (const fs::path& account : accounts)
        for (fs::recursive_directory_iterator it(account), end; it != end; ++it) {
            if (!fs::is_regular_file(it->path()) || it->path().extension() == ".info")
                continue;
            const fs::path dst = default_dir / fs::relative(it->path(), account);
            if (fs::exists(dst) && fs::last_write_time(dst) >= fs::last_write_time(it->path()))
                continue;
            fs::create_directories(dst.parent_path());
            fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing);
            ++copied;
        }
    return copied;
}

} // namespace Slic3r
