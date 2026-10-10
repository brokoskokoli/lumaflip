// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/windows/WinCom.h"

#include <thread>

namespace lf::win {

void runInSta(const std::function<void()>& fn) {
    std::thread t([&fn] {
        ComScope com(COINIT_APARTMENTTHREADED);
        fn();
    });
    t.join();
}

QString errorText(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    QString text = n && buffer ? QString::fromWCharArray(buffer, int(n)).trimmed() : QString();
    if (buffer)
        LocalFree(buffer);
    if (text.isEmpty())
        text = QStringLiteral("0x%1").arg(quint32(code), 8, 16, QLatin1Char('0'));
    return text;
}

} // namespace lf::win
