// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Locale-aware display texts (F-I18N-04): sizes, dates, exposure values and GPS
// coordinates. Numbers and dates follow `locale` (default: formatLocale(), i.e. the UI
// language in the user's region, see Language.h); units and words are translatable
// (context "lf::Format"). Any thread.

#include "Language.h"

#include <QDateTime>
#include <QLocale>
#include <QString>

namespace lf {

// "680 Byte", "2,8 MB" / "2.8 MB" (SI units like the Finder; French "2,8 Mo").
QString formatByteSize(qint64 bytes, const QLocale& locale = formatLocale());
// Info panel: "2,8 MB (2.812.345 Byte)"; below 1000 bytes only "680 Byte".
QString formatByteSizeExact(qint64 bytes, const QLocale& locale = formatLocale());

// `value` with at most `decimals` decimals, trailing zeros removed ("2,8", "2", "0,33").
QString formatDecimal(double value, int decimals, const QLocale& locale = formatLocale());

// Date and time with seconds and four-digit year in the locale's short format
// ("10.10.2026 14:03:09", "10/10/2026 2:03:09 PM", "2026/10/10 14:03:09"; Danish
// "10.10.2026 14.03.09": the seconds get the separator the locale puts before the
// minutes). The time is shown as stored in `dt` (convert with toLocalTime() first if needed).
QString formatDateTime(const QDateTime& dt, const QLocale& locale = formatLocale());

// Exposure values (info panel, F-INFO-02). Exposure time, aperture and focal length have
// no group separators ("1/4000 s", "1200 mm").
QString formatExposureTime(double seconds, const QLocale& locale = formatLocale()); // "1/250 s", "2,5 s"
QString formatAperture(double fNumber, const QLocale& locale = formatLocale());     // "f/2,8"
QString formatExposureBias(double ev, const QLocale& locale = formatLocale());      // "+0,7 EV"
// "50 mm", with a different 35 mm equivalent "35 mm (KB: 52 mm)".
QString formatFocalLength(double mm, double mm35, const QLocale& locale = formatLocale());

// GPS: "48,137154° N", "11,575490° O" (German O = east, translatable), "519 m".
QString formatLatitude(double degrees, const QLocale& locale = formatLocale());
QString formatLongitude(double degrees, const QLocale& locale = formatLocale());
QString formatAltitude(double meters, const QLocale& locale = formatLocale());

} // namespace lf
