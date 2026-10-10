// SPDX-License-Identifier: GPL-3.0-or-later
#include "Share.h"

#include <QAction>
#include <QCoreApplication>
#include <QIcon>
#include <QMenu>
#include <QPixmap>

namespace lf {

namespace {
QString tr(const char* text) {
    return QCoreApplication::translate("lf::Share", text);
}
} // namespace

QList<QUrl> shareItems(const QStringList& paths) {
    QList<QUrl> out;
    for (const QString& p : paths)
        if (!p.isEmpty())
            out << QUrl::fromLocalFile(p);
    return out;
}

void populateShareMenu(QMenu* menu, const QStringList& paths, const std::function<void()>& showPicker) {
    menu->clear();
    QAction* picker = menu->addAction(tr("Teilen …"));
    picker->setObjectName(QStringLiteral("share:picker"));
    picker->setEnabled(!paths.isEmpty());
    QObject::connect(picker, &QAction::triggered, menu, [showPicker] {
        if (showPicker)
            showPicker();
    });
    if (paths.isEmpty())
        return;
    const QList<ShareService> services = shareServices(paths);
    if (services.isEmpty())
        return;
    menu->addSeparator();
    for (const ShareService& s : services) {
        QAction* act =
            menu->addAction(s.icon.isNull() ? QIcon() : QIcon(QPixmap::fromImage(s.icon)), s.title);
        act->setObjectName(QStringLiteral("share:") + s.title);
        const QString title = s.title;
        QObject::connect(act, &QAction::triggered, menu,
                         [title, paths] { performShareService(title, paths); });
    }
}

} // namespace lf
