#include "forum_page.h"
#include "webdriver_client.h"
#include "forum_links.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>
#include <utility>

namespace {
// Читаем DOM одной командой: профильные ссылки вне тела поста указывают автора,
// а href/src внутри тела дают признаки BBCode даже у рекламы из одной картинки.
const QString kSnapshot = QStringLiteral(R"JS(
    const field = document.querySelector('#main-reply');
    const button = field && field.form
        ? field.form.querySelector('input.button.submit, button[type="submit"]') : null;
    return {
        pageUrl: location.href,
        documentTime: performance.timeOrigin,
        formAction: button && button.hasAttribute('formaction') ? button.formAction
            : field && field.form ? field.form.action : '',
        userId: typeof UserID !== 'undefined' ? String(UserID) : '',
        guest: typeof GroupID !== 'undefined' && String(GroupID) === '3',
        // Часть форумов включает кнопку только после заполнения текста.
        replyReady: !!field && !field.disabled && !!button,
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

QString linkedForumHome(const QString &reference)
{
    QUrl url(reference.trimmed());
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()
        || (url.scheme() != "http" && url.scheme() != "https")) return {};
    // Ссылка вокруг картинки может вести прямо на файл фотохостинга.
    // Такие файлы не должны создавать отдельный «форум» в статистике.
    static const QRegularExpression asset(
        R"(\.(?:avif|bmp|gif|ico|jpe?g|png|svg|webp|mp[34]|webm|ogg|wav|pdf|zip|rar|7z|css|js)$)",
        QRegularExpression::CaseInsensitiveOption);
    if (asset.match(url.path()).hasMatch()) return {};
    url = QUrl(referenceKey(reference));
    QString host = url.host().toLower();
    if (host.startsWith("www.")) host.remove(0, 4);
    if (host.endsWith('.')) host.chop(1);
    url.setHost(host);
    // У форумов в подкаталоге сохраняем путь, когда его можно определить
    // по стандартной PHP-странице. Прочие ссылки группируем по полному хосту;
    // разные поддомены RusFF остаются разными площадками.
    static const QRegularExpression endpoint(
        R"(/(?:viewtopic|viewforum|index|profile|search|post|misc|login|register)\.php$)",
        QRegularExpression::CaseInsensitiveOption);
    const QString path = url.path();
    const auto match = endpoint.match(path);
    url.setPath(match.hasMatch() ? path.left(match.capturedStart()) : QString());
    url.setQuery(QString());
    url.setFragment(QString());
    return ForumLinks::normalizeForum(url.toString(QUrl::FullyEncoded));
}

QSet<QString> messageReferences(const QString &message)
{
    QSet<QString> result;
    for (const auto &reference : ForumLinks::fromBbCode(message)) {
        const QString key = referenceKey(reference);
        if (!key.isEmpty()) result.insert(key);
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

QString matchingPost(const QJsonObject &snapshot, const QJsonObject &proof, bool requireNew)
{
    // Совпадающая реклама от того же PR-аккаунта в другой теме не подтверждает
    // эту отправку. Номер страницы и якорь могут измениться после ответа.
    const QUrl actual(snapshot.value("pageUrl").toString()), topic(proof.value("topic_url").toString());
    const QString topicId = QUrlQuery(topic).queryItemValue("id");
    if (ForumLinks::forumIdentity(topic.toString()).isEmpty()
        || ForumLinks::forumIdentity(actual.toString()) != ForumLinks::forumIdentity(topic.toString())
        || (!topicId.isEmpty() && QUrlQuery(actual).queryItemValue("id") != topicId)) return {};
    auto expected = messageReferences(proof.value("message").toString());
    const QString text = plainMessage(proof.value("message").toString());
    const QString backlink = referenceKey(proof.value("backlink").toString());
    expected.remove(backlink);
    if (expected.isEmpty() && text.isEmpty()) return {};
    for (const auto &value : snapshot.value("posts").toArray()) {
        const auto post = value.toObject();
        const QString id = post.value("id").toString();
        if ((requireNew && (id.mid(1).toLongLong() <= proof.value("largest_id").toVariant().toLongLong()
             || proof.value("previous_ids").toArray().contains(id)))
            || !post.value("authors").toArray().contains(proof.value("author").toString())) continue;
        QSet<QString> refs;
        for (const auto &ref : post.value("references").toArray()) refs.insert(referenceKey(ref.toString()));
        bool matches = backlink.isEmpty() || refs.contains(backlink);
        // Ссылки сохраняются при форматировании BBCode; для чистого текста
        // проверяем видимое содержимое, а автора и ID проверяем всегда.
        if (expected.isEmpty()) matches &= post.value("text").toString().simplified().toCaseFolded().contains(text);
        for (const auto &ref : expected) matches &= refs.contains(ref);
        if (matches && !post.value("permalink").toString().isEmpty()) return post.value("permalink").toString();
    }
    return {};
}

}

PublicationError::PublicationError(Status status, const QString &message)
    : std::runtime_error(message.toStdString()), m_status(status)
{
}

ForumPage::ForumPage(WebDriverClient &driver, std::function<void()> checkpoint,
                     std::function<void(const QString &)> notice)
    : m_driver(driver), m_checkpoint(std::move(checkpoint)), m_notice(std::move(notice))
{
}

QJsonObject ForumPage::postingSnapshot()
{
    if (m_checkpoint)
        m_checkpoint();
    const QJsonObject before = m_driver.execute(kSnapshot).toObject();
    const QString author = before.value("userId").toString();
    bool numericAuthor = false;
    const qlonglong authorId = author.toLongLong(&numericAuthor);
    if (!numericAuthor || authorId <= 0 || before.value("guest").toBool())
        throw std::runtime_error("Перед отправкой не подтверждён вход в аккаунт.");
    if (!before.value("replyReady").toBool())
        throw std::runtime_error("Не найдена доступная форма ответа с кнопкой отправки.");
    return before;
}

void ForumPage::checkReady()
{
    postingSnapshot();
}

ForumLinkAnalysis ForumPage::analyseForumLinks(const QStringList &localForums)
{
    if (m_checkpoint) m_checkpoint();
    // Читаем только текущую страницу. Шаблон первого поста и взаимные ссылки
    // на местный форум не должны превращать одну рекламируемую площадку в две.
    const auto posts = m_driver.execute(R"JS(
        return Array.from(document.querySelectorAll('.post[id]'))
            .filter(post => /^p\d+$/.test(post.id))
            .filter(post => !(post.classList.contains('topicpost') && post.querySelector('.code-box')))
            .map(post => {
                const body = post.querySelector('.post-content');
                return body ? Array.from(body.querySelectorAll('a[href]'))
                    .filter(link => !link.closest('.code-box, .quote-box, blockquote, .post-sig, .signature, .sig-content'))
                    .map(link => link.href) : [];
            });
    )JS").toArray();
    ForumLinkAnalysis result;
    result.scannedPosts = int(posts.size());
    for (const auto &post : posts) {
        QSet<QString> forums;
        for (const auto &value : post.toArray()) {
            const QString reference = value.toString();
            bool local = false;
            for (const auto &forum : localForums)
                local |= ForumLinks::belongsToForum(reference, forum);
            if (local) continue;
            const QString home = linkedForumHome(reference);
            if (!home.isEmpty()) forums.insert(home);
        }
        if (forums.isEmpty()) continue;
        ++result.linkedPosts;
        for (const auto &forum : forums) ++result.postsByForum[forum];
    }
    return result;
}

QString ForumPage::publish(const QString &message, const QString &requiredBacklink,
                           const std::function<void()> &beforeSubmit, int timeoutMs,
                           const std::function<void(const QJsonObject &)> &prepared)
{
    const QJsonObject before = postingSnapshot();
    const QString author = before.value("userId").toString();

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
    QJsonArray ids;
    for (const auto &id : previousIds) ids.append(id);
    const QJsonObject proof{{"topic_url", before.value("pageUrl")}, {"author", author},
        {"form_action", before.value("formAction")},
        {"previous_ids", ids}, {"largest_id", QString::number(largestId)},
        {"message", message}, {"backlink", requiredBacklink}};
    // Ошибка записи на диск здесь прерывает отправку до клика.
    if (prepared) prepared(proof);
    if (beforeSubmit)
        beforeSubmit();

    // После клика ни тайм-аут, ни потеря связи не доказывают, что сервер ничего
    // не записал. Возвращаем неопределённый результат и никогда не кликаем снова.
    try {
        QJsonValue clicked;
        try {
            clicked = m_driver.execute(R"JS(
                const field = document.querySelector('#main-reply');
                const button = field && field.form
                    ? field.form.querySelector('input.button.submit, button[type="submit"]') : null;
                if (!button) return false;
                // При ручном ожидании страницы фоновые переходы не должны
                // подменить документ, аккаунт или содержимое перед кликом.
                const action = button.hasAttribute('formaction') ? button.formAction : field.form.action;
                const value = ('value' in field ? field.value : field.textContent) || '';
                if (location.href !== arguments[0] || action !== arguments[1]
                    || performance.timeOrigin !== arguments[2]
                    || typeof UserID === 'undefined' || String(UserID) !== arguments[3]
                    || (typeof GroupID !== 'undefined' && String(GroupID) === '3')
                    || value.replace(/\r\n?/g, '\n') !== arguments[4].replace(/\r\n?/g, '\n')) return false;
                button.click();
                return true;
            )JS", {before.value("pageUrl"), before.value("formAction"),
                   before.value("documentTime"), author, message});
        } catch (const WebDriverError &error) {
            if (error.kind() != WebDriverError::Kind::Timeout
                && error.kind() != WebDriverError::Kind::Browser) throw;
            // Клик мог успеть отправить POST. Проверяем результат только
            // чтением, никогда не повторяем эту команду.
            // Отсутствующий результат ниже также допускает только сверку.
        }
        // Только явное false доказывает, что проверка остановила команду до
        // клика. Потерянный/неожиданный ответ сохраняет неопределённый результат.
        if (!clicked.isBool() && m_notice)
            m_notice("ChromeDriver не подтвердил завершение клика. Проверяю появление сообщения без повторной отправки.");
        if (clicked.isBool() && !clicked.toBool())
            throw PublicationError(PublicationError::Status::Rejected,
                "Страница, форма, аккаунт или текст изменились перед кликом. Отправка не начиналась; обход остановлен.");
        try {
            return m_driver.waitFor([&]() -> QJsonValue {
                const QJsonObject current = m_driver.execute(kSnapshot).toObject();
                const QString match = matchingPost(current, proof, true);
                if (!match.isEmpty()) return match;
                const QString errors = current.value("errors").toString();
                if (!errors.isEmpty() && errors != before.value("errors").toString())
                    throw PublicationError(PublicationError::Status::Rejected,
                                           "Форум отклонил отправку: " + errors);
                return false;
            }, "появление нового сообщения от текущего аккаунта", timeoutMs, m_checkpoint).toString();
        } catch (const WebDriverError &error) {
            if (error.kind() != WebDriverError::Kind::Timeout) throw;
            if (m_notice) m_notice("Сообщение пока не подтверждено в открытой странице. Сверяю сохранённую тему и её последние страницы без повторной отправки.");
            const QString found = findPublished(proof);
            if (!found.isEmpty()) return found;
            throw;
        }
    } catch (const PublicationError &) {
        throw;
    } catch (const std::exception &error) {
        throw PublicationError(PublicationError::Status::Unknown,
            QString("Результат отправки не подтверждён: %1. Автоматический повтор не выполняется.")
                .arg(QString::fromUtf8(error.what())));
    }
}

QString ForumPage::findPublished(const QJsonObject &proof, bool requireNew)
{
    if (proof.value("author").toString().isEmpty() || proof.value("message").toString().isEmpty()
        || proof.value("topic_url").toString().isEmpty())
        throw std::runtime_error("Недостаточно сохранённых данных для сверки публикации.");
    const QString topic = proof.value("topic_url").toString();
    QStringList queue{topic};
    QSet<QString> visited;
    // Ограничиваем обход: отсутствие совпадения означает ручную сверку,
    // а не разрешение повторить отправку. Ищем также на соседней/последней странице.
    while (!queue.isEmpty() && visited.size() < 4) {
        const QString url = queue.takeFirst();
        if (visited.contains(url)) continue;
        visited.insert(url);
        if (m_checkpoint) m_checkpoint();
        m_driver.navigate(url, m_checkpoint);
        try {
            const auto found = m_driver.waitFor([&]() -> QJsonValue {
                const QString match = matchingPost(m_driver.execute(kSnapshot).toObject(), proof, requireNew);
                return match.isEmpty() ? QJsonValue(false) : QJsonValue(match);
            }, "сверка сохранённой публикации", 900, m_checkpoint).toString();
            if (!found.isEmpty()) return found;
        } catch (const WebDriverError &error) {
            if (error.kind() != WebDriverError::Kind::Timeout) throw;
        }
        const auto links = m_driver.execute(R"JS(
            return Array.from(document.querySelectorAll('.pagelink a[href]')).map(a => a.href).reverse();
        )JS").toArray();
        for (const auto &link : links) {
            const QUrl candidate(link.toString()), original(topic);
            if (candidate.host() == original.host() && candidate.port() == original.port()
                && candidate.path() == original.path()
                && QUrlQuery(candidate).queryItemValue("id") == QUrlQuery(original).queryItemValue("id"))
                queue.append(candidate.toString());
        }
    }
    return {};
}
