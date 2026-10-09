// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Application.h"
#include "core/ImageProvider.h"
#include "core/InputBindings.h"
#include "core/SortSpec.h"

#include <QApplication>

Q_DECLARE_METATYPE(lf::InputEvent)
Q_DECLARE_METATYPE(lf::SortSpec)

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Lumaflip"));
    QApplication::setOrganizationName(QStringLiteral("Lumaflip"));
    QApplication::setOrganizationDomain(QStringLiteral("brokoskokoli.github.io"));
    QApplication::setApplicationVersion(QStringLiteral(LF_APP_VERSION));

    qRegisterMetaType<lf::InputEvent>();
    qRegisterMetaType<lf::SortSpec>();
    qRegisterMetaType<lf::DecodedImagePtr>("lf::DecodedImagePtr");
    qRegisterMetaType<lf::Quality>("lf::Quality");

    lf::Application shell;
    shell.start(QApplication::arguments());
    return QApplication::exec();
}
