// SPDX-License-Identifier: GPL-3.0-or-later
#include "RenameController.h"

#include "Application.h"
#include "ImageView.h"
#include "ViewerWindow.h"
#include "core/Collection.h"
#include "core/FileOps.h"

#include <QEvent>
#include <QFileInfo>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>

#include <algorithm>

namespace lf {

namespace {

constexpr int kMaxNameBytes = 255; // APFS/HFS+ limit (UTF-8)
constexpr int kTopMargin = 12;

QString folderOf(const QString& path) {
    return QFileInfo(path).absolutePath();
}

QString withoutExtension(const QString& name) {
    const QString ext = RenameController::extensionOf(name);
    return ext.isEmpty() ? name : name.left(name.size() - ext.size() - 1);
}

} // namespace

RenameController::RenameController(ViewerWindow& window, Application& app)
    : QObject(&window), m_window(window), m_app(app) {
    setObjectName(QStringLiteral("lfRenameController"));
    // Another image became current (flipping, a file operation of another window): the
    // edit belongs to the old one.
    connect(window.collection(), &Collection::currentChanged, this, [this](int, const QString& path) {
        if (isEditing() && !m_pending && path != m_path)
            cancel();
    });
}

RenameController::~RenameController() {
    delete m_question.data();
    delete m_editor.data();
}

RenameController* RenameController::of(ViewerWindow* window, Application& app) {
    if (auto* c = window->findChild<RenameController*>(QStringLiteral("lfRenameController"),
                                                       Qt::FindDirectChildrenOnly))
        return c;
    return new RenameController(*window, app);
}

bool RenameController::isEditing() const {
    return m_editor != nullptr;
}

QString RenameController::extensionOf(const QString& fileName) {
    const qsizetype dot = fileName.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == fileName.size() - 1)
        return {};
    return fileName.mid(dot + 1);
}

int RenameController::baseNameLength(const QString& fileName) {
    const QString ext = extensionOf(fileName);
    return int(ext.isEmpty() ? fileName.size() : fileName.size() - ext.size() - 1);
}

QString RenameController::validationError(const QString& newName) {
    if (newName.trimmed().isEmpty())
        return tr("Bitte gib einen Namen ein.");
    if (newName.contains(QLatin1Char('/')) || newName.contains(QLatin1Char(':')))
        return tr("Der Name darf keinen Schrägstrich (/) und keinen Doppelpunkt (:) enthalten.");
    if (newName.startsWith(QLatin1Char('.')))
        return tr("Namen, die mit einem Punkt beginnen, sind für Systemdateien reserviert.");
    for (const QChar c : newName)
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            return tr("Der Name enthält ungültige Zeichen.");
    if (newName.toUtf8().size() > kMaxNameBytes)
        return tr("Der Name ist zu lang (höchstens %1 Byte).").arg(kMaxNameBytes);
    return {};
}

QString RenameController::conflictError(const QString& newName) const {
    // The folder's images are known (no file system access on the GUI thread); other
    // files with the name are reported by FileOps when renaming.
    const QString folder = folderOf(m_path);
    const QString wanted = newName.normalized(QString::NormalizationForm_C);
    for (const FileEntry& e : m_window.collection()->entries()) {
        if (e.path == m_path || folderOf(e.path) != folder)
            continue;
        // APFS/HFS+ volumes are case-insensitive by default.
        if (e.name.normalized(QString::NormalizationForm_C).compare(wanted, Qt::CaseInsensitive) == 0)
            return tr("„%1“ existiert bereits. Bitte wähle einen anderen Namen.").arg(newName);
    }
    return {};
}

bool RenameController::start() {
    const QString path = m_window.currentPath();
    if (path.isEmpty())
        return false;
    if (isEditing()) {
        if (m_path == path || m_pending) {
            if (m_editor && !m_question)
                m_editor->setFocus(Qt::OtherFocusReason);
            return true;
        }
        cancel();
    }
    m_path = path;
    const QString name = QFileInfo(path).fileName();
    auto* edit = new QLineEdit(m_window.view());
    edit->setObjectName(QStringLiteral("lfRenameEditor"));
    edit->setAccessibleName(tr("Neuer Dateiname"));
    edit->setAttribute(Qt::WA_MacShowFocusRect, false);
    edit->setStyleSheet(
        QStringLiteral("QLineEdit#lfRenameEditor { background: palette(base); color: palette(text); "
                       "border: 2px solid palette(highlight); border-radius: 6px; "
                       "padding: 3px 6px; }"));
    edit->setText(name);
    // F-FILE-04: the extension is protected - only the name is selected.
    edit->setSelection(0, baseNameLength(name));
    edit->installEventFilter(this);
    m_window.view()->installEventFilter(this);
    connect(edit, &QLineEdit::returnPressed, this, &RenameController::commit);
    connect(edit, &QLineEdit::textEdited, this, &RenameController::place);
    m_editor = edit;
    place();
    edit->show();
    edit->raise();
    edit->setFocus(Qt::OtherFocusReason);
    return true;
}

void RenameController::place() {
    if (!m_editor)
        return;
    QWidget* view = m_window.view();
    const int textWidth = m_editor->fontMetrics().horizontalAdvance(m_editor->text()) + 40;
    const int maxWidth = std::max(120, view->width() - 32);
    const int w = std::clamp(textWidth, std::min(240, maxWidth), maxWidth);
    const int h = m_editor->sizeHint().height();
    m_editor->setGeometry((view->width() - w) / 2, kTopMargin, w, h);
}

void RenameController::cancel() {
    if (m_pending)
        return; // the rename is running; its result closes the editor
    close(false);
}

void RenameController::close(bool renamed) {
    delete m_question.data();
    if (m_editor) {
        m_editor->removeEventFilter(this);
        m_editor->hide();
        m_editor->deleteLater();
    }
    m_editor = nullptr;
    m_window.view()->removeEventFilter(this);
    m_path.clear();
    m_pending = false;
    if (m_window.isActiveWindow())
        m_window.view()->setFocus(Qt::OtherFocusReason);
    emit finished(renamed);
}

void RenameController::commit() {
    if (!m_editor || m_pending || m_question)
        return;
    const QString oldName = QFileInfo(m_path).fileName();
    const QString newName = m_editor->text();
    if (newName == oldName) {
        close(false);
        return;
    }
    QString error = validationError(newName);
    if (error.isEmpty())
        error = conflictError(newName);
    if (!error.isEmpty()) {
        m_window.showMessage(error, 4000);
        m_editor->setFocus(Qt::OtherFocusReason);
        return;
    }
    const QString oldExt = extensionOf(oldName);
    const QString newExt = extensionOf(newName);
    if (oldExt.compare(newExt, Qt::CaseInsensitive) == 0) {
        rename(newName);
        return;
    }
    // F-FILE-04: the extension is protected - changing it needs a confirmation.
    QString text;
    if (newExt.isEmpty())
        text = tr("Soll die Endung „.%1“ entfernt werden?").arg(oldExt);
    else if (oldExt.isEmpty())
        text = tr("Soll die Endung „.%1“ hinzugefügt werden?").arg(newExt);
    else
        text = tr("Soll die Endung „.%1“ in „.%2“ geändert werden?").arg(oldExt, newExt);
    auto* box = new QMessageBox(QMessageBox::Question, tr("Dateiendung ändern?"), text, QMessageBox::NoButton,
                                &m_window);
    box->setObjectName(QStringLiteral("lfRenameExtension"));
    box->setInformativeText(
        tr("Die Datei wird dadurch nicht umgewandelt. Andere Programme erkennen sie danach "
           "eventuell nicht mehr."));
    QPushButton* keep =
        box->addButton(oldExt.isEmpty() ? tr("Ohne Endung lassen") : tr("„.%1“ behalten").arg(oldExt),
                       QMessageBox::AcceptRole);
    keep->setObjectName(QStringLiteral("lfKeepExtension"));
    QPushButton* use =
        box->addButton(newExt.isEmpty() ? tr("Endung entfernen") : tr("„.%1“ verwenden").arg(newExt),
                       QMessageBox::DestructiveRole);
    use->setObjectName(QStringLiteral("lfUseExtension"));
    QPushButton* back = box->addButton(tr("Abbrechen"), QMessageBox::RejectRole);
    box->setDefaultButton(keep);
    box->setEscapeButton(back);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::WindowModal);
    m_question = box;
    connect(box, &QMessageBox::buttonClicked, this, [this, keep, use, newName, oldExt](QAbstractButton* b) {
        m_question = nullptr;
        if (!m_editor)
            return;
        if (b == use) {
            rename(newName);
            return;
        }
        if (b == keep) {
            const QString base = withoutExtension(newName);
            const QString kept = oldExt.isEmpty() ? base : base + QLatin1Char('.') + oldExt;
            m_editor->setText(kept);
            QString error = validationError(kept);
            if (error.isEmpty())
                error = conflictError(kept);
            if (!error.isEmpty()) {
                m_window.showMessage(error, 4000);
                m_editor->setSelection(0, baseNameLength(kept));
                m_editor->setFocus(Qt::OtherFocusReason);
                return;
            }
            if (kept == QFileInfo(m_path).fileName())
                close(false);
            else
                rename(kept);
            return;
        }
        m_editor->setFocus(Qt::OtherFocusReason); // back to editing
    });
    box->open();
}

void RenameController::rename(const QString& newName) {
    m_pending = true;
    m_editor->setReadOnly(true);
    QPointer<RenameController> self(this);
    // Asynchronous and undoable (⌘Z). FileOps' fileRenamed updates all windows (Application).
    m_app.fileOps().renameAsync(m_path, newName, [self](bool ok, const QString& error) {
        if (!self)
            return;
        self->m_pending = false;
        if (ok) {
            self->close(true);
            return;
        }
        if (self->m_editor) {
            self->m_editor->setReadOnly(false);
            self->m_editor->setFocus(Qt::OtherFocusReason);
        }
        self->m_window.showMessage(error, 5000);
    });
}

bool RenameController::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_window.view()) {
        if (event->type() == QEvent::Resize)
            place();
        return false;
    }
    if (watched != m_editor)
        return false;
    switch (event->type()) {
    case QEvent::KeyPress: {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            cancel();
            return true;
        }
        break;
    }
    case QEvent::FocusOut: {
        // Clicking elsewhere in the window ends the edit like in Finder (keep the name if
        // unchanged); switching apps, menus and our own question keep it open.
        const auto reason = static_cast<QFocusEvent*>(event)->reason();
        if (m_pending || m_question || reason == Qt::ActiveWindowFocusReason ||
            reason == Qt::PopupFocusReason || reason == Qt::MenuBarFocusReason)
            break;
        if (m_editor && m_editor->text() == QFileInfo(m_path).fileName())
            QMetaObject::invokeMethod(this, &RenameController::cancel, Qt::QueuedConnection);
        else
            QMetaObject::invokeMethod(this, &RenameController::commit, Qt::QueuedConnection);
        break;
    }
    default:
        break;
    }
    return false;
}

} // namespace lf
