// SPDX-License-Identifier: GPL-3.0-or-later
// F-I18N-03: live language switch of a viewer window. Texts that are set once when the
// window is built get their translation again; the rest (title, status bar, start screen)
// is computed anew. The info panel retranslates itself (InfoPanel::changeEvent).

#include "Application.h"
#include "Filmstrip.h"
#include "HdrController.h"
#include "HistogramWidget.h"
#include "StartScreenRecent.h"
#include "ViewerWindow.h"
#include "WindowPanels.h"
#include "core/ImageProvider.h"

#include <QDockWidget>
#include <QLabel>
#include <QPushButton>

namespace lf {

void ViewerWindow::retranslateUi() {
    auto setButton = [this](const char* name, const QString& text) {
        if (auto* b = findChild<QPushButton*>(QLatin1String(name)))
            b->setText(text);
    };
    setButton("lfAllowAccess", tr("Zugriff erlauben …"));
    setButton("lfDismissAccess", tr("Ausblenden"));
    setButton("lfOpenButton", tr("Öffnen …"));
    if (m_accessHint && !m_accessHint->isHidden() && !m_accessFolder.isEmpty())
        showAccessHint(m_accessFolder, m_accessPrivacy);
    if (auto* dock = findChild<QDockWidget*>(QStringLiteral("lfInfoDock")))
        dock->setWindowTitle(WindowPanels::tr("Informationen"));
    if (auto* dock = findChild<QDockWidget*>(QStringLiteral("lfFilmDock")))
        dock->setWindowTitle(WindowPanels::tr("Filmstreifen"));
    if (auto* film = findChild<Filmstrip*>())
        film->setAccessibleName(Filmstrip::tr("Filmstreifen"));
    for (auto* histogram : findChildren<HistogramWidget*>())
        histogram->setAccessibleName(HistogramWidget::tr("Histogramm"));
    if (auto* title = findChild<QLabel*>(QStringLiteral("lfStartRecentTitle")))
        title->setText(StartScreenRecent::tr("Zuletzt geöffnet"));
    if (m_hdr)
        m_hdr->retranslate();
    // An error card holds the decoder's text in the old language: decode the file again
    // (a new failure shows the card in the current language, onImageFailed()).
    m_app.provider().forgetFailures();
    if (m_shownFailed && !currentPath().isEmpty() && currentPath() == m_shownPath)
        m_app.provider().request(m_shownPath, Quality::Screen, decodeTarget(), this, 0, m_page);
    updateTitle();
    updateEmptyState();
    updateStatus();
}

} // namespace lf
