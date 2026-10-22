#pragma once

#include "config.h"
#include "run_log.h"

#include <QDateTime>
#include <QObject>
#include <QStringList>
#include <memory>
#include <atomic>

class WebDriverClient;

// GUI меняет только атомарный флаг. Обращаться к QObject рабочего потока
// из окна или посылать queued-slot во время блокирующего run() не требуется.
struct RunControl {
    std::atomic_bool stopRequested{false};
};

// Сохраняем и частичный результат: первая отправка могла пройти, а вторая —
// завершиться ошибкой. Ссылки позволяют проверить это перед повторным запуском.
struct ForumRunResult {
    QString forumUrl;
    QString outcome;
    QString stage;
    QString detail;
    QString topicUrl;
    QString sourcePageUrl;
    QString targetPageUrl;
    qint64 elapsedSeconds = 0;
};

struct BotReport {
    QStringList successful;
    QStringList noElements;
    QStringList wrongTheme;
    QStringList accountErrors;
    QStringList timeoutErrors;
    QStringList alreadyPosted;
    QStringList cancelledForums;
    bool cancelled = false;
    bool checkOnly = false;
    QString criticalError;
    QDateTime startedAt;
    QDateTime finishedAt;
    int plannedForums = 0;
    qint64 elapsedSeconds = 0;
    QList<ForumRunResult> forums;

    int skippedCount() const;
    QString toText(int elapsedMinutes) const;
};

// Весь браузерный сценарий выполняется в рабочем потоке; окно получает только
// сигналы. Две вкладки сохраняют исходную тему и текущий целевой форум.
class PrBot final : public QObject {
    Q_OBJECT

public:
    PrBot(BotConfig config, QString reportPath, std::shared_ptr<RunControl> control = {});

public slots:
    void run();

signals:
    void progressChanged(int value);
    void logMessage(const QString &message);
    void finished();
    void reportWritten(const QString &path);
    void runCompleted(bool hasErrors, bool cancelled);
    void activityChanged(const QString &forumUrl, const QString &stage);
    void statisticsChanged(int processed, int successful, int skipped, int failed);

private:
    QStringList readForums() const;
    QString normalizeUrl(const QString &input) const;
    QString ancestorForumUrl() const;
    void enterForum(WebDriverClient &driver);
    void visitForum(WebDriverClient &driver, const QString &forumUrl,
                    const QString &ancestorUrl);
    bool firstEnter(WebDriverClient &driver);
    bool openAccountTopic(WebDriverClient &driver);
    bool getPrMessages(WebDriverClient &driver);
    bool checkImageAndFormAnswer(WebDriverClient &driver);
    void forcedLogin(WebDriverClient &driver);
    void forumLogout(WebDriverClient &driver);
    void setStage(const QString &stage, const QString &message);
    void log(const QString &message, LogLevel level = LogLevel::Info);
    void writeReport(int elapsedMinutes);
    void checkStop() const;
    void navigate(WebDriverClient &driver, const QString &url);
    void waitForTopic(WebDriverClient &driver);

    BotConfig m_config;
    QString m_reportPath;
    BotReport m_report;
    ForumRunResult m_currentResult;
    std::unique_ptr<RunLog> m_log;
    QString m_stage;
    bool m_logWriteWarningSent = false;
    std::shared_ptr<RunControl> m_control;
    bool m_finishingPublication = false;
    QString m_currentUrl;
    QString m_userId;
    QString m_ancestorUrl;
    QString m_ancestorTopic;
    QString m_actualSourceForum;
    QString m_parentWindow; // Здесь размещаем рекламу посещаемого форума.
    QString m_forumWindow;  // Здесь публикуем наш шаблон со ссылкой на взаимность.
};
