// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// COM helpers for the Windows platform layer. Most of it runs on worker threads
// (QThreadPool), which have no COM apartment: ComScope enters one for the scope (and
// leaves it again only if it entered it itself; a thread that is already in another
// apartment - Qt's GUI thread is STA - keeps it).

#include <QString>

#include <windows.h>

#include <objbase.h>

#include <functional>

namespace lf::win {

class ComScope {
public:
    explicit ComScope(DWORD model = COINIT_MULTITHREADED) {
        const HRESULT hr = CoInitializeEx(nullptr, model | COINIT_DISABLE_OLE1DDE);
        m_uninit = SUCCEEDED(hr); // S_OK or S_FALSE: balanced by CoUninitialize
        m_ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }
    ~ComScope() {
        if (m_uninit)
            CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    bool ok() const { return m_ok; }

private:
    bool m_uninit = false;
    bool m_ok = false;
};

// Runs `fn` on a new thread in a single-threaded apartment and waits for it (shell
// operations such as IFileOperation require an STA; the calling worker thread has none).
void runInSta(const std::function<void()>& fn);

// Text of a Win32/HRESULT error code in the user's language (FormatMessageW).
QString errorText(DWORD code);

// UTF-16 pointer of a QString for Win32 calls.
inline const wchar_t* wstr(const QString& s) {
    return reinterpret_cast<const wchar_t*>(s.utf16());
}

} // namespace lf::win
