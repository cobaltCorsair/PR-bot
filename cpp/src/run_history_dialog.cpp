#include "run_history_dialog.h"

#include "run_log.h"
#include "window_theme.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>

RunHistoryDialog::RunHistoryDialog(const QString &reportPath, QWidget *parent)
    : QDialog(parent), m_directory(RunLog::historyDirectory(reportPath))
{
    setObjectName("workspace");
    setWindowTitle("История запусков — PR-Bot");
    setAttribute(Qt::WA_DeleteOnClose);
    setStyleSheet(UiTheme::stylesheet());
    resize(1000, 680);
    setMinimumSize(760, 500);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 22);
    layout->setSpacing(14);
    auto *title = new QLabel("История запусков", this);
    title->setProperty("role", "sectionTitle");
    layout->addWidget(title);
    m_description = new QLabel(this);
    m_description->setProperty("role", "hint");
    m_description->setWordWrap(true);
    layout->addWidget(m_description);
    auto *splitter = new QSplitter(this);
    m_runs = new QListWidget(splitter);
    m_runs->setObjectName("historyRuns");
    m_runs->setAccessibleName("Сохранённые запуски");
    m_runs->setMinimumWidth(240);
    m_preview = new QTextEdit(splitter);
    m_preview->setObjectName("log");
    m_preview->setReadOnly(true);
    m_preview->setAccessibleName("Журнал выбранного запуска");
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    layout->addWidget(splitter, 1);
    auto *actions = new QHBoxLayout;
    auto *refresh = new QPushButton("Обновить", this);
    auto *folder = new QPushButton("Открыть папку", this);
    folder->setIcon(UiTheme::icon(UiTheme::Icon::Folder));
    m_openFile = new QPushButton("Открыть полный журнал", this);
    auto *close = new QPushButton("Закрыть", this);
    actions->addWidget(refresh);
    actions->addWidget(folder);
    actions->addWidget(m_openFile);
    actions->addStretch();
    actions->addWidget(close);
    layout->addLayout(actions);
    connect(m_runs, &QListWidget::currentRowChanged, this, [this] { showSelected(); });
    connect(refresh, &QPushButton::clicked, this, [this] { reload(); });
    connect(folder, &QPushButton::clicked, this, [this] {
        if (QDir(m_directory).exists()) QDesktopServices::openUrl(QUrl::fromLocalFile(m_directory));
    });
    connect(m_openFile, &QPushButton::clicked, this, [this] {
        if (!selectedPath().isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(selectedPath()));
    });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    reload();
}

QString RunHistoryDialog::selectedPath() const
{
    const auto *item = m_runs->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void RunHistoryDialog::reload()
{
    const QString previous = selectedPath();
    m_runs->clear();
    const auto files = QDir(m_directory).entryInfoList({"*.log"}, QDir::Files | QDir::NoSymLinks,
                                                      QDir::Name | QDir::Reversed);
    int selected = 0;
    for (const auto &file : files) {
        const QString name = file.fileName();
        const QDateTime started = QDateTime::fromString(name.left(23), "yyyy-MM-dd_HH-mm-ss-zzz");
        const QString mode = name.contains("_check_") ? "Проверка без публикации"
            : name.contains("_imported_") ? "Журнал прежней версии" : "Публикация";
        auto *item = new QListWidgetItem(
            (started.isValid() ? started.toString("dd.MM.yyyy  HH:mm:ss") : name) + '\n' + mode, m_runs);
        item->setData(Qt::UserRole, file.absoluteFilePath());
        item->setToolTip(file.absoluteFilePath());
        if (file.absoluteFilePath() == previous) selected = m_runs->count() - 1;
    }
    m_runs->setCurrentRow(files.isEmpty() ? -1 : selected);
    m_description->setText(files.isEmpty()
        ? "История пока пуста. Здесь появятся журналы проверок и публикаций после запуска."
        : QString("Сохранено запусков: %1. Выберите запись; «Обновить» перечитает и текущий журнал.")
              .arg(files.size()));
    showSelected();
}

void RunHistoryDialog::showSelected()
{
    const QString path = selectedPath();
    m_openFile->setEnabled(!path.isEmpty());
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        m_preview->setPlainText(path.isEmpty() ? "Выберите запуск слева." : "Не удалось открыть журнал.");
        return;
    }
    // Большой файл не должен блокировать окно: показываем хвост и оставляем
    // отдельную кнопку для полного журнала. HTML из ошибок всегда остаётся текстом.
    constexpr qint64 limit = 2 * 1024 * 1024;
    const bool tail = file.size() > limit;
    if (tail) {
        file.seek(file.size() - limit);
        file.readLine(); // Начинаем с целой строки UTF-8.
    }
    m_preview->setPlainText((tail ? "Показаны последние 2 МБ журнала.\n\n" : QString())
                           + QString::fromUtf8(file.readAll()));
    m_preview->verticalScrollBar()->setValue(m_preview->verticalScrollBar()->maximum());
}
