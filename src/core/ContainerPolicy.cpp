// SPDX-License-Identifier: GPL-3.0-or-later
#include "ContainerPolicy.h"

#include <QCoreApplication>

namespace lf::containers {

namespace {
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::ContainerPolicy)
};
} // namespace

bool isBlocked(Action action) {
    switch (action) {
    case Action::Trash:
    case Action::Rename:
    case Action::QuickMove:
    case Action::QuickCopy:
    case Action::SaveRotation:
    case Action::OpenInEditor:
    case Action::DragOut:
    case Action::Rate0:
    case Action::Rate1:
    case Action::Rate2:
    case Action::Rate3:
    case Action::Rate4:
    case Action::Rate5:
    case Action::RatePick:
    case Action::RateReject:
    case Action::RateClear:
    case Action::ToggleRawJpeg:
    case Action::ToggleSubfolders:
    case Action::SetWallpaper:
    case Action::QuickLook:
        return true;
    default:
        return false;
    }
}

QString blockedMessage() {
    return Tr::tr("Bilder in Archiven und PDF-Dokumenten sind nur zum Ansehen geöffnet – "
                  "Papierkorb, Umbenennen, Verschieben und Bewerten gehen hier nicht.");
}

Action readingDirection(Action action, const InputEvent& event, bool rightToLeft) {
    if (!rightToLeft || (action != Action::NextImage && action != Action::PreviousImage))
        return action;
    const bool arrow =
        event.kind == InputKind::Key && (event.key == Qt::Key_Left || event.key == Qt::Key_Right);
    const bool swipe = event.kind == InputKind::Swipe;
    if (!arrow && !swipe)
        return action;
    return action == Action::NextImage ? Action::PreviousImage : Action::NextImage;
}

} // namespace lf::containers
