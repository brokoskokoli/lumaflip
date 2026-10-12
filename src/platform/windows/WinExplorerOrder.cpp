// SPDX-License-Identifier: GPL-3.0-or-later
// The sort order of the Explorer window showing a folder (SPEC 6.1, F-SORT-04 on Windows).
//
// Explorer keeps the per-folder view settings in undocumented ShellBags; the documented way
// is to ask the open window itself: IShellWindows lists the open Explorer windows (tabs on
// Windows 11), each one's IShellBrowser gives the active IShellView, whose IFolderView2 knows
// the folder (IPersistFolder2), the sort columns and the "Group by" column. The common case
// is a double click in Explorer: that window is open and shows the folder. Without one,
// the caller falls back to name order (WinSorting.cpp) and says so.
//
// No permission is needed (unlike AppleScript on macOS), so the query is on by default.
// It runs on a thread of its own in a single-threaded apartment and is time-limited: a hung
// Explorer never holds up opening a folder; at most two queries wait for it at a time.

#include "platform/windows/WinExplorerOrder.h"

#include "platform/windows/WinCom.h"

#include <QDir>
#include <QUuid>

#include <windows.h>

#include <exdisp.h>
#include <servprov.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace lf::win {

using Microsoft::WRL::ComPtr;

namespace {

QString normalizedPath(const QString& path) {
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
}

explorer::PropertyKey toCore(const PROPERTYKEY& key) {
    return explorer::PropertyKey{QUuid(key.fmtid), quint32(key.pid)};
}

BOOL CALLBACK collectWindow(HWND hwnd, LPARAM param) {
    reinterpret_cast<std::vector<HWND>*>(param)->push_back(hwnd);
    return TRUE;
}

QString folderOf(IFolderView2* view) {
    ComPtr<IPersistFolder2> folder;
    if (FAILED(view->GetFolder(IID_PPV_ARGS(&folder))) || !folder)
        return {};
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(folder->GetCurFolder(&pidl)) || !pidl)
        return {};
    std::vector<wchar_t> buffer(32768);
    QString path;
    if (SHGetPathFromIDListEx(pidl, buffer.data(), DWORD(buffer.size()), GPFIDL_DEFAULT))
        path = QString::fromWCharArray(buffer.data());
    CoTaskMemFree(pidl);
    return path; // empty for virtual folders (This PC, libraries, search results)
}

std::optional<SortSpec> viewOrder(IFolderView2* view) {
    std::optional<explorer::ColumnSort> sort;
    int count = 0;
    if (SUCCEEDED(view->GetSortColumnCount(&count)) && count > 0) {
        std::vector<SORTCOLUMN> columns(static_cast<size_t>(count));
        if (SUCCEEDED(view->GetSortColumns(columns.data(), count)))
            sort = explorer::ColumnSort{toCore(columns.front().propkey),
                                        columns.front().direction != SORT_DESCENDING};
    }
    std::optional<explorer::ColumnSort> group;
    PROPERTYKEY groupKey{};
    BOOL groupAscending = TRUE;
    if (SUCCEEDED(view->GetGroupBy(&groupKey, &groupAscending)) &&
        !(IsEqualGUID(groupKey.fmtid, GUID_NULL) && groupKey.pid == 0))
        group = explorer::ColumnSort{toCore(groupKey), groupAscending != FALSE};
    return explorer::specForView(sort, group);
}

// Runs in an STA. `windowsSeen`: Explorer windows (and tabs) looked at, -1 without
// IShellWindows.
std::optional<SortSpec> query(const QString& wanted, int* windowsSeen) {
    *windowsSeen = -1;
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows))) || !windows)
        return std::nullopt;
    long count = 0;
    if (FAILED(windows->get_Count(&count)))
        return std::nullopt;
    *windowsSeen = 0;

    std::vector<HWND> zOrder; // top-level windows, topmost first
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&zOrder));

    struct Candidate {
        SortSpec spec;
        bool visibleTab = false;
        size_t z = SIZE_MAX;
    };
    std::optional<Candidate> best;
    for (long i = 0; i < count; ++i) {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> item;
        if (windows->Item(index, &item) != S_OK || !item)
            continue;
        ++*windowsSeen;
        ComPtr<IServiceProvider> services;
        ComPtr<IShellBrowser> browser;
        ComPtr<IShellView> shellView;
        ComPtr<IFolderView2> view;
        if (FAILED(item.As(&services)) ||
            FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) || !browser ||
            FAILED(browser->QueryActiveShellView(&shellView)) || !shellView || FAILED(shellView.As(&view)))
            continue;
        const QString folder = folderOf(view.Get());
        if (folder.isEmpty() || normalizedPath(folder) != wanted)
            continue;
        const std::optional<SortSpec> spec = viewOrder(view.Get());
        if (!spec)
            continue;
        Candidate c{*spec};
        HWND viewWindow = nullptr;
        c.visibleTab =
            SUCCEEDED(shellView->GetWindow(&viewWindow)) && viewWindow && IsWindowVisible(viewWindow);
        ComPtr<IWebBrowserApp> app;
        SHANDLE_PTR frame = 0;
        if (SUCCEEDED(item.As(&app)) && SUCCEEDED(app->get_HWND(&frame))) {
            const auto it = std::find(zOrder.cbegin(), zOrder.cend(), reinterpret_cast<HWND>(frame));
            if (it != zOrder.cend())
                c.z = size_t(it - zOrder.cbegin());
        }
        // The active tab first, then the window highest in the z-order (last active).
        if (!best || (c.visibleTab && !best->visibleTab) ||
            (c.visibleTab == best->visibleTab && c.z < best->z))
            best = c;
    }
    if (!best)
        return std::nullopt;
    return best->spec;
}

struct Pending {
    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    std::optional<SortSpec> result;
    int windowsSeen = -1;
};

std::atomic<int> g_running{0}; // queries whose thread has not finished (also abandoned ones)

} // namespace

std::optional<SortSpec> explorerOrder(const QString& folderPath, int timeoutMs, int* windowsSeen) {
    if (windowsSeen)
        *windowsSeen = -1;
    if (folderPath.isEmpty() || g_running.load() >= 2) // Explorer hangs: do not pile up threads
        return std::nullopt;
    auto pending = std::make_shared<Pending>();
    const QString wanted = normalizedPath(folderPath);
    g_running.fetch_add(1);
    try {
        std::thread([pending, wanted] {
            std::optional<SortSpec> result;
            int seen = -1;
            {
                ComScope com(COINIT_APARTMENTTHREADED);
                if (com.ok())
                    result = query(wanted, &seen);
            }
            {
                const std::lock_guard<std::mutex> lock(pending->mutex);
                pending->result = result;
                pending->windowsSeen = seen;
                pending->finished = true;
            }
            pending->done.notify_all();
            g_running.fetch_sub(1);
        }).detach();
    } catch (const std::system_error&) {
        g_running.fetch_sub(1);
        return std::nullopt;
    }
    std::unique_lock<std::mutex> lock(pending->mutex);
    if (!pending->done.wait_for(lock, std::chrono::milliseconds(std::max(1, timeoutMs)),
                                [&] { return pending->finished; }))
        return std::nullopt; // the thread finishes on its own
    if (windowsSeen)
        *windowsSeen = pending->windowsSeen;
    return pending->result;
}

std::optional<SortSpec> explorerOrder(const QString& folderPath, int timeoutMs) {
    return explorerOrder(folderPath, timeoutMs, nullptr);
}

} // namespace lf::win
