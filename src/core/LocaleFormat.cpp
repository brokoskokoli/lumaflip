// SPDX-License-Identifier: GPL-3.0-or-later
#include "LocaleFormat.h"

#include <QCoreApplication>

#include <cmath>

namespace lf {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::Format)
};

// The plural form of `n` items for a 64-bit count: the numerus rules look at n, n % 10,
// n % 100 (and "n >= 2" or similar), which this keeps for any n >= 1000000.
int pluralIndex(qint64 n) {
    return n < 1000000 ? int(n) : int(n % 1000000) + 1000000;
}

// Numbers that read as one token without group separators: exposure denominators
// ("1/4000 s", never "1/4.000 s"), apertures and focal lengths ("1200 mm").
QLocale withoutGroupSeparator(QLocale locale) {
    locale.setNumberOptions(locale.numberOptions() | QLocale::OmitGroupSeparator);
    return locale;
}

// The separator the locale puts between minutes and seconds: the one before the minutes
// ("HH.mm" in Danish and Finnish -> "."), ":" if there is none or it is quoted text.
QString minuteSeparator(const QString& time, qsizetype minutes) {
    qsizetype start = minutes;
    while (start > 0) {
        const QChar c = time.at(start - 1);
        if (c == u'h' || c == u'H')
            break;
        --start;
    }
    if (start == 0)
        return QStringLiteral(":");
    const QString sep = time.mid(start, minutes - start);
    for (const QChar c : sep)
        if (c.isLetter() || c == u'\'' || c.isSpace())
            return QStringLiteral(":");
    return sep.isEmpty() ? QStringLiteral(":") : sep;
}

QString unitText(int unit, const QString& number) {
    switch (unit) {
    case 0:
        //: SI kilobyte (1000 bytes) as in the Finder; e.g. French "%1 ko"
        return Tr::tr("%1 KB").arg(number);
    case 1:
        //: SI megabyte as in the Finder; e.g. French "%1 Mo"
        return Tr::tr("%1 MB").arg(number);
    case 2:
        //: SI gigabyte as in the Finder; e.g. French "%1 Go"
        return Tr::tr("%1 GB").arg(number);
    case 3:
        //: SI terabyte as in the Finder; e.g. French "%1 To"
        return Tr::tr("%1 TB").arg(number);
    default:
        //: SI petabyte as in the Finder; e.g. French "%1 Po"
        return Tr::tr("%1 PB").arg(number);
    }
}

} // namespace

QString formatByteSize(qint64 bytes, const QLocale& locale) {
    if (bytes < 1000)
        //: file size below 1000 bytes; %n is the number of bytes
        return Tr::tr("%n Byte", nullptr, int(std::max<qint64>(bytes, 0)));
    double v = double(bytes);
    int u = -1;
    while (v >= 999.95 && u < 4) {
        v /= 1000.0;
        ++u;
    }
    return unitText(u, locale.toString(v, 'f', 1));
}

QString formatByteSizeExact(qint64 bytes, const QLocale& locale) {
    if (bytes < 1000)
        return formatByteSize(bytes, locale);
    //: size and exact number of bytes, e.g. "2,8 MB (2.812.345 Byte)"; plural by the byte count
    return Tr::tr("%1 (%2 Byte)", nullptr, pluralIndex(bytes))
        .arg(formatByteSize(bytes, locale), locale.toString(bytes));
}

QString formatDecimal(double value, int decimals, const QLocale& locale) {
    QString s = locale.toString(value, 'f', decimals);
    const QString point = locale.decimalPoint();
    if (decimals > 0 && s.contains(point)) {
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(point))
            s.chop(point.size());
    }
    return s;
}

QString formatDateTime(const QDateTime& dt, const QLocale& locale) {
    if (!dt.isValid())
        return {};
    QString date = locale.dateFormat(QLocale::ShortFormat);
    if (!date.contains(QLatin1String("yyyy")))
        date.replace(QLatin1String("yy"), QLatin1String("yyyy"));
    QString time = locale.timeFormat(QLocale::ShortFormat);
    if (!time.contains(QLatin1String("ss"))) {
        const qsizetype m = time.indexOf(QLatin1String("mm"));
        if (m >= 0)
            time.insert(m + 2, minuteSeparator(time, m) + QLatin1String("ss"));
    }
    return locale.toString(dt, date + QLatin1Char(' ') + time);
}

QString formatExposureTime(double t, const QLocale& locale) {
    if (t <= 0)
        return {};
    if (t < 1.0) {
        const double inv = 1.0 / t;
        //: exposure time as a fraction of a second, e.g. "1/250 s"
        return Tr::tr("1/%1 s").arg(
            formatDecimal(inv >= 10 ? std::round(inv) : inv, 1, withoutGroupSeparator(locale)));
    }
    //: exposure time in seconds, e.g. "2,5 s"
    return Tr::tr("%1 s").arg(formatDecimal(t, 1, withoutGroupSeparator(locale)));
}

QString formatAperture(double fNumber, const QLocale& locale) {
    //: aperture (f-number), e.g. "f/2,8"
    return Tr::tr("f/%1").arg(formatDecimal(fNumber, 1, withoutGroupSeparator(locale)));
}

QString formatExposureBias(double ev, const QLocale& locale) {
    const QString num = formatDecimal(ev, 1, locale);
    //: exposure compensation in exposure values; %1 is the signed number, e.g. "+0,7"
    return Tr::tr("%1 EV").arg(ev > 0 ? locale.positiveSign() + num : num);
}

QString formatFocalLength(double mm, double mm35, const QLocale& locale) {
    const QLocale plain = withoutGroupSeparator(locale);
    //: focal length in millimetres
    QString s = Tr::tr("%1 mm").arg(formatDecimal(mm, 1, plain));
    if (mm35 > 0 && mm35 != mm)
        //: appended to the focal length: the 35 mm equivalent ("KB" = Kleinbild), e.g. " (35-mm-equiv.: 52
        //: mm)"
        s += Tr::tr(" (KB: %1 mm)").arg(formatDecimal(mm35, 0, plain));
    return s;
}

static QString coordinate(double degrees, const QString& hemisphere, const QLocale& locale) {
    //: GPS coordinate: degrees and hemisphere letter, e.g. "48,137154° N"
    return Tr::tr("%1° %2").arg(formatDecimal(std::abs(degrees), 6, locale), hemisphere);
}

QString formatLatitude(double degrees, const QLocale& locale) {
    if (degrees < 0)
        //: GPS hemisphere: abbreviation of "south"
        return coordinate(degrees, Tr::tr("S", "south"), locale);
    //: GPS hemisphere: abbreviation of "north"
    return coordinate(degrees, Tr::tr("N", "north"), locale);
}

QString formatLongitude(double degrees, const QLocale& locale) {
    if (degrees < 0)
        //: GPS hemisphere: abbreviation of "west"
        return coordinate(degrees, Tr::tr("W", "west"), locale);
    //: GPS hemisphere: abbreviation of "east" (German "O" = Ost)
    return coordinate(degrees, Tr::tr("O", "east"), locale);
}

QString formatAltitude(double meters, const QLocale& locale) {
    //: altitude in metres
    return Tr::tr("%1 m").arg(formatDecimal(meters, 1, locale));
}

} // namespace lf
