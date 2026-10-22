#include "forum_page.h"
#include "webdriver_client.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <utility>

namespace {
// Читаем DOM одной командой: профильные ссылки вне тела поста указывают автора,
// а href/src внутри тела дают признаки BBCode даже у рекламы из одной картинки.
const QString kSnapshot = QStringLiteral(R"JS(
    return {
        pageUrl: location.href,
        userId: typeof UserID !== 'undefined' ? String(UserID) : '',
        guest: typeof GroupID !== 'undefined' && String(GroupID) === '3',
        errors: Array.from(document.querySelectorAll(
            '#post-errors, .errorlist, #pun-main .form-error, #pun-main .error'))
            .map(e => e.textContent.trim()).filter(Boolean).join('; '),
        posts: Array.from(document.querySelectorAll('.post[id]'))
            .filter(p => /^p\d+$/.test(p.id)).map(p => {
                const body = p.querySelector('.post-content');
                const profileLinks = Array.from(p.querySelectorAll('a[href*="profile.php"]'))
                    .filter(a => !body || !body.contains(a));
                const authors = profileLinks.map(a => new URL(a.href).searchParams.get('id'));
                const link = Array.from(p.querySelectorAll('a[href*="viewtopic.php"]'))
                    .find(a => new URL(a.href).hash === '#' + p.id);
                const fallback = new URL(location.href);
                fallback.hash = p.id;
                return {
                    id: p.id, authors,
                    text: body ? body.textContent : '',
                    references: body ? Array.from(body.querySelectorAll('a[href], img[src]'))
                        .map(e => e.href || e.src) : [],
                    permalink: link ? link.href :
                        (fallback.pathname.endsWith('/viewtopic.php') ? fallback.href : '')
                };
            })
    };
)JS");

QString referenceKey(const QString &text)
{
    QUrl url(text.trimmed());
    if (!url.isValid() || url.host().isEmpty()
        || (url.scheme() != "http" && url.scheme() != "https"))
        return {};
    if ((url.scheme() == "http" && url.port() == 80)
        || (url.scheme() == "https" && url.port() == 443))
        url.setPort(-1);
    url.setScheme("https"); // Одна ссылка могла быть перенаправлена с HTTP на HTTPS.
    return url.adjusted(QUrl::NormalizePathSegments | QUrl::StripTrailingSlash)
        .toString(QUrl::FullyEncoded);
}

QSet<QString> messageReferences(const QString &message)
{
    const QRegularExpression expressions[] = {
        QRegularExpression(R"ref(\[(?:img|url)(?:=[^\]]+)?\](https?://[^\[]+)\[/(?:img|url)\])ref",
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(R"ref(\[url=["']?(https?://[^\]"']+)["']?\])ref",
                           QRegularExpression::CaseInsensitiveOption)
    };
    QSet<QString> result;
    for (const auto &expression : expressions) {
        auto matches = expression.globalMatch(message);
        while (matches.hasNext()) {
            const QString key = referenceKey(matches.next().captured(1));
            if (!key.isEmpty())
                result.insert(key);
        }
    }
    return result;
}

QString plainMessage(QString text)
{
    static const QRegularExpression bbcode(R"(\[/?[a-z][^\]]*\])",
                                           QRegularExpression::CaseInsensitiveOption);
    text.remove(bbcode);
    return text.simplified().toCaseFolded();
}
}

PublicationError::PublicationError(Status status, const QString &message)
    : std::runtime_error(message.toStdString()), m_status(status)
{
}

ForumPage::ForumPage(WebDriverClient &driver, std::function<void()> checkpoint)
    : m_driver(driver), m_checkpoint(std::move(checkpoint))
{
}

QString ForumPage::publish(const QString &message, const QString &requiredBacklink,
                           const std::function<void()> &beforeSubmit, int timeoutMs)
{
    if (m_checkpoint)
        m_checkpoint();
    const QJsonObject before = m_driver.execute(kSnapshot).toObject();
    const QString author = before.value("userId").toString();
    if (author.isEmpty() || author == "0" || before.value("guest").toBool())
        throw std::runtime_error("Перед отправкой не подтверждён вход в аккаунт.");

    QSet<QString> previousIds;
    qlonglong largestId = 0;
    for (const auto &value : before.value("posts").toArray()) {
        const QString id = value.toObject().value("id").toString();
        previousIds.insert(id);
        largestId = qMax(largestId, id.mid(1).toLongLong());
    }
    QSet<QString> expectedReferences = messageReferences(message);
    const QString expectedText = plainMessage(message);
    const QString backlink = referenceKey(requiredBacklink);
    // Наличие одной взаимной ссылки не доказывает, что само объявление дошло.
    // Её проверяем отдельно; остальные ссылки/картинки должны сохраниться все.
    if (!backlink.isEmpty())
        expectedReferences.remove(backlink);
    if (message.trimmed().isEmpty() || (expectedReferences.isEmpty() && expectedText.isEmpty()))
        throw std::runtime_error("Нет содержимого для проверяемой публикации.");

    m_driver.execute(R"JS(
        const field = document.querySelector('#main-reply');
        if (!field) throw new Error('Не найдена форма ответа');
        if ('value' in field) field.value = arguments[0];
        else field.textContent = arguments[0];
        field.dispatchEvent(new Event('input', { bubbles: true }));
        field.dispatchEvent(new Event('change', { bubbles: true }));
        const current = ('value' in field ? field.value : field.textContent) || '';
        if (!current.trim()) throw new Error('Поле ответа осталось пустым после заполнения');
    )JS", {message});
    if (m_checkpoint)
        m_checkpoint();
    if (beforeSubmit)
        beforeSubmit();

    // После клика ни тайм-аут, ни потеря связи не доказывают, что сервер ничего
    // не записал. Возвращаем неопределённый результат и никогда не кликаем снова.
    try {
        m_driver.execute(R"JS(
            const field = document.querySelector('#main-reply');
            const button = field && field.form
                ? field.form.querySelector('input.button.submit, button[type="submit"]') : null;
            if (!button) throw new Error('Не найдена кнопка отправки в форме ответа');
            button.click();
        )JS");
        return m_driver.waitFor([&]() -> QJsonValue {
            const QJsonObject current = m_driver.execute(kSnapshot).toObject();
            const QString errors = current.value("errors").toString();
            if (!errors.isEmpty())
                throw PublicationError(PublicationError::Status::Rejected,
                                       "Форум отклонил отправку: " + errors);
            for (const auto &value : current.value("posts").toArray()) {
                const QJsonObject post = value.toObject();
                const QString id = post.value("id").toString();
                if (previousIds.contains(id) || id.mid(1).toLongLong() <= largestId
                    || !post.value("authors").toArray().contains(author))
                    continue;
                QSet<QString> references;
                for (const auto &ref : post.value("references").toArray())
                    references.insert(referenceKey(ref.toString()));
                if (!backlink.isEmpty() && !references.contains(backlink))
                    continue;
                bool contentMatches = expectedReferences.isEmpty()
                    ? post.value("text").toString().simplified().toCaseFolded().contains(expectedText)
                    : true;
                for (const auto &reference : expectedReferences)
                    contentMatches = contentMatches && references.contains(reference);
                const QString permalink = post.value("permalink").toString();
                if (contentMatches && !permalink.isEmpty())
                    return permalink;
            }
            return false;
        }, "появление нового сообщения от текущего аккаунта", timeoutMs, m_checkpoint).toString();
    } catch (const PublicationError &) {
        throw;
    } catch (const std::exception &error) {
        throw PublicationError(PublicationError::Status::Unknown,
            QString("Результат отправки не подтверждён: %1. Автоматический повтор не выполняется.")
                .arg(QString::fromUtf8(error.what())));
    }
}
