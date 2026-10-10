// SPDX-License-Identifier: GPL-3.0-or-later
// F-OPEN-11 on Windows: the app's jump list (taskbar / Start menu "Recent") via the shell's
// recent documents for this application.
#include "RecentItems.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <windows.h>

#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace lf {

bool runsAsAppBundle() {
    // The installed/deployed app (not a test binary): Lumaflip.exe.
    return QFileInfo(QCoreApplication::applicationFilePath())
               .fileName()
               .compare(QStringLiteral("Lumaflip.exe"), Qt::CaseInsensitive) == 0;
}

void noteSystemRecentDocument(const QString& path) {
    const QString native = QDir::toNativeSeparators(path);
    SHAddToRecentDocs(SHARD_PATHW, reinterpret_cast<const wchar_t*>(native.utf16()));
}

void clearSystemRecentDocuments() {
    // Only this app's jump list entries (SHAddToRecentDocs(0, nullptr) would clear the
    // user's whole recent documents list).
    Microsoft::WRL::ComPtr<IApplicationDestinations> dest;
    if (SUCCEEDED(CoCreateInstance(CLSID_ApplicationDestinations, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dest))))
        dest->RemoveAllDestinations();
}

} // namespace lf
