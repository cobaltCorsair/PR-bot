#pragma once

#include "config.h"

#include <QMainWindow>
#include <QStringList>
#include <memory>

class QCheckBox;
class QLineEdit;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTextEdit;
class QThread;
class QCloseEvent;
class QResizeEvent;
class QBoxLayout;
class QStackedWidget;
class QComboBox;
class QAction;
class DriverUpdater;
struct RunControl;

// Окно отвечает за форму и отображение событий. Браузерный сценарий находится
// в PrBot; во время прохода параметры фиксируются в отдельном BotConfig.
class BotWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit BotWindow(QWidget *parent = nullptr);
    ~BotWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void browseForums();
    void browseChromeDriver();
    void refreshDriverControls();
    void startBot();
    void saveSettings();
    void loadSettings();
    void openReport();
    void setBusy(bool busy);
    void updateLaunchStatus();
    void stopBot();
    void appendLog(const QString &message);
    void rebuildLog();

private:
    void setStatus(const QString &text, const QString &state);
    BotConfig readForm() const;
    void populateForm(const BotConfig &config);
    QWidget *buildCampaign();
    QWidget *buildJournal();
    void showPage(int index);
    void refreshForumFile();
    QString resolvedDriverPath() const;
    QString launchIssue(const BotConfig &config) const;
    QStringList visibleLogEntries() const;
    void insertLogEntry(const QString &message);
    QString settingsPath() const;
    QString reportPath() const;

    QPlainTextEdit *m_prCode = nullptr;
    QLineEdit *m_threadUrl = nullptr;
    QLineEdit *m_forumsFile = nullptr;
    QLineEdit *m_driverPath = nullptr;
    QCheckBox *m_useAccount = nullptr;
    QLineEdit *m_login = nullptr;
    QLineEdit *m_password = nullptr;
    QCheckBox *m_checkLastPage = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_forumsStatus = nullptr;
    QLabel *m_driverStatus = nullptr;
    QLabel *m_driverVersions = nullptr;
    QLabel *m_statusDot = nullptr;
    QLabel *m_pageTitle = nullptr;
    QLabel *m_pageDescription = nullptr;
    QLabel *m_characterCount = nullptr;
    QLabel *m_forumFileName = nullptr;
    QLabel *m_progressLabel = nullptr;
    QLabel *m_currentStage = nullptr;
    QLabel *m_currentForum = nullptr;
    QLabel *m_logCount = nullptr;
    QLabel *m_metricValues[4] = {};
    QTextEdit *m_log = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_statistics = nullptr;
    QPushButton *m_stop = nullptr;
    QPushButton *m_load = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_campaignNav = nullptr;
    QPushButton *m_journalNav = nullptr;
    QPushButton *m_advancedToggle = nullptr;
    QPushButton *m_chooseDriver = nullptr;
    QPushButton *m_checkDriver = nullptr;
    QPushButton *m_installDriver = nullptr;
    QPushButton *m_cancelDriver = nullptr;
    DriverUpdater *m_driverUpdater = nullptr;
    QStackedWidget *m_pages = nullptr;
    QBoxLayout *m_campaignColumns = nullptr;
    QWidget *m_campaignForm = nullptr;
    QWidget *m_advanced = nullptr;
    QWidget *m_credentials = nullptr;
    QLineEdit *m_logSearch = nullptr;
    QComboBox *m_logFilter = nullptr;
    QCheckBox *m_followLog = nullptr;
    QAction *m_passwordVisibility = nullptr;
    QStringList m_logEntries;
    int m_forumCount = 0;
    int m_visibleLogCount = 0;
    bool m_forumsReadable = false;
    bool m_busy = false;
    QThread *m_workerThread = nullptr;
    std::shared_ptr<RunControl> m_control;
    bool m_closeWhenFinished = false;
    QString m_completionText;
    QString m_completionState;
};
