// SPDX-License-Identifier: GPL-3.0-or-later
// Ratings and culling, pure logic (see Rating.h).

#include "Rating.h"

#include "InputBindings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace lf {

namespace {

// Tr::tr() for the code in this file outside QObject classes (lupdate takes the context from it).
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::Rating)
};

} // namespace

// ---- filter ----

bool RatingFilter::matches(const Rating& r) const {
    switch (kind) {
    case Kind::All:
        return true;
    case Kind::Picks:
        return r.isPick();
    case Kind::MinStars:
        return !r.isReject() && r.stars >= minStars;
    case Kind::Unrated:
        return r.isEmpty();
    case Kind::Rejects:
        return r.isReject();
    }
    return true;
}

QString RatingFilter::id() const {
    switch (kind) {
    case Kind::All:
        return QStringLiteral("all");
    case Kind::Picks:
        return QStringLiteral("picks");
    case Kind::MinStars:
        return QStringLiteral("min%1").arg(std::clamp(minStars, 1, 5));
    case Kind::Unrated:
        return QStringLiteral("unrated");
    case Kind::Rejects:
        return QStringLiteral("rejects");
    }
    return QStringLiteral("all");
}

RatingFilter RatingFilter::fromId(const QString& id) {
    RatingFilter f;
    if (id == QLatin1String("picks")) {
        f.kind = Kind::Picks;
    } else if (id == QLatin1String("unrated")) {
        f.kind = Kind::Unrated;
    } else if (id == QLatin1String("rejects")) {
        f.kind = Kind::Rejects;
    } else if (id.startsWith(QLatin1String("min"))) {
        bool ok = false;
        const int n = id.mid(3).toInt(&ok);
        if (ok && n >= 1 && n <= 5) {
            f.kind = Kind::MinStars;
            f.minStars = n;
        }
    }
    return f;
}

QString RatingFilter::describe() const {
    switch (kind) {
    case Kind::All:
        return Tr::tr("alle");
    case Kind::Picks:
        return Tr::tr("nur Picks");
    case Kind::MinStars:
        return Tr::tr("≥ %1 ★").arg(std::clamp(minStars, 1, 5));
    case Kind::Unrated:
        return Tr::tr("nur unbewertete");
    case Kind::Rejects:
        return Tr::tr("nur verworfene");
    }
    return {};
}

namespace rating {

QString describe(const Rating& r) {
    if (r.isEmpty())
        return QStringLiteral("–");
    QString s;
    if (r.stars > 0)
        s = QString(r.stars, QChar(0x2605)) + QString(5 - r.stars, QChar(0x2606));
    if (r.isPick())
        s += (s.isEmpty() ? QString() : QStringLiteral(" · ")) + Tr::tr("Pick");
    else if (r.isReject())
        s += (s.isEmpty() ? QString() : QStringLiteral(" · ")) + Tr::tr("Verworfen");
    return s;
}

int sortValue(const Rating& r) {
    if (r.isReject())
        return -1;
    return std::clamp(r.stars, 0, 5) * 2 + (r.isPick() ? 1 : 0);
}

bool isRatingAction(Action a) {
    switch (a) {
    case Action::Rate0:
    case Action::Rate1:
    case Action::Rate2:
    case Action::Rate3:
    case Action::Rate4:
    case Action::Rate5:
    case Action::RatePick:
    case Action::RateReject:
    case Action::RateClear:
        return true;
    default:
        return false;
    }
}

Rating apply(const Rating& current, Action a) {
    Rating r = current;
    switch (a) {
    case Action::Rate0:
        r.stars = 0;
        break;
    case Action::Rate1:
        r.stars = 1;
        break;
    case Action::Rate2:
        r.stars = 2;
        break;
    case Action::Rate3:
        r.stars = 3;
        break;
    case Action::Rate4:
        r.stars = 4;
        break;
    case Action::Rate5:
        r.stars = 5;
        break;
    case Action::RatePick:
        r.flag = PickFlag::Pick;
        break;
    case Action::RateReject:
        r.flag = PickFlag::Reject;
        break;
    case Action::RateClear:
        r.flag = PickFlag::None;
        break;
    default:
        break;
    }
    return r;
}

Step cullingStep(const Rating& current, Action a, bool autoAdvance) {
    Step s;
    s.rating = apply(current, a);
    s.advance = autoAdvance && isRatingAction(a);
    return s;
}

// ---- sidecars ----

QString sidecarPath(const QString& imagePath) {
    const QFileInfo fi(imagePath);
    return fi.dir().filePath(fi.completeBaseName() + QStringLiteral(".xmp"));
}

QString sidecarPathWithExtension(const QString& imagePath) {
    return imagePath + QStringLiteral(".xmp");
}

QStringList sidecarCandidates(const QString& imagePath) {
    const QFileInfo fi(imagePath);
    const QString base = fi.dir().filePath(fi.completeBaseName());
    // The variant with the extension names exactly one image: it wins over the Adobe one
    // (which RAW+JPEG pairs share).
    return {imagePath + QStringLiteral(".xmp"), base + QStringLiteral(".xmp"),
            imagePath + QStringLiteral(".XMP"), base + QStringLiteral(".XMP")};
}

// ---- Finder tags ----

QString tagName(const QString& raw) {
    const qsizetype nl = raw.indexOf(QLatin1Char('\n'));
    return nl < 0 ? raw : raw.left(nl);
}

int tagColor(const QString& raw) {
    const qsizetype nl = raw.indexOf(QLatin1Char('\n'));
    if (nl < 0)
        return 0;
    bool ok = false;
    const int c = raw.mid(nl + 1).toInt(&ok);
    return ok && c >= 0 && c <= 7 ? c : 0;
}

QString rawTag(const QString& name, int color) {
    return color > 0 && color <= 7 ? name + QLatin1Char('\n') + QString::number(color) : name;
}

namespace {

// Stars encoded by a tag name ("★★★" -> 3), 0 if it is not a star tag.
int starsOf(const QString& name, const RatingTagNames& names) {
    if (names.starSymbol.isEmpty() || name.isEmpty() || name.size() % names.starSymbol.size() != 0)
        return 0;
    const int n = int(name.size() / names.starSymbol.size());
    if (n < 1 || n > 5)
        return 0;
    return name == names.starSymbol.repeated(n) ? n : 0;
}

bool isOurTag(const QString& name, const RatingTagNames& names) {
    return starsOf(name, names) > 0 || (!names.pick.isEmpty() && name == names.pick) ||
           (!names.reject.isEmpty() && name == names.reject);
}

} // namespace

Rating fromTags(const QStringList& rawTags, const RatingTagNames& names, bool* found) {
    Rating r;
    bool any = false;
    for (const QString& raw : rawTags) {
        const QString name = tagName(raw);
        if (const int s = starsOf(name, names)) {
            r.stars = std::max(r.stars, s);
            any = true;
        } else if (!names.reject.isEmpty() && name == names.reject) {
            r.flag = PickFlag::Reject; // reject wins over a (stale) pick tag
            any = true;
        } else if (!names.pick.isEmpty() && name == names.pick) {
            if (r.flag != PickFlag::Reject)
                r.flag = PickFlag::Pick;
            any = true;
        }
    }
    if (found)
        *found = any;
    return r;
}

QStringList applyToTags(const QStringList& existing, const Rating& r, const RatingTagNames& names) {
    QStringList out;
    for (const QString& raw : existing)
        if (!isOurTag(tagName(raw), names))
            out << raw;
    if (r.stars > 0 && !names.starSymbol.isEmpty())
        out << rawTag(names.starSymbol.repeated(std::clamp(r.stars, 1, 5)), names.starColor);
    if (r.isPick() && !names.pick.isEmpty())
        out << rawTag(names.pick, names.pickColor);
    if (r.isReject() && !names.reject.isEmpty())
        out << rawTag(names.reject, names.rejectColor);
    return out;
}

} // namespace rating

} // namespace lf
