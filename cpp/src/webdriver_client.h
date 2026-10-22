#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <memory>
#include <functional>
#include <stdexcept>

class QNetworkAccessManager;

// Код ошибки отделён от её текста: локализация сообщения не меняет обработку.
class WebDriverError final : public std::runtime_error {
public:
    enum class Kind { Startup, Timeout, Connection, Protocol, Browser };
    WebDriverError(Kind kind, const QString &message, QString code = {});
    Kind kind() const { return m_kind; }
    QString code() const { return m_code; }
private:
    Kind m_kind;
    QString m_code;
};

// Минимальный синхронный клиент W3C WebDriver. Владение процессом и браузерной
// сессией объединено: деструктор закрывает их даже при исключении в сценарии.
// Используется только из рабочего потока PrBot, не из потока интерфейса.
class WebDriverClient {
public:
    explicit WebDriverClient(QString driverExecutable);
    ~WebDriverClient();

    void start();
    void quit();
    void navigate(const QString &url);
    QString currentUrl(int timeoutMs = 20000);
    QByteArray takeScreenshot(const QStringList &hiddenValues = {});
    QJsonValue execute(const QString &script, const QJsonArray &arguments = {});
    QStringList windowHandles();
    void switchToWindow(const QString &handle);
    void openTab();
    bool hasSession() const { return !m_sessionId.isEmpty(); }
    QString browserVersion() const;
    QString driverVersion() const;
    // Проверяем состояние страницы до срока, без обязательной полной паузы.
    // checkpoint позволяет остановить подготовку между браузерными командами.
    QJsonValue waitFor(const std::function<QJsonValue()> &probe,
                       const QString &description, int timeoutMs = 10000,
                       const std::function<void()> &checkpoint = {});

private:
    QJsonValue request(const QByteArray &method, const QString &path,
                       const QJsonObject &body = {}, int timeoutMs = 20000);
    QString sessionPath(const QString &suffix = {}) const;
    void waitForServer();

    QString m_driverExecutable;
    QString m_sessionId;
    QString m_browserVersion;
    QString m_driverVersion;
    QUrl m_serverUrl;
    quint16 m_port = 0;
    std::unique_ptr<QProcess> m_driverProcess;
    std::unique_ptr<QNetworkAccessManager> m_network;
};
