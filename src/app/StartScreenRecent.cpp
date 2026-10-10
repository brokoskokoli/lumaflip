// SPDX-License-Identifier: GPL-3.0-or-later
#include "StartScreenRecent.h"

#include "RecentItems.h"

#include <QApplication>
#include <QLabel>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

namespace lf {

StartScreenRecent::StartScreenRecent(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("lfStartRecent"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    m_title = new QLabel(tr("Zuletzt geöffnet"), this);
    m_title->setObjectName(QStringLiteral("lfStartRecentTitle"));
    m_title->setAlignment(Qt::AlignHCenter);
    layout->addWidget(m_title);
    m_list = new QVBoxLayout;
    m_list->setSpacing(0);
    layout->addLayout(m_list);
    hide();
}

void StartScreenRecent::refresh() {
    while (QLayoutItem* item = m_list->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const QList<RecentItem> all = RecentItems::items();
    const QList<RecentItem> shown = all.mid(0, kMaxShown);
    QStyle* style = QApplication::style();
    const QIcon fileIcon = shown.isEmpty() ? QIcon() : style->standardIcon(QStyle::SP_FileIcon);
    const QIcon folderIcon = shown.isEmpty() ? QIcon() : style->standardIcon(QStyle::SP_DirIcon);
    const QString color = m_dark ? QStringLiteral("#e6e6e6") : QStringLiteral("#1c1c1c");
    const QString hover =
        m_dark ? QStringLiteral("rgba(255, 255, 255, 40)") : QStringLiteral("rgba(0, 0, 0, 25)");
    for (const RecentItem& item : shown) {
        auto* b = new QToolButton(this);
        b->setObjectName(QStringLiteral("lfStartRecentItem"));
        b->setProperty("path", item.path);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setIcon(item.folder ? folderIcon : fileIcon);
        b->setText(RecentItems::menuTitle(item, all));
        b->setToolTip(item.path);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QStringLiteral("QToolButton { color: %1; border: none; border-radius: 4px; "
                                        "padding: 3px 8px; font-size: 13px; }"
                                        "QToolButton:hover { background-color: %2; }")
                             .arg(color, hover));
        const QString p = item.path;
        connect(b, &QToolButton::clicked, this, [this, p] { emit openRequested(p); });
        m_list->addWidget(b, 0, Qt::AlignHCenter);
    }
    m_title->setStyleSheet(m_dark ? QStringLiteral("color: rgb(170, 170, 170); font-size: 12px;")
                                  : QStringLiteral("color: rgb(90, 90, 90); font-size: 12px;"));
    setVisible(!shown.isEmpty());
}

void StartScreenRecent::setDark(bool dark) {
    if (dark == m_dark)
        return;
    m_dark = dark;
    if (!isHidden())
        refresh();
}

QStringList StartScreenRecent::shownPaths() const {
    QStringList out;
    for (int i = 0; i < m_list->count(); ++i)
        if (QWidget* w = m_list->itemAt(i)->widget())
            out << w->property("path").toString();
    return out;
}

} // namespace lf
