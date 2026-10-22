#pragma once

#include <QObject>
#include <QStringList>
#include <QUrl>
#include <functional>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTemporaryDir;
class QLockFile;

// Проверка и установка разделены: сеть используется по кнопке проверки,
// исполняемый файл заменяется только по отдельной кнопке установки.
class DriverUpdater final : public QObject {
    Q_OBJECT

public:
    explicit DriverUpdater(QObject *parent = nullptr);
    ~DriverUpdater() override;

    static QString localDriverPath();
    bool isBusy() const { return m_busy; }
    bool canInstall() const { return !m_downloadUrl.isEmpty() && !m_candidateVersion.isEmpty(); }
    bool isKnownIncompatible(const QString &driverPath) const;
    QString status() const { return m_status; }
    QString chromeVersion() const { return m_chromeVersion; }
    QString driverVersion() const { return m_driverVersion; }
    QString candidateVersion() const { return m_candidateVersion; }
    QString checkedPath() const { return m_checkedPath; }

    void check(const QString &driverPath);
    void install();
    void cancel();
    void clear();

signals:
    void changed();
    void logMessage(const QString &message, bool error);
    void installed(const QString &path);

private:
    using CommandResult = std::function<void(bool, const QByteArray &, const QString &)>;
    using DownloadResult = std::function<void(const QByteArray &)>;
    void runCommand(const QString &program, const QStringList &arguments, int timeoutMs,
                    CommandResult done, const QString &outputFile = {});
    void readChromeVersion(std::function<void(const QString &)> done);
    void fetch(const QUrl &url, qint64 limit, DownloadResult done, bool archive = false);
    void requestRelease(bool milestone = false);
    void downloadDriver();
    void extractDriver();
    void commitDriver();
    void setStatus(const QString &message);
    void finish(const QString &message, bool error = false);
    void releaseFiles();

    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_reply = nullptr;
    QProcess *m_process = nullptr;
    std::unique_ptr<QTemporaryDir> m_temporary;
    std::unique_ptr<QLockFile> m_installLock;
    quint64 m_generation = 0;
    bool m_busy = false;
    QString m_status;
    QString m_checkedPath;
    QString m_chromeVersion;
    QString m_driverVersion;
    QString m_candidateVersion;
    QString m_candidateChromeVersion;
    QString m_platform;
    QUrl m_downloadUrl;
};
