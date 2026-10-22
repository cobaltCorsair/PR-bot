#pragma once

#include <QJsonObject>
#include <QStringList>

class WebDriverClient;

// Диагностика относится к конкретному архивному журналу. Она не управляет
// отправкой сообщений; ошибки её сохранения возвращаются как предупреждения.
class RunDiagnostics final {
public:
    explicit RunDiagnostics(QStringList hiddenValues);
    bool start(const QString &archiveLogPath, bool checkOnly);
    bool setVersions(const QString &browser, const QString &driver);
    bool recordFailure(WebDriverClient *driver, const QString &forum, const QString &stage,
                       const QString &message, const QString &publicationPhase);
    bool finish(bool cancelled, const QString &criticalError);
    QString screenshotError() const { return m_screenshotError; }
    QString errorString() const { return m_error; }

    static QString directoryForLog(const QString &archiveLogPath);
    static bool available(const QString &archiveLogPath);
    static QString latestLog(const QString &reportPath);
    static void exportZip(const QString &archiveLogPath, const QString &destination);

private:
    bool save();
    QString redact(const QString &text) const;
    QStringList m_hiddenValues;
    QString m_directory;
    QString m_error;
    QString m_screenshotError;
    QJsonObject m_metadata;
};
