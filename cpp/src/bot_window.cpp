#include "bot_window.h"

#include "pr_bot.h"
#include "driver_updater.h"
#include "run_history_dialog.h"
#include "window_theme.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QBoxLayout>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QStylePainter>
#include <QStyleOptionButton>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QStringConverter>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <exception>
#include <utility>
#include <functional>

namespace {

QFrame *makeCard(const QString &title, const QString &description,
                 QVBoxLayout **bodyLayout, QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setProperty("role", "card");

    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(20, 18, 20, 20);
    layout->setSpacing(12);

    auto *heading = new QLabel(title, card);
    heading->setProperty("role", "sectionTitle");
    if (!title.isEmpty())
        layout->addWidget(heading);
    else
        heading->hide();

    if (!description.isEmpty()) {
        auto *hint = new QLabel(description, card);
        hint->setProperty("role", "hint");
        hint->setWordWrap(true);
        layout->addWidget(hint);
    }

    *bodyLayout = layout;
    return card;
}

QLabel *makeFieldLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setProperty("role", "fieldLabel");
    return label;
}

QLabel *makeHint(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setProperty("role", "hint");
    label->setWordWrap(true);
    return label;
}

class FileDropCard final : public QFrame {
public:
    explicit FileDropCard(QWidget *parent = nullptr) : QFrame(parent) { setAcceptDrops(true); }
    std::function<void(const QString &)> onFile;
protected:
    void dragEnterEvent(QDragEnterEvent *event) override {
        if (event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1
            && event->mimeData()->urls().first().isLocalFile())
            event->acceptProposedAction();
    }
    void dropEvent(QDropEvent *event) override {
        if (event->mimeData()->hasUrls() && !event->mimeData()->urls().isEmpty()
            && event->mimeData()->urls().first().isLocalFile() && onFile) {
            onFile(event->mimeData()->urls().first().toLocalFile());
            event->acceptProposedAction();
        }
    }
};

// Сохраняем клавиатуру и доступность QCheckBox, добавляя явную галочку
// поверх стилизованного индикатора: один цвет не должен обозначать выбор.
class CheckBox final : public QCheckBox {
public:
    using QCheckBox::QCheckBox;
protected:
    void paintEvent(QPaintEvent *) override {
        QStyleOptionButton option;
        initStyleOption(&option);
        QStylePainter painter(this);
        painter.drawControl(QStyle::CE_CheckBox, option);
        if (isChecked()) {
            const QRect rect = style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, this);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(isEnabled() ? QColor("#ffffff") : QColor("#89997e"), 1.8,
                                Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawLine(QPointF(rect.left() + 4, rect.center().y()),
                             QPointF(rect.left() + 7, rect.bottom() - 4));
            painter.drawLine(QPointF(rect.left() + 7, rect.bottom() - 4),
                             QPointF(rect.right() - 3, rect.top() + 4));
        }
    }
};

QString counted(qsizetype count, const QString &one, const QString &few, const QString &many)
{
    const int last = int(count % 10);
    const int lastTwo = int(count % 100);
    const QString word = lastTwo >= 11 && lastTwo <= 14 ? many
        : last == 1 ? one : last >= 2 && last <= 4 ? few : many;
    return QString::number(count) + ' ' + word;
}

bool matchesLogEntry(const QString &message, const QString &search, int filter)
{
    if (filter >= 1 && !message.contains("[ОШИБКА]") && !message.contains("[ВНИМАНИЕ]"))
        return false;
    if (filter == 2 && !message.contains("[ОШИБКА]"))
        return false;
    return search.isEmpty() || message.contains(search, Qt::CaseInsensitive);
}

QString applicationLog(const QString &message, const QString &level = "ИНФО")
{
    return QString("[%1] [%2] [Настройки] %3")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), level, message);
}

} // namespace

BotWindow::BotWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("PR-Bot — публикации");
    setMinimumSize(940, 680);
    resize(1220, 840);
    setStyleSheet(UiTheme::stylesheet());
    m_driverUpdater = new DriverUpdater(this);

    auto *root = new QWidget(this);
    root->setObjectName("workspace");
    auto *rootLayout = new QHBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto *sidebar = new QFrame(root);
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(190);
    auto *side = new QVBoxLayout(sidebar);
    side->setContentsMargins(18, 28, 18, 24);
    side->setSpacing(8);
    auto *identity = new QHBoxLayout;
    identity->setSpacing(10);
    auto *mark = new QLabel("PR", sidebar);
    mark->setObjectName("brandMark");
    mark->setAlignment(Qt::AlignCenter);
    mark->setFixedSize(40, 40);
    auto *brand = new QLabel("PR-Bot", sidebar);
    brand->setObjectName("brandName");
    identity->addWidget(mark);
    identity->addWidget(brand);
    side->addLayout(identity);
    side->addSpacing(32);
    auto *sideTitle = new QLabel("РАБОЧЕЕ ПРОСТРАНСТВО", sidebar);
    sideTitle->setObjectName("sideEyebrow");
    side->addWidget(sideTitle);
    side->addSpacing(5);
    m_campaignNav = new QPushButton("  Кампания", sidebar);
    m_journalNav = new QPushButton("  Журнал", sidebar);
    m_campaignNav->setObjectName("campaignNav");
    m_journalNav->setObjectName("journalNav");
    m_campaignNav->setIcon(UiTheme::icon(UiTheme::Icon::Campaign, "#d0dfc8"));
    m_journalNav->setIcon(UiTheme::icon(UiTheme::Icon::Journal, "#d0dfc8"));
    auto *navigation = new QButtonGroup(this);
    for (auto *button : {m_campaignNav, m_journalNav}) {
        button->setProperty("role", "nav");
        button->setCheckable(true);
        button->setMinimumHeight(46);
        navigation->addButton(button);
        side->addWidget(button);
    }
    side->addStretch();
    auto *sideNote = new QLabel("Взаимная реклама.\nВсё под контролем.", sidebar);
    sideNote->setObjectName("sideNote");
    sideNote->setWordWrap(true);
    side->addWidget(sideNote);
    side->addSpacing(14);
    auto *platform = new QLabel("MYBB  /  RUSFF", sidebar);
    platform->setObjectName("sidePlatform");
    side->addWidget(platform);
    rootLayout->addWidget(sidebar);

    auto *main = new QWidget(root);
    auto *mainLayout = new QVBoxLayout(main);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    auto *header = new QWidget(main);
    auto *headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(32, 27, 32, 19);
    auto *headings = new QVBoxLayout;
    headings->setSpacing(5);
    auto *eyebrow = new QLabel("ПУБЛИКАЦИИ", header);
    eyebrow->setObjectName("eyebrow");
    m_pageTitle = new QLabel(header);
    m_pageTitle->setObjectName("pageTitle");
    m_pageDescription = new QLabel(header);
    m_pageDescription->setObjectName("pageDescription");
    m_pageDescription->setWordWrap(true);
    headings->addWidget(eyebrow);
    headings->addWidget(m_pageTitle);
    headings->addWidget(m_pageDescription);
    headerRow->addLayout(headings, 1);
    m_load = new QPushButton("Загрузить", header);
    m_load->setProperty("role", "quiet");
    m_load->setIcon(UiTheme::icon(UiTheme::Icon::Load));
    m_load->setToolTip("Загрузить сохранённые настройки");
    m_save = new QPushButton("Сохранить", header);
    m_save->setIcon(UiTheme::icon(UiTheme::Icon::Save));
    m_save->setToolTip("Сохранить настройки · ⌘S / Ctrl+S");
    headerRow->addWidget(m_load);
    headerRow->addWidget(m_save);
    mainLayout->addWidget(header);

    m_pages = new QStackedWidget(main);
    m_pages->addWidget(buildCampaign());
    m_pages->addWidget(buildJournal());
    mainLayout->addWidget(m_pages, 1);

    // Управление проходом доступно на обоих экранах и не уезжает при прокрутке.
    auto *footer = new QFrame(main);
    footer->setObjectName("footer");
    auto *footerLayout = new QVBoxLayout(footer);
    footerLayout->setContentsMargins(0, 0, 0, 0);
    footerLayout->setSpacing(0);
    m_progress = new QProgressBar(footer);
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);
    m_progress->setValue(0);
    footerLayout->addWidget(m_progress);
    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(30, 17, 30, 17);
    actions->setSpacing(13);
    m_statusDot = new QLabel(footer);
    m_statusDot->setObjectName("statusDot");
    actions->addWidget(m_statusDot);
    auto *statusText = new QVBoxLayout;
    statusText->setSpacing(3);
    m_status = new QLabel(footer);
    m_status->setObjectName("status");
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_status->setMinimumWidth(150);
    m_progressLabel = new QLabel("Настройте кампанию перед запуском", footer);
    m_progressLabel->setObjectName("progressLabel");
    statusText->addWidget(m_status);
    statusText->addWidget(m_progressLabel);
    actions->addLayout(statusText, 1);
    m_stop = new QPushButton("Остановить", footer);
    m_stop->setObjectName("stopButton");
    m_stop->setIcon(UiTheme::icon(UiTheme::Icon::Stop, "#935738"));
    m_stop->hide();
    m_checkForums = new QPushButton("Проверить", footer);
    m_checkForums->setObjectName("checkForumsButton");
    m_checkForums->setIcon(UiTheme::icon(UiTheme::Icon::Search));
    m_checkForums->setToolTip("Войти в PR-аккаунты и проверить темы без отправки рекламных сообщений");
    m_start = new QPushButton("Запустить обход", footer);
    m_start->setObjectName("primaryButton");
    m_start->setIcon(UiTheme::icon(UiTheme::Icon::Play, "#ffffff"));
    m_start->setMinimumWidth(174);
    m_start->setToolTip("Начать публикации · ⌘Enter / Ctrl+Enter");
    actions->addWidget(m_stop);
    actions->addWidget(m_checkForums);
    actions->addWidget(m_start);
    footerLayout->addLayout(actions);
    mainLayout->addWidget(footer);
    rootLayout->addWidget(main, 1);
    setCentralWidget(root);

    connect(m_campaignNav, &QPushButton::clicked, this, [this] { showPage(0); });
    connect(m_journalNav, &QPushButton::clicked, this, [this] { showPage(1); });
    connect(m_start, &QPushButton::clicked, this, &BotWindow::startBot);
    connect(m_checkForums, &QPushButton::clicked, this, &BotWindow::checkForums);
    connect(m_stop, &QPushButton::clicked, this, &BotWindow::stopBot);
    connect(m_save, &QPushButton::clicked, this, &BotWindow::saveSettings);
    connect(m_load, &QPushButton::clicked, this, &BotWindow::loadSettings);
    connect(m_statistics, &QPushButton::clicked, this, &BotWindow::openReport);
    connect(m_prCode, &QPlainTextEdit::textChanged, this, [this] {
        m_characterCount->setText(counted(m_prCode->toPlainText().size(), "символ", "символа", "символов"));
        updateLaunchStatus();
    });
    connect(m_forumsFile, &QLineEdit::textChanged, this, [this] {
        refreshForumFile();
        updateLaunchStatus();
    });
    for (auto *field : {m_threadUrl, m_login, m_password})
        connect(field, &QLineEdit::textChanged, this, &BotWindow::updateLaunchStatus);
    connect(m_driverPath, &QLineEdit::textChanged, this, [this] {
        // Результат проверки относится к конкретному выбранному файлу.
        if (m_driverUpdater->checkedPath() != resolvedDriverPath())
            m_driverUpdater->clear();
        updateLaunchStatus();
    });
    connect(m_driverUpdater, &DriverUpdater::changed, this, &BotWindow::updateLaunchStatus);
    connect(m_driverUpdater, &DriverUpdater::logMessage, this, [this](const QString &message, bool error) {
        appendLog(QString("[%1] [%2] [ChromeDriver] %3")
            .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
                 error ? "ОШИБКА" : "ИНФО", message));
    });
    connect(m_driverUpdater, &DriverUpdater::installed, this, [this](const QString &path) {
        // Установка всегда выбирает локальную копию, даже если ранее был
        // указан старый драйвер из другого каталога. Остальная форма сохранится.
        const QSignalBlocker blocker(m_driverPath);
        m_driverPath->setText(path);
        updateLaunchStatus();
    });
    connect(m_useAccount, &QCheckBox::toggled, this, [this](bool enabled) {
        m_credentials->setVisible(enabled);
        if (enabled)
            QTimer::singleShot(0, this, [this] {
                if (m_advanced->isVisible())
                    findChild<QScrollArea *>("campaignScroll")->ensureWidgetVisible(m_password, 0, 22);
            });
        updateLaunchStatus();
    });
    connect(m_checkLastPage, &QCheckBox::toggled, this, &BotWindow::updateLaunchStatus);

    auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this, [this] { if (!m_busy) saveSettings(); });
    auto *openShortcut = new QShortcut(QKeySequence::Open, this);
    connect(openShortcut, &QShortcut::activated, this, [this] { if (!m_busy) browseForums(); });
    auto *startShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    connect(startShortcut, &QShortcut::activated, this, [this] {
        if (m_start->isEnabled()) startBot();
    });
    auto *journalShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_L), this);
    connect(journalShortcut, &QShortcut::activated, this, [this] {
        showPage(1);
        m_logSearch->setFocus();
    });
    QWidget::setTabOrder(m_prCode, m_threadUrl);
    QWidget::setTabOrder(m_threadUrl, m_advancedToggle);
    QWidget::setTabOrder(m_advancedToggle, m_start);
    showPage(0);
    try {
        populateForm(BotConfig::load(settingsPath()));
    } catch (const std::exception &error) {
        setStatus("Не удалось загрузить настройки", "attention");
        appendLog(applicationLog(QString("Не удалось загрузить настройки: %1")
            .arg(QString::fromUtf8(error.what())), "ОШИБКА"));
    }
}

QWidget *BotWindow::buildCampaign()
{
    auto *scroll = new QScrollArea;
    scroll->setObjectName("campaignScroll");
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    m_campaignForm = new QWidget;
    auto *layout = new QVBoxLayout(m_campaignForm);
    layout->setContentsMargins(32, 5, 32, 20);
    layout->setSpacing(10);
    m_campaignColumns = new QBoxLayout(QBoxLayout::LeftToRight);
    m_campaignColumns->setSpacing(20);

    auto *editor = new QFrame(m_campaignForm);
    editor->setProperty("role", "card");
    editor->setObjectName("editorCard");
    auto *editorLayout = new QVBoxLayout(editor);
    editorLayout->setContentsMargins(22, 20, 22, 20);
    editorLayout->setSpacing(12);
    auto *editorHeading = new QHBoxLayout;
    auto *editorTitle = new QLabel("Ваше объявление", editor);
    editorTitle->setProperty("role", "sectionTitle");
    m_characterCount = new QLabel("0 символов", editor);
    m_characterCount->setObjectName("characterCount");
    editorHeading->addWidget(editorTitle);
    editorHeading->addStretch();
    editorHeading->addWidget(m_characterCount);
    editorLayout->addLayout(editorHeading);
    m_prCode = new QPlainTextEdit(editor);
    m_prCode->setObjectName("advertisement");
    m_prCode->setAccessibleName("Текст рекламы");
    m_prCode->setPlaceholderText("Вставьте BBCode вашего объявления…\n\nБот добавит ссылку на взаимную рекламу автоматически.");
    m_prCode->setMinimumHeight(230);
    m_prCode->setTabStopDistance(QFontMetricsF(m_prCode->font()).horizontalAdvance(' ') * 4);
    editorLayout->addWidget(m_prCode, 1);
    auto *formatRow = new QHBoxLayout;
    auto *format = new QLabel("BBCODE", editor);
    format->setObjectName("formatBadge");
    formatRow->addWidget(format);
    formatRow->addWidget(makeHint("Ссылки и изображения сохранятся в публикации", editor), 1);
    editorLayout->addLayout(formatRow);
    auto *rule = new QFrame(editor);
    rule->setObjectName("rule");
    rule->setFixedHeight(1);
    editorLayout->addWidget(rule);
    auto *topicLabel = makeFieldLabel("Тема взаимной рекламы", editor);
    editorLayout->addWidget(topicLabel);
    auto *topicRow = new QHBoxLayout;
    topicRow->setSpacing(7);
    m_threadUrl = new QLineEdit(editor);
    m_threadUrl->setObjectName("threadUrl");
    m_threadUrl->setPlaceholderText("https://ваш-форум/viewtopic.php?id=…");
    m_threadUrl->setAccessibleName("Ссылка на тему взаимной рекламы");
    topicLabel->setBuddy(m_threadUrl);
    auto *openTopic = new QPushButton(editor);
    openTopic->setIcon(UiTheme::icon(UiTheme::Icon::Arrow));
    openTopic->setToolTip("Открыть тему в браузере");
    openTopic->setAccessibleName("Открыть тему в браузере");
    topicRow->addWidget(m_threadUrl, 1);
    topicRow->addWidget(openTopic);
    connect(openTopic, &QPushButton::clicked, this, [this] {
        const QUrl url(m_threadUrl->text().trimmed());
        if (url.isValid() && (url.scheme() == "http" || url.scheme() == "https") && !url.host().isEmpty())
            QDesktopServices::openUrl(url);
    });
    editorLayout->addLayout(topicRow);
    editorLayout->addWidget(makeHint("Здесь бот разместит рекламу выбранных форумов.", editor));
    m_campaignColumns->addWidget(editor, 3);

    auto *targets = new FileDropCard(m_campaignForm);
    targets->setObjectName("targetsCard");
    targets->onFile = [this](const QString &path) { if (!m_busy) m_forumsFile->setText(path); };
    auto *targetsLayout = new QVBoxLayout(targets);
    targetsLayout->setContentsMargins(22, 22, 22, 20);
    targetsLayout->setSpacing(13);
    auto *targetsTitle = new QLabel("Куда публикуем", targets);
    targetsTitle->setProperty("role", "sectionTitle");
    targetsLayout->addWidget(targetsTitle);
    targetsLayout->addWidget(makeHint("Один адрес форума на строку в текстовом файле.", targets));
    auto *choose = new QPushButton("Выбрать список", targets);
    choose->setObjectName("chooseForums");
    choose->setIcon(UiTheme::icon(UiTheme::Icon::Folder));
    choose->setToolTip("Открыть файл со списком форумов · ⌘O / Ctrl+O");
    connect(choose, &QPushButton::clicked, this, &BotWindow::browseForums);
    targetsLayout->addWidget(choose);
    auto *dropHint = makeHint("или перетащите файл сюда", targets);
    dropHint->setAlignment(Qt::AlignCenter);
    targetsLayout->addWidget(dropHint);
    m_forumsFile = new QLineEdit(targets);
    m_forumsFile->setObjectName("forumsFile");
    m_forumsFile->hide(); // Полный путь хранится отдельно от короткого имени в карточке.
    m_forumFileName = new QLabel("Файл не выбран", targets);
    m_forumFileName->setObjectName("forumFileName");
    m_forumFileName->setTextFormat(Qt::PlainText);
    m_forumFileName->setWordWrap(true);
    m_forumFileName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    targetsLayout->addWidget(m_forumFileName);
    m_forumsStatus = makeHint("Выберите список для обхода", targets);
    targetsLayout->addWidget(m_forumsStatus);
    targetsLayout->addStretch();
    auto *targetsRule = new QFrame(targets);
    targetsRule->setObjectName("rule");
    targetsRule->setFixedHeight(1);
    targetsLayout->addWidget(targetsRule);
    m_checkLastPage = new CheckBox("Проверять повторы", targets);
    m_checkLastPage->setObjectName("checkLastPage");
    m_checkLastPage->setChecked(true);
    targetsLayout->addWidget(m_checkLastPage);
    targetsLayout->addWidget(makeHint("Пропускать форум, если наша реклама уже есть на последней странице.", targets));
    m_campaignColumns->addWidget(targets, 2);
    layout->addLayout(m_campaignColumns);

    m_advancedToggle = new QPushButton("Параметры входа и браузера  ▸", m_campaignForm);
    m_advancedToggle->setObjectName("advancedToggle");
    m_advancedToggle->setCheckable(true);
    layout->addWidget(m_advancedToggle);
    QVBoxLayout *advancedLayout = nullptr;
    m_advanced = makeCard({}, {}, &advancedLayout, m_campaignForm);
    m_advanced->setObjectName("advancedSettings");
    auto *advancedRow = new QHBoxLayout;
    advancedRow->setSpacing(28);
    auto *account = new QVBoxLayout;
    m_useAccount = new CheckBox("Указать логин и пароль", m_advanced);
    m_useAccount->setObjectName("useAccount");
    account->addWidget(m_useAccount);
    account->addWidget(makeHint("По умолчанию используется PR-вход форума.", m_advanced));
    m_credentials = new QWidget(m_advanced);
    m_credentials->setObjectName("credentials");
    auto *credentials = new QGridLayout(m_credentials);
    credentials->setContentsMargins(0, 8, 0, 0);
    credentials->setHorizontalSpacing(9);
    auto *loginLabel = makeFieldLabel("Логин", m_credentials);
    auto *passwordLabel = makeFieldLabel("Пароль", m_credentials);
    m_login = new QLineEdit(m_credentials);
    m_login->setObjectName("login");
    m_login->setPlaceholderText("Логин аккаунта");
    m_password = new QLineEdit(m_credentials);
    m_password->setObjectName("password");
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText("На этот запуск");
    loginLabel->setBuddy(m_login);
    passwordLabel->setBuddy(m_password);
    m_passwordVisibility = m_password->addAction(UiTheme::icon(UiTheme::Icon::Eye), QLineEdit::TrailingPosition);
    m_passwordVisibility->setCheckable(true);
    m_passwordVisibility->setToolTip("Показать пароль");
    connect(m_passwordVisibility, &QAction::toggled, this, [this](bool show) {
        m_password->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
        m_passwordVisibility->setToolTip(show ? "Скрыть пароль" : "Показать пароль");
    });
    credentials->addWidget(loginLabel, 0, 0);
    credentials->addWidget(passwordLabel, 0, 1);
    credentials->addWidget(m_login, 1, 0);
    credentials->addWidget(m_password, 1, 1);
    account->addWidget(m_credentials);
    account->addWidget(makeHint("Пароль не сохраняется на диск.", m_advanced));
    account->addStretch();
    auto *driver = new QVBoxLayout;
    auto *driverLabel = makeFieldLabel("ChromeDriver", m_advanced);
    driver->addWidget(driverLabel);
    auto *driverRow = new QHBoxLayout;
    m_driverPath = new QLineEdit(m_advanced);
    m_driverPath->setObjectName("driverPath");
    m_driverPath->setPlaceholderText("Автоматический выбор");
    driverLabel->setBuddy(m_driverPath);
    m_chooseDriver = new QPushButton(m_advanced);
    m_chooseDriver->setIcon(UiTheme::icon(UiTheme::Icon::Folder));
    m_chooseDriver->setToolTip("Выбрать исполняемый файл ChromeDriver");
    m_chooseDriver->setAccessibleName("Выбрать ChromeDriver");
    connect(m_chooseDriver, &QPushButton::clicked, this, &BotWindow::browseChromeDriver);
    driverRow->addWidget(m_driverPath, 1);
    driverRow->addWidget(m_chooseDriver);
    driver->addLayout(driverRow);
    m_driverVersions = makeHint({}, m_advanced);
    m_driverVersions->setObjectName("driverVersions");
    m_driverVersions->setTextFormat(Qt::PlainText);
    driver->addWidget(m_driverVersions);
    m_driverStatus = makeHint({}, m_advanced);
    m_driverStatus->setObjectName("driverStatus");
    m_driverStatus->setTextFormat(Qt::PlainText);
    driver->addWidget(m_driverStatus);
    auto *driverActions = new QHBoxLayout;
    driverActions->setSpacing(6);
    m_checkDriver = new QPushButton("Проверить обновления", m_advanced);
    m_checkDriver->setObjectName("checkDriverUpdate");
    m_checkDriver->setToolTip("Определить версии Chrome и драйвера и найти совместимое обновление");
    m_installDriver = new QPushButton("Установить", m_advanced);
    m_installDriver->setObjectName("installDriverUpdate");
    m_installDriver->setIcon(UiTheme::icon(UiTheme::Icon::Load));
    m_installDriver->setToolTip("Скачать проверенный драйвер в окружение проекта и выбрать его для кампании");
    m_installDriver->hide();
    m_cancelDriver = new QPushButton("Отмена", m_advanced);
    m_cancelDriver->setObjectName("cancelDriverUpdate");
    m_cancelDriver->hide();
    driverActions->addWidget(m_checkDriver);
    driverActions->addWidget(m_installDriver);
    driverActions->addWidget(m_cancelDriver);
    driverActions->addStretch();
    driver->addLayout(driverActions);
    driver->addWidget(makeHint("Проверяйте после обновления Chrome; установка — по кнопке.", m_advanced));
    connect(m_checkDriver, &QPushButton::clicked, this, [this] {
        if (!m_busy) m_driverUpdater->check(resolvedDriverPath());
    });
    connect(m_installDriver, &QPushButton::clicked, this, [this] {
        if (!m_busy) m_driverUpdater->install();
    });
    connect(m_cancelDriver, &QPushButton::clicked, m_driverUpdater, &DriverUpdater::cancel);
    driver->addStretch();
    advancedRow->addLayout(account, 1);
    advancedRow->addLayout(driver, 1);
    advancedLayout->addLayout(advancedRow);
    layout->addWidget(m_advanced);
    m_advanced->hide();
    connect(m_advancedToggle, &QPushButton::toggled, this, [this](bool expanded) {
        m_advanced->setVisible(expanded);
        m_advancedToggle->setText(expanded ? "Параметры входа и браузера  ▾" : "Параметры входа и браузера  ▸");
        if (expanded)
            QTimer::singleShot(0, this, [this] {
                findChild<QScrollArea *>("campaignScroll")->ensureWidgetVisible(m_advanced, 0, 16);
            });
    });
    layout->addStretch();
    scroll->setWidget(m_campaignForm);
    return scroll;
}

QWidget *BotWindow::buildJournal()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(32, 2, 32, 22);
    layout->setSpacing(17);
    m_runMode = makeFieldLabel("Результаты последнего запуска", page);
    m_runMode->setObjectName("runMode");
    layout->addWidget(m_runMode);
    auto *metrics = new QFrame(page);
    metrics->setObjectName("metrics");
    auto *metricsRow = new QHBoxLayout(metrics);
    metricsRow->setContentsMargins(0, 0, 0, 8);
    metricsRow->setSpacing(22);
    const QStringList labels = {"Обработано", "Успешно", "Пропущено", "Ошибки"};
    for (int index = 0; index < 4; ++index) {
        auto *column = new QVBoxLayout;
        column->setSpacing(1);
        m_metricValues[index] = new QLabel("0", metrics);
        m_metricValues[index]->setObjectName("metric" + QString::number(index));
        m_metricValues[index]->setProperty("role", "metricValue");
        if (index == 1 || index == 3)
            m_metricValues[index]->setProperty("tone", index == 1 ? "success" : "error");
        auto *label = new QLabel(labels.at(index), metrics);
        if (index == 1) m_successMetricLabel = label;
        label->setProperty("role", "metricLabel");
        column->addWidget(m_metricValues[index]);
        column->addWidget(label);
        metricsRow->addLayout(column, 1);
    }
    layout->addWidget(metrics);
    m_currentStage = new QLabel("Обход ещё не запущен", page);
    m_currentStage->setObjectName("currentStage");
    m_currentForum = new QLabel("После запуска здесь появится текущий форум и этап работы.", page);
    m_currentForum->setObjectName("currentForum");
    m_currentForum->setTextFormat(Qt::PlainText);
    m_currentForum->setWordWrap(true);
    layout->addWidget(m_currentStage);
    layout->addWidget(m_currentForum);
    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    m_logSearch = new QLineEdit(page);
    m_logSearch->setObjectName("logSearch");
    m_logSearch->setPlaceholderText("Поиск по журналу");
    m_logSearch->setClearButtonEnabled(true);
    m_logSearch->setAccessibleName("Поиск по журналу");
    m_logSearch->addAction(UiTheme::icon(UiTheme::Icon::Search), QLineEdit::LeadingPosition);
    m_logFilter = new QComboBox(page);
    m_logFilter->setObjectName("logFilter");
    m_logFilter->addItems({"Все события", "Предупреждения и ошибки", "Только ошибки"});
    m_logFilter->setAccessibleName("Фильтр событий журнала");
    auto *copy = new QPushButton(page);
    copy->setObjectName("copyLog");
    copy->setIcon(UiTheme::icon(UiTheme::Icon::Copy));
    copy->setToolTip("Копировать показанные события");
    copy->setAccessibleName("Копировать журнал");
    m_statistics = new QPushButton("Отчёт", page);
    m_statistics->setIcon(UiTheme::icon(UiTheme::Icon::Arrow));
    m_statistics->setToolTip("Открыть полный журнал и итоги прохода");
    m_statistics->setEnabled(QFileInfo::exists(reportPath()));
    auto *history = new QPushButton("История", page);
    history->setObjectName("openHistory");
    history->setIcon(UiTheme::icon(UiTheme::Icon::Folder));
    history->setToolTip("Открыть журналы прошлых проверок и публикаций");
    connect(history, &QPushButton::clicked, this, &BotWindow::openHistory);
    toolbar->addWidget(m_logSearch, 1);
    toolbar->addWidget(m_logFilter);
    toolbar->addWidget(copy);
    toolbar->addWidget(m_statistics);
    toolbar->addWidget(history);
    layout->addLayout(toolbar);
    m_log = new QTextEdit(page);
    m_log->setObjectName("log");
    m_log->setReadOnly(true);
    m_log->setPlaceholderText("Журнал пока пуст.\n\nПодготовьте кампанию и запустите обход — здесь появятся события и результаты.");
    m_log->document()->setMaximumBlockCount(15000);
    layout->addWidget(m_log, 1);
    auto *journalFooter = new QHBoxLayout;
    m_logCount = makeHint("0 событий", page);
    m_followLog = new CheckBox("Автопрокрутка", page);
    m_followLog->setChecked(true);
    journalFooter->addWidget(m_logCount);
    journalFooter->addStretch();
    journalFooter->addWidget(m_followLog);
    layout->addLayout(journalFooter);
    connect(m_logSearch, &QLineEdit::textChanged, this, &BotWindow::rebuildLog);
    connect(m_logFilter, &QComboBox::currentIndexChanged, this, &BotWindow::rebuildLog);
    connect(copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(visibleLogEntries().join('\n'));
    });
    connect(m_followLog, &QCheckBox::toggled, this, [this](bool follow) {
        if (follow) m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
    });
    return page;
}

BotWindow::~BotWindow()
{
    // Останавливаем дочерние процессы/загрузку до разрушения виджетов и их
    // соединений с changed(). Старый драйвер при отмене остаётся на месте.
    delete m_driverUpdater;
    m_driverUpdater = nullptr;
    // closeEvent обычно откладывает закрытие до finished. Это страховка для
    // программного удаления окна/завершения приложения без closeEvent.
    if (m_workerThread) {
        m_control->stopRequested.store(true);
        m_workerThread->quit();
        m_workerThread->wait();
    }
}

void BotWindow::closeEvent(QCloseEvent *event)
{
    if (m_workerThread) {
        m_closeWhenFinished = true;
        stopBot();
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

void BotWindow::stopBot()
{
    if (!m_control || !m_workerThread)
        return;
    m_control->stopRequested.store(true);
    m_stop->setEnabled(false);
    m_stop->setText("Завершаю…");
    setStatus(m_runningCheck ? "Останавливаю проверку форумов…"
                             : "Останавливаю обход. Начатая пара публикаций будет завершена…", "running");
}

void BotWindow::browseForums()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Выберите список форумов", {}, "Текстовые файлы (*.txt);;Все файлы (*)");
    if (!path.isEmpty())
        m_forumsFile->setText(path);
}

void BotWindow::browseChromeDriver()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Выберите ChromeDriver", {}, "ChromeDriver (chromedriver.exe chromedriver);;Все файлы (*)");
    if (!path.isEmpty())
        m_driverPath->setText(path);
}

void BotWindow::startBot()
{
    runBot(false);
}

void BotWindow::checkForums()
{
    runBot(true);
}

void BotWindow::runBot(bool checkOnly)
{
    if (m_workerThread || m_driverUpdater->isBusy())
        return;
    refreshForumFile();
    BotConfig config = readForm();
    config.checkOnly = checkOnly;
    if (!launchIssue(config).isEmpty()) {
        updateLaunchStatus();
        return;
    }
    config.chromeDriverPath = resolvedDriverPath();
    m_runningCheck = checkOnly;
    m_runMode->setText(checkOnly ? "ПРОВЕРКА БЕЗ ПУБЛИКАЦИИ" : "ПУБЛИКАЦИЯ");
    m_successMetricLabel->setText(checkOnly ? "Готовы" : "Успешно");

    m_logEntries.clear();
    m_visibleLogCount = 0;
    m_log->clear();
    m_logSearch->clear();
    m_logFilter->setCurrentIndex(0);
    m_logCount->setText("0 событий");
    m_progress->setValue(0);
    m_statistics->setEnabled(false);
    setBusy(true);
    showPage(1);

    // Сетевые ожидания и браузерные команды блокируют только рабочий поток.
    // Qt доставляет сигналы обратно в GUI через очередь между потоками.
    m_workerThread = new QThread(this);
    m_control = std::make_shared<RunControl>();
    m_completionText = checkOnly ? "Проверка завершена. Рекламные сообщения не отправлялись."
                                : "Обработка завершена. Результат — в журнале и отчёте.";
    m_completionState = "done";
    auto *worker = new PrBot(config, reportPath(), m_control);
    worker->moveToThread(m_workerThread);
    connect(m_workerThread, &QThread::started, worker, &PrBot::run);
    connect(worker, &PrBot::progressChanged, this, [this](int value) {
        m_progress->setValue(value);
        m_progressLabel->setText(QString("Обработано %1% списка").arg(value));
    });
    connect(worker, &PrBot::activityChanged, this, [this](const QString &forum, const QString &stage) {
        m_currentStage->setText(stage);
        QUrl url(forum);
        url.setUserInfo({});
        m_currentForum->setText(forum.isEmpty() ? "Общий этап прохода" : url.toDisplayString());
    });
    connect(worker, &PrBot::statisticsChanged, this, [this](int processed, int successful, int skipped, int failed) {
        const int values[] = {processed, successful, skipped, failed};
        for (int index = 0; index < 4; ++index)
            m_metricValues[index]->setText(QString::number(values[index]));
    });
    connect(worker, &PrBot::logMessage, this, &BotWindow::appendLog);
    connect(worker, &PrBot::reportWritten, this, [this](const QString &) {
        m_statistics->setEnabled(true);
    });
    // Сначала завершается работа, затем поток и принадлежащий ему worker.
    connect(worker, &PrBot::runCompleted, this, [this, checkOnly](bool hasErrors, bool cancelled) {
        if (checkOnly) {
            m_completionText = hasErrors ? "Проверка завершена с ошибкой. Рекламные сообщения не отправлялись."
                : cancelled ? "Проверка остановлена. Рекламные сообщения не отправлялись."
                            : "Проверка завершена. Рекламные сообщения не отправлялись.";
        } else {
            m_completionText = hasErrors ? "Обход завершён с ошибкой. Подробности — в журнале."
                : cancelled ? "Обход остановлен. Результат сохранён в отчёте."
                            : "Обработка завершена. Результат — в журнале и отчёте.";
        }
        m_completionState = hasErrors || cancelled ? "attention" : "done";
    });
    // quit() потокобезопасен. DirectConnection не требует работающего GUI-loop
    // при аварийном программном уничтожении окна и ожидании в деструкторе.
    connect(worker, &PrBot::finished, m_workerThread, &QThread::quit, Qt::DirectConnection);
    connect(m_workerThread, &QThread::finished, worker, &QObject::deleteLater);
    connect(m_workerThread, &QThread::finished, this, [this] {
        setBusy(false);
        m_workerThread->deleteLater();
        m_workerThread = nullptr;
        m_statistics->setEnabled(QFileInfo::exists(reportPath()));
        m_control.reset();
        setStatus(m_completionText, m_completionState);
        m_progressLabel->setText(QString("Обработано %1% списка · полный журнал доступен в отчёте")
                                    .arg(m_progress->value()));
        if (m_closeWhenFinished)
            QTimer::singleShot(0, this, &QWidget::close);
    });
    m_workerThread->start();
}

void BotWindow::saveSettings()
{
    if (m_busy || m_driverUpdater->isBusy())
        return;
    try {
        readForm().save(settingsPath());
        appendLog(applicationLog("Настройки сохранены. Пароль в файл не записывается."));
        updateLaunchStatus();
    } catch (const std::exception &error) {
        setStatus("Не удалось сохранить настройки", "attention");
        QMessageBox::critical(this, "Не удалось сохранить настройки",
                              QString::fromUtf8(error.what()));
    }
}

void BotWindow::loadSettings()
{
    if (m_busy || m_driverUpdater->isBusy())
        return;
    try {
        populateForm(BotConfig::load(settingsPath()));
        appendLog(applicationLog("Настройки загружены. Если нужен специальный аккаунт, введите пароль на этот запуск."));
    } catch (const std::exception &error) {
        setStatus("Не удалось загрузить настройки", "attention");
        QMessageBox::critical(this, "Не удалось загрузить настройки",
                              QString::fromUtf8(error.what()));
    }
}

void BotWindow::openReport()
{
    const QString path = reportPath();
    if (!QFileInfo::exists(path)) {
        setStatus("Отчёт ещё не создан", "attention");
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void BotWindow::openHistory()
{
    auto *history = new RunHistoryDialog(reportPath(), this);
    history->show();
}

void BotWindow::setBusy(bool busy)
{
    m_busy = busy;
    m_campaignForm->setEnabled(!busy);
    m_load->setEnabled(!busy);
    m_save->setEnabled(!busy);
    m_start->setVisible(!busy);
    m_checkForums->setVisible(!busy);
    m_stop->setVisible(busy);
    m_stop->setEnabled(busy);
    m_stop->setText("Остановить");
    if (busy) {
        refreshDriverControls();
        m_start->setEnabled(false);
        m_checkForums->setEnabled(false);
        setStatus(m_runningCheck ? "Проверяю форумы без публикации" : "Выполняется обход форумов", "running");
    } else {
        updateLaunchStatus();
    }
}

QString BotWindow::resolvedDriverPath() const
{
    const QString chosen = m_driverPath->text().trimmed();
    if (!chosen.isEmpty())
        return chosen;
    // Локальное окружение проекта имеет приоритет перед случайным старым
    // драйвером в глобальном PATH. Автовыбор не меняет сохранённые настройки.
    const QString local = DriverUpdater::localDriverPath();
    return QFileInfo(local).isExecutable() ? local : QStandardPaths::findExecutable("chromedriver");
}

QString BotWindow::launchIssue(const BotConfig &config) const
{
    if (m_driverUpdater->isBusy())
        return "Дождитесь проверки или установки ChromeDriver";
    if (!config.checkOnly && config.prCode.trimmed().isEmpty())
        return "Добавьте текст объявления";
    const QUrl topic(config.threadUrl);
    if (!topic.isValid() || topic.host().isEmpty()
        || (topic.scheme() != "http" && topic.scheme() != "https"))
        return "Укажите полную ссылку на тему взаимной рекламы";
    if (config.forumsFile.isEmpty())
        return "Выберите список форумов";
    if (!QFileInfo::exists(config.forumsFile) || !m_forumsReadable)
        return "Не удалось прочитать список форумов";
    if (m_forumCount == 0)
        return "Список форумов пуст";
    if (config.useAccount && config.login.trimmed().isEmpty())
        return "Введите логин аккаунта в дополнительных параметрах";
    if (config.useAccount && config.password.isEmpty())
        return "Введите пароль на текущий запуск";
    if (!QFileInfo(resolvedDriverPath()).isExecutable())
        return "Выберите ChromeDriver в дополнительных параметрах";
    if (m_driverUpdater->isKnownIncompatible(resolvedDriverPath()))
        return "Обновите ChromeDriver в параметрах браузера";
    return {};
}

void BotWindow::refreshForumFile()
{
    m_forumCount = 0;
    m_forumsReadable = false;
    const QString path = m_forumsFile->text().trimmed();
    QFile file(path);
    if (!path.isEmpty() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_forumsReadable = true;
        QTextStream stream(&file);
        stream.setEncoding(QStringConverter::Utf8);
        while (!stream.atEnd()) {
            const QString line = stream.readLine().trimmed();
            if (!line.isEmpty() && !line.startsWith('#'))
                ++m_forumCount;
        }
    }
    m_forumFileName->setText(path.isEmpty() ? "Файл не выбран" : QFileInfo(path).fileName());
    m_forumFileName->setToolTip(path);
    m_forumsStatus->setText(path.isEmpty() ? "Выберите список для обхода"
        : !m_forumsReadable ? "Файл недоступен для чтения"
        : m_forumCount == 0 ? "В файле пока нет адресов"
        : counted(m_forumCount, "форум", "форума", "форумов") + " в списке");
    m_forumsStatus->setProperty("state", m_forumsReadable && m_forumCount > 0 ? "ready" : "attention");
    m_forumsStatus->style()->unpolish(m_forumsStatus);
    m_forumsStatus->style()->polish(m_forumsStatus);
}

void BotWindow::updateLaunchStatus()
{
    if (!m_status)
        return;
    refreshDriverControls();
    if (m_busy)
        return;
    BotConfig config = readForm();
    const QString publicationIssue = launchIssue(config);
    config.checkOnly = true;
    const QString checkIssue = launchIssue(config);
    m_start->setEnabled(publicationIssue.isEmpty());
    m_checkForums->setEnabled(checkIssue.isEmpty());
    const QString status = publicationIssue.isEmpty() ? "Всё готово к запуску"
        : checkIssue.isEmpty() ? "Можно проверить форумы; для публикации добавьте объявление"
                               : checkIssue;
    setStatus(status, m_driverUpdater->isBusy() ? "running" : checkIssue.isEmpty() ? "ready" : "attention");
    m_progressLabel->setText(m_forumCount > 0
        ? counted(m_forumCount, "форум", "форума", "форумов")
            + (m_checkLastPage->isChecked() ? " · проверка повторов включена" : " · проверка повторов выключена")
        : "Заполните кампанию — кнопка запуска станет доступна");
}

void BotWindow::refreshDriverControls()
{
    const QString driver = resolvedDriverPath();
    const bool driverReady = QFileInfo(driver).isExecutable();
    const bool updating = m_driverUpdater->isBusy();
    const QString status = m_driverUpdater->status();
    m_driverStatus->setText(status.isEmpty()
        ? driverReady ? "Драйвер найден. Совместимость можно проверить ниже."
                      : "Драйвер не найден. Проверьте обновления, чтобы установить его."
        : status);
    m_driverStatus->setToolTip(driver);
    const QString chrome = m_driverUpdater->chromeVersion();
    const QString version = m_driverUpdater->driverVersion();
    m_driverVersions->setText(QString("Chrome: %1\nChromeDriver: %2")
        .arg(chrome.isEmpty() ? "ещё не проверен" : chrome,
             version.isEmpty() ? "ещё не определён" : version));
    m_checkDriver->setEnabled(!m_busy && !updating);
    m_installDriver->setVisible(m_driverUpdater->canInstall() && !updating);
    m_installDriver->setEnabled(!m_busy && !updating);
    m_cancelDriver->setVisible(updating);
    m_driverPath->setEnabled(!m_busy && !updating);
    m_chooseDriver->setEnabled(!m_busy && !updating);
    m_load->setEnabled(!m_busy && !updating);
    m_save->setEnabled(!m_busy && !updating);
}

void BotWindow::showPage(int index)
{
    m_pages->setCurrentIndex(index);
    m_campaignNav->setChecked(index == 0);
    m_journalNav->setChecked(index == 1);
    m_pageTitle->setText(index == 0 ? "Кампания" : "Журнал работы");
    m_pageDescription->setText(index == 0 ? "Бот размещает ваше объявление на выбранных форумах и публикует ответную рекламу в вашей теме."
                                         : "Ход обхода, подтверждённые публикации и события.");
}

void BotWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (!m_campaignColumns)
        return;
    const bool wide = width() >= 1080;
    m_campaignColumns->setDirection(wide ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    m_campaignColumns->setStretch(0, wide ? 3 : 0);
    m_campaignColumns->setStretch(1, wide ? 2 : 0);
}

void BotWindow::setStatus(const QString &text, const QString &state)
{
    if (!m_status)
        return;
    m_status->setText(text);
    m_status->setProperty("state", state);
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    m_statusDot->setProperty("state", state);
    m_statusDot->style()->unpolish(m_statusDot);
    m_statusDot->style()->polish(m_statusDot);
}

BotConfig BotWindow::readForm() const
{
    BotConfig config;
    config.prCode = m_prCode->toPlainText();
    config.threadUrl = m_threadUrl->text().trimmed();
    config.forumsFile = m_forumsFile->text().trimmed();
    config.chromeDriverPath = m_driverPath->text().trimmed();
    config.useAccount = m_useAccount->isChecked();
    config.login = m_login->text();
    config.password = m_password->text();
    config.checkLastPage = m_checkLastPage->isChecked();
    return config;
}

void BotWindow::populateForm(const BotConfig &config)
{
    m_prCode->setPlainText(config.prCode);
    m_threadUrl->setText(config.threadUrl);
    m_forumsFile->setText(config.forumsFile);
    m_driverPath->setText(config.chromeDriverPath);
    m_useAccount->setChecked(config.useAccount);
    m_login->setText(config.login);
    m_password->clear();
    m_passwordVisibility->setChecked(false);
    m_password->setEchoMode(QLineEdit::Password);
    m_checkLastPage->setChecked(config.checkLastPage);
    m_credentials->setVisible(config.useAccount);
    m_advancedToggle->setChecked(config.useAccount || !QFileInfo(resolvedDriverPath()).isExecutable());
    refreshForumFile();
    updateLaunchStatus();
}

void BotWindow::appendLog(const QString &message)
{
    // Храним ограниченный хвост для быстрого поиска в UI. Полный журнал
    // продолжает независимо записываться рабочим потоком в log.txt.
    m_logEntries.append(message);
    if (m_logEntries.size() > 5000) {
        m_logEntries.removeFirst();
        rebuildLog();
        return;
    }
    auto *bar = m_log->verticalScrollBar();
    const int position = bar->value();
    const bool follow = m_followLog->isChecked() && position >= bar->maximum() - 8;
    if (matchesLogEntry(message, m_logSearch->text().trimmed(), m_logFilter->currentIndex())) {
        insertLogEntry(message);
        ++m_visibleLogCount;
    }
    bar->setValue(follow ? bar->maximum() : position);
    m_logCount->setText(m_visibleLogCount == m_logEntries.size()
        ? counted(m_logEntries.size(), "событие", "события", "событий")
        : QString("Показано %1 из %2").arg(m_visibleLogCount).arg(m_logEntries.size()));
}

QStringList BotWindow::visibleLogEntries() const
{
    QStringList visible;
    const QString search = m_logSearch->text().trimmed();
    for (const auto &entry : m_logEntries) {
        if (matchesLogEntry(entry, search, m_logFilter->currentIndex()))
            visible.append(entry);
    }
    return visible;
}

void BotWindow::rebuildLog()
{
    const int position = m_log->verticalScrollBar()->value();
    const bool follow = m_followLog->isChecked() && position >= m_log->verticalScrollBar()->maximum() - 8;
    const QStringList entries = visibleLogEntries();
    m_log->setUpdatesEnabled(false);
    m_log->clear();
    for (const auto &entry : entries)
        insertLogEntry(entry);
    m_log->setUpdatesEnabled(true);
    m_visibleLogCount = int(entries.size());
    m_logCount->setText(entries.size() == m_logEntries.size()
        ? counted(entries.size(), "событие", "события", "событий")
        : QString("Показано %1 из %2").arg(entries.size()).arg(m_logEntries.size()));
    m_log->setPlaceholderText(m_logEntries.isEmpty()
        ? "События появятся после запуска обхода."
        : "Ничего не найдено. Измените запрос или фильтр событий.");
    auto *bar = m_log->verticalScrollBar();
    bar->setValue(follow ? bar->maximum() : position);
}

void BotWindow::insertLogEntry(const QString &message)
{
    // QTextCursor вставляет текст, а не HTML: разметка из ошибки сайта
    // не интерпретируется. Время сокращается только в UI; копия содержит оригинал.
    static const QRegularExpression pattern(
        R"(^\[([^\]]+)\] \[([^\]]+)\] \[([^\]]+)\](?: \[([^\]]+)\])? (.*)$)");
    const auto match = pattern.match(message);
    const bool error = message.contains("[ОШИБКА]");
    const bool warning = message.contains("[ВНИМАНИЕ]");
    const QColor color(error ? "#a65d46" : warning ? "#96713d" : "#41543c");
    QTextCursor cursor(m_log->document());
    cursor.movePosition(QTextCursor::End);
    if (!m_log->document()->isEmpty())
        cursor.insertBlock();
    QTextBlockFormat block;
    block.setTopMargin(4);
    block.setBottomMargin(3);
    cursor.setBlockFormat(block);
    QTextCharFormat header;
    header.setForeground(error || warning ? color : QColor("#83907a"));
    header.setFontPointSize(9);
    if (match.hasMatch()) {
        const QDateTime time = QDateTime::fromString(match.captured(1), Qt::ISODateWithMs);
        const QString host = QUrl(match.captured(4)).host();
        QString line = time.toString("HH:mm:ss") + "  ·  " + match.captured(3);
        if (!host.isEmpty()) line += "  ·  " + host;
        if (warning || error) line += "  ·  " + match.captured(2);
        cursor.insertText(line, header);
        cursor.insertBlock();
        block.setTopMargin(0);
        block.setBottomMargin(8);
        cursor.setBlockFormat(block);
    }
    QTextCharFormat body;
    body.setForeground(color);
    body.setFontPointSize(10);
    cursor.insertText(match.hasMatch() ? match.captured(5) : message, body);
}

QString BotWindow::settingsPath() const
{
    return QDir::current().filePath("settings.json");
}

QString BotWindow::reportPath() const
{
    return QDir::current().filePath("log.txt");
}
