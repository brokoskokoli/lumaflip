// SPDX-License-Identifier: GPL-3.0-or-later
#include "UpdateConfig.h"

#include <QByteArray>
#include <QUrl>

namespace lf::update {

QString configurationProblem(const QString& feedUrl, const QString& publicKey) {
    const QUrl url(feedUrl);
    if (feedUrl.isEmpty() || !url.isValid())
        return QStringLiteral("feed URL missing or invalid");
    // https only; file: never touches the network (tests).
    if (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("file"))
        return QStringLiteral("feed URL must use https");
    if (url.scheme() == QLatin1String("https") && url.host().isEmpty())
        return QStringLiteral("feed URL has no host");
    if (publicKey.isEmpty())
        return QStringLiteral("EdDSA public key missing - never ship updates without a key");
    const auto decoded =
        QByteArray::fromBase64Encoding(publicKey.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() != 32)
        return QStringLiteral("public key is not a base64 EdDSA (ed25519) public key");
    return {};
}

QString winSparkleLanguage(const QString& uiLanguageCode) {
    if (uiLanguageCode.isEmpty() || uiLanguageCode == QLatin1String("system"))
        return {};
    QString lang = uiLanguageCode;
    lang.replace(QLatin1Char('_'), QLatin1Char('-'));
    return lang;
}

} // namespace lf::update
