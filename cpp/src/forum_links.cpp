#include "forum_links.h"

#include <QRegularExpression>
#include <QUrl>

namespace {
QString hostKey(const QUrl &url)
{
    const QString original = url.host().toLower();
    QString host = original.contains(':') ? original : QString::fromLatin1(QUrl::toAce(original));
    if (host.isEmpty()) host = original;
    if (host.endsWith('.')) host.chop(1);
    if (host.startsWith("www.")) host.remove(0, 4);
    return host;
}

int portKey(const QUrl &url)
{
    const int port = url.port(-1);
    return (url.scheme() == "http" && port == 80)
        || (url.scheme() == "https" && port == 443) ? -1 : port;
}

bool isWebUrl(const QUrl &url)
{
    return url.isValid() && !url.host().isEmpty()
        && (url.scheme() == "http" || url.scheme() == "https");
}

QString pathKey(const QUrl &url)
{
    QString path = url.adjusted(QUrl::NormalizePathSegments).path(QUrl::FullyEncoded);
    while (path.endsWith('/')) path.chop(1);
    return path;
}
}

QStringList ForumLinks::fromBbCode(const QString &message)
{
    static const QRegularExpression body(
        R"bb(\[(url|img)(?:=[^\]]*)?\]\s*(https?://[^\[\s]+)\s*\[/\1\])bb",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression attribute(
        R"bb(\[url\s*=\s*(['"]?)(https?://[^\]\s"']+)\1\s*\])bb",
        QRegularExpression::CaseInsensitiveOption);
    QStringList result;
    for (const auto &expression : {body, attribute}) {
        auto matches = expression.globalMatch(message);
        while (matches.hasNext()) {
            const QString value = matches.next().captured(2).trimmed();
            if (isWebUrl(QUrl(value)) && !result.contains(value))
                result.append(value);
        }
    }
    return result;
}

bool ForumLinks::belongsToForum(const QString &reference, const QString &forumUrl)
{
    const QUrl link(reference.trimmed());
    const QUrl forum(forumUrl.trimmed());
    if (!isWebUrl(link) || !isWebUrl(forum) || hostKey(link) != hostKey(forum)
        || portKey(link) != portKey(forum))
        return false;
    // HTTP/HTTPS и www допускаются, другие доменные алиасы — только если
    // браузер действительно перенаправил в рекламную тему на таком домене.
    const QString base = pathKey(forum);
    const QString path = pathKey(link);
    return base.isEmpty() || path == base || path.startsWith(base + '/');
}

bool ForumLinks::containsForum(const QStringList &references, const QString &forumUrl)
{
    for (const auto &reference : references)
        if (belongsToForum(reference, forumUrl)) return true;
    return false;
}

QString ForumLinks::forumFromTopic(const QString &topicUrl)
{
    QUrl url(topicUrl);
    if (!isWebUrl(url)) return {};
    const int topic = url.path().lastIndexOf("/viewtopic");
    if (topic < 0) return {}; // Не объявляем произвольную страницу алиасом форума.
    url.setPath(url.path().left(topic));
    url.setQuery(QString());
    url.setFragment(QString());
    url.setUserInfo(QString());
    return url.toString(QUrl::FullyEncoded);
}
