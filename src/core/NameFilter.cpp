// SPDX-License-Identifier: GPL-3.0-or-later
#include "NameFilter.h"

#include <QRegularExpression>

namespace lf::namefilter {

QString fold(const QString& text) {
    const QString decomposed = text.normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        const QChar::Category cat = c.category();
        if (cat == QChar::Mark_NonSpacing || cat == QChar::Mark_SpacingCombining || cat == QChar::Mark_Enclosing)
            continue; // diacritics ("ä" -> "a")
        out.append(c);
    }
    // Case folding; QString folds one character to one, so "ß"/"ẞ" -> "ss" is done here
    // (full folding as in Unicode CaseFolding.txt). Composed again so the result is stable.
    QString folded = out.toCaseFolded();
    folded.replace(QChar(0x00DF), QStringLiteral("ss"));
    folded.replace(QChar(0x1E9E), QStringLiteral("ss"));
    return folded.normalized(QString::NormalizationForm_C);
}

QStringList terms(const QString& query) {
    static const QRegularExpression space(QStringLiteral("\\s+"));
    QStringList out;
    for (const QString& part : query.split(space, Qt::SkipEmptyParts)) {
        const QString f = fold(part);
        if (!f.isEmpty())
            out << f;
    }
    return out;
}

bool matchesTerms(const QString& fileName, const QStringList& foldedTerms) {
    if (foldedTerms.isEmpty())
        return true;
    const QString name = fold(fileName);
    for (const QString& t : foldedTerms)
        if (!name.contains(t))
            return false;
    return true;
}

bool matches(const QString& fileName, const QString& query) {
    return matchesTerms(fileName, terms(query));
}

} // namespace lf::namefilter
