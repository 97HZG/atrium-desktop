// Folder stack entries: path normalization, deduplication, background
// display-name/icon loading and Explorer window reuse or launch.
#pragma once

#include "icon_image.h"

#include <windows.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace atrium {

struct StackFolder {
    std::wstring path;
    std::wstring name;
    IconImage icon;
    bool missing = false;
};

// Trims leading/trailing ASCII whitespace and non-root trailing slashes.
std::wstring NormalizeFolderPath(std::wstring path);

// Normalizes, drops empty paths, deduplicates case-insensitively keeping the
// first occurrence, and truncates to maxCount.
std::vector<std::wstring> DedupeFolders(std::vector<std::wstring> paths, size_t maxCount);

class FolderStackModel {
public:
    FolderStackModel(HWND notify, UINT msg);
    ~FolderStackModel();
    FolderStackModel(const FolderStackModel&) = delete;
    FolderStackModel& operator=(const FolderStackModel&) = delete;

    void SetFolders(std::vector<std::wstring> paths, int iconPx);
    std::vector<StackFolder> Snapshot() const;
    void OpenFolder(const std::wstring& path, bool forceNewWindow = false);

private:
    struct OpenJob {
        std::wstring path;
        bool forceNewWindow = false;
    };

    void Worker();
    void ResolveBatch(const std::vector<std::wstring>& paths, int iconPx, uint64_t gen);

    HWND notify_;
    UINT msg_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::wstring> pendingPaths_;
    int pendingIconPx_ = 32;
    uint64_t pendingGen_ = 0;
    uint64_t resolvedGen_ = 0;
    bool stop_ = false;
    std::deque<OpenJob> openJobs_;
    std::vector<StackFolder> snapshot_;
    std::thread thread_;
};

} // namespace atrium
