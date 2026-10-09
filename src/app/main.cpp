// SPDX-License-Identifier: GPL-3.0-or-later
#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Lumaflip");
    QApplication::setOrganizationDomain("brokoskokoli.github.io");
    // Wiring (Application, windows, open events) is added by the app shell module.
    return 0;
}
