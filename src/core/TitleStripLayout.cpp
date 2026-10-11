// SPDX-License-Identifier: GPL-3.0-or-later
#include "TitleStripLayout.h"

#include <QStringList>

#include <algorithm>

namespace lf {

namespace {

constexpr int kTitleGap = 8;          // between the buttons and the title text
constexpr int kMinCenteredWidth = 80; // narrower: the title uses the free space as it is

} // namespace

TitleStripLayout layoutTitleStrip(const TitleStripSpec& spec) {
    TitleStripLayout l;
    const int w = std::max(0, spec.width);
    const int h = std::max(0, spec.height);
    const int bw = std::max(1, spec.buttonWidth);
    int left = std::clamp(spec.reservedLeft, 0, w);
    int right = w;
    if (spec.ownButtons) {
        if (spec.side == TitleButtonSide::Right) {
            // Windows order from the right edge: close, maximize, minimize.
            l.close = QRect(right - bw, 0, bw, h);
            l.maximize = QRect(right - 2 * bw, 0, bw, h);
            l.minimize = QRect(right - 3 * bw, 0, bw, h);
            right -= 3 * bw;
        } else {
            // Left like macOS / GNOME "close,minimize,maximize:".
            l.close = QRect(left, 0, bw, h);
            l.minimize = QRect(left + bw, 0, bw, h);
            l.maximize = QRect(left + 2 * bw, 0, bw, h);
            left += 3 * bw;
        }
    }
    if (spec.menuButton) {
        if (spec.ownButtons && spec.side == TitleButtonSide::Left) {
            l.menu = QRect(right - bw, 0, bw, h);
            right -= bw;
        } else {
            l.menu = QRect(left, 0, bw, h);
            left += bw;
        }
    }
    left = std::min(left + kTitleGap, w);
    right = std::max(right - kTitleGap, left);
    l.titleArea = QRect(left, 0, right - left, h);
    const int inset = std::max(left, w - right);
    if (w - 2 * inset >= kMinCenteredWidth)
        l.centeredTitle = QRect(inset, 0, w - 2 * inset, h);
    return l;
}

TitleButton titleButtonAt(const TitleStripLayout& l, QPoint p) {
    if (l.close.contains(p))
        return TitleButton::Close;
    if (l.maximize.contains(p))
        return TitleButton::Maximize;
    if (l.minimize.contains(p))
        return TitleButton::Minimize;
    if (l.menu.contains(p))
        return TitleButton::Menu;
    return TitleButton::None;
}

TitleButtonSide buttonSideFromGnomeLayout(const QString& value, TitleButtonSide fallback) {
    QString v = value.trimmed();
    if (v.size() >= 2 && (v.front() == QLatin1Char('\'') || v.front() == QLatin1Char('"')))
        v = v.mid(1, v.size() - 2);
    const qsizetype colon = v.indexOf(QLatin1Char(':'));
    if (colon < 0)
        return fallback;
    const QString leftPart = v.left(colon);
    const QString rightPart = v.mid(colon + 1);
    auto has = [](const QString& part, const char* name) {
        const auto items = part.split(QLatin1Char(','), Qt::SkipEmptyParts);
        return std::any_of(items.cbegin(), items.cend(),
                           [name](const QString& s) { return s.trimmed() == QLatin1String(name); });
    };
    for (const char* name : {"close", "maximize", "minimize"}) {
        if (has(leftPart, name))
            return TitleButtonSide::Left;
        if (has(rightPart, name))
            return TitleButtonSide::Right;
    }
    return fallback;
}

TitleButtonSide buttonSideFromKdeButtons(const QString& onLeft, const QString& onRight,
                                         TitleButtonSide fallback) {
    if (onLeft.contains(QLatin1Char('X')))
        return TitleButtonSide::Left;
    if (onRight.contains(QLatin1Char('X')))
        return TitleButtonSide::Right;
    return fallback;
}

TitleDoubleClick titleDoubleClickAction(const QString& action, std::optional<bool> miniaturize) {
    const QString a = action.trimmed();
    if (a.isEmpty()) {
        // Older macOS: only "Minimize" or the default zoom.
        return miniaturize.value_or(false) ? TitleDoubleClick::Minimize : TitleDoubleClick::Zoom;
    }
    if (a.compare(QLatin1String("Minimize"), Qt::CaseInsensitive) == 0)
        return TitleDoubleClick::Minimize;
    if (a.compare(QLatin1String("None"), Qt::CaseInsensitive) == 0)
        return TitleDoubleClick::Nothing;
    return TitleDoubleClick::Zoom; // "Maximize" (Zoom), "Fill", unknown values
}

} // namespace lf
