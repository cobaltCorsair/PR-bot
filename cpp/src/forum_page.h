#pragma once

#include <QString>
#include <QStringList>
#include <QMap>
#include <functional>
#include <stdexcept>

class WebDriverClient;
class QJsonObject;

// Один форум учитывается один раз в каждом сообщении, даже если в шаблоне
// несколько ссылок на него. Одного-двух сообщений недостаточно для вывода.
struct ForumLinkAnalysis {
    static constexpr int MinLinkedPosts = 3;
    int scannedPosts = 0;
    int linkedPosts = 0;
    QMap<QString, int> postsByForum;
    bool singleForumOnly() const
    {
        return linkedPosts >= MinLinkedPosts && postsByForum.size() == 1;
    }
};

class PublicationError final : public std::runtime_error {
public:
    enum class Status { Rejected, Unknown };
    PublicationError(Status status, const QString &message);
    Status status() const { return m_status; }
private:
    Status m_status;
};

// Проверка публикации для разметки MyBB/RusFF. До клика запоминаем сообщения,
// после — ищем новый ID, того же автора и признаки отправленного содержимого.
class ForumPage final {
public:
    ForumPage(WebDriverClient &driver, std::function<void()> checkpoint = {},
              std::function<void(const QString &)> notice = {});
    void checkReady(); // Только чтение: аккаунт, форма и кнопка отправки.
    ForumLinkAnalysis analyseForumLinks(const QStringList &localForums);
    QString publish(const QString &message, const QString &requiredBacklink = {},
                    const std::function<void()> &beforeSubmit = {}, int timeoutMs = 15000,
                    const std::function<void(const QJsonObject &)> &prepared = {});
    // Только чтение: сверка после потери связи, без повторного клика.
    QString findPublished(const QJsonObject &proof, bool requireNew = true);
private:
    QJsonObject postingSnapshot();
    WebDriverClient &m_driver;
    std::function<void()> m_checkpoint;
    std::function<void(const QString &)> m_notice;
};
