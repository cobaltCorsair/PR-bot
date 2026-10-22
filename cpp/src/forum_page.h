#pragma once

#include <QString>
#include <functional>
#include <stdexcept>

class WebDriverClient;
class QJsonObject;

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
    ForumPage(WebDriverClient &driver, std::function<void()> checkpoint = {});
    void checkReady(); // Только чтение: аккаунт, форма и кнопка отправки.
    QString publish(const QString &message, const QString &requiredBacklink = {},
                    const std::function<void()> &beforeSubmit = {}, int timeoutMs = 15000,
                    const std::function<void(const QJsonObject &)> &prepared = {});
    // Только чтение: сверка после потери связи, без повторного клика.
    QString findPublished(const QJsonObject &proof, bool requireNew = true);
private:
    QJsonObject postingSnapshot();
    WebDriverClient &m_driver;
    std::function<void()> m_checkpoint;
};
