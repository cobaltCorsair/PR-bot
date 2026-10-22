#include "../src/bot_window.h"
#include "../src/config.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextEdit>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template<typename T> T *find(BotWindow &window, const char *name)
{
    auto *widget = window.findChild<T *>(QString::fromLatin1(name));
    require(widget, name);
    return widget;
}

void settle()
{
    // Достаточно обработать раскладку/перерисовку; браузер не запускается.
    for (int pass = 0; pass < 4; ++pass)
        QApplication::processEvents();
}

void saveView(BotWindow &window, const QString &directory, const QString &name)
{
    if (!directory.isEmpty()) {
        settle();
        require(window.grab().save(directory + '/' + name + ".png"), "Cannot save UI preview.");
    }
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    const QString previews = argc > 1 ? QDir(QString::fromLocal8Bit(argv[1])).absolutePath() : QString();
    if (!previews.isEmpty())
        require(QDir().mkpath(previews), "Cannot create preview directory.");
    QTemporaryDir directory;
    const QString previousDirectory = QDir::currentPath();
    try {
        require(directory.isValid(), "Cannot create UI test directory.");
        QFile forums(directory.filePath("forums.txt"));
        require(forums.open(QIODevice::WriteOnly), "Cannot create UI fixture.");
        forums.write("# A local UI fixture, never visited\n\nhttps://one.example/\nhttps://two.example/\n"
                     "https://three.example/\nhttps://four.example/\n");
        forums.close();
        BotConfig config;
        config.prCode = "[align=center]\n[url=https://your-forum.example/]\n"
                        "[img]https://your-forum.example/banner.png[/img]\n[/url]\n\n"
                        "[b]Ваша история начинается здесь[/b]\nМир, персонажи и новые приключения.\n[/align]";
        config.threadUrl = "https://your-forum.example/viewtopic.php?id=14";
        config.forumsFile = forums.fileName();
        // Нужен только существующий executable для проверки готовности формы.
        // Старт в этом тесте не вызывается, файл не запускается как ChromeDriver.
        config.chromeDriverPath = QCoreApplication::applicationFilePath();
        config.save(directory.filePath("settings.json"));
        require(QDir::setCurrent(directory.path()), "Cannot enter UI fixture directory.");
        {
            BotWindow window;
            window.show();
            settle();
            auto *start = find<QPushButton>(window, "primaryButton");
            auto *advanced = find<QPushButton>(window, "advancedToggle");
            auto *credentials = find<QWidget>(window, "credentials");
            auto *account = find<QCheckBox>(window, "useAccount");
            auto *scroll = find<QScrollArea>(window, "campaignScroll");
            require(start->isEnabled(), "Valid form did not enable launch.");
            require(!credentials->isVisible(), "Unused credentials should be hidden.");
            require(scroll->horizontalScrollBar()->maximum() == 0, "Wide layout overflows horizontally.");
            require(find<QWidget>(window, "targetsCard")->x() > find<QWidget>(window, "editorCard")->x(),
                    "Wide layout did not put targets next to the editor.");
            saveView(window, previews, "campaign");

            advanced->click();
            account->setChecked(true);
            settle();
            require(credentials->isVisible() && !start->isEnabled(), "Account options are not progressive.");
            find<QLineEdit>(window, "login")->setText("PR");
            auto *password = find<QLineEdit>(window, "password");
            password->setText("demo-only");
            require(start->isEnabled(), "Filled account settings did not enable launch.");
            saveView(window, previews, "advanced");
            password->actions().first()->setChecked(true);
            require(password->echoMode() == QLineEdit::Normal, "Password visibility action failed.");
            require(QMetaObject::invokeMethod(&window, "loadSettings", Qt::DirectConnection), "Cannot reload settings.");
            require(password->text().isEmpty() && password->echoMode() == QLineEdit::Password,
                    "Loading settings left a visible password behind.");

            QFile empty(directory.filePath("empty.txt"));
            require(empty.open(QIODevice::WriteOnly), "Cannot create empty list fixture.");
            empty.write("# No targets\n\n");
            empty.close();
            QMimeData mime;
            mime.setUrls({QUrl::fromLocalFile(empty.fileName())});
            auto *dropArea = find<QWidget>(window, "targetsCard");
#if QT_VERSION >= QT_VERSION_CHECK(6, 16, 0)
            QDragEnterEvent enter(QPointF(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
#else
            QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
#endif
            QApplication::sendEvent(dropArea, &enter);
            QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(dropArea, &drop);
            require(enter.isAccepted() && drop.isAccepted() && !start->isEnabled(),
                    "Dropping an empty target file did not update readiness.");
            find<QLineEdit>(window, "forumsFile")->setText(forums.fileName());
            scroll->verticalScrollBar()->setValue(0);
            require(start->isEnabled(), "Restoring the target list did not restore readiness.");
            find<QLineEdit>(window, "threadUrl")->setText("broken URL");
            require(!start->isEnabled(), "Invalid source URL enabled launch.");
            find<QLineEdit>(window, "threadUrl")->setText(config.threadUrl);

            window.resize(980, 720);
            settle();
            require(dropArea->y() > find<QWidget>(window, "editorCard")->y(),
                    "Compact layout did not stack the sections.");
            require(scroll->horizontalScrollBar()->maximum() == 0 && start->isVisible()
                    && window.rect().contains(start->mapTo(&window, QPoint(0, 0))),
                    "Compact layout clipped the launch action or created horizontal overflow.");
            saveView(window, previews, "compact");

            window.resize(1220, 840);
            find<QPushButton>(window, "journalNav")->click();
            find<QPushButton>(window, "journalNav")->setFocus();
            const QString events[] = {
                "[2026-10-22T12:00:00.000+03:00] [ИНФО] [Настройки] Пример события журнала: настройки готовы.",
                "[2026-10-22T12:00:01.000+03:00] [ВНИМАНИЕ] [Проверка] [https://forum.example/] Пример предупреждения.",
                "[2026-10-22T12:00:02.000+03:00] [ОШИБКА] [Диагностика] Пример ошибки: <b>это текст, а не HTML</b>."
            };
            for (const auto &event : events)
                require(QMetaObject::invokeMethod(&window, "appendLog", Qt::DirectConnection, Q_ARG(QString, event)),
                        "Cannot deliver a UI log event.");
            auto *log = find<QTextEdit>(window, "log");
            require(log->toPlainText().contains("<b>это текст, а не HTML</b>"), "Log interpreted untrusted HTML.");
            saveView(window, previews, "journal");
            auto *filter = find<QComboBox>(window, "logFilter");
            filter->setCurrentIndex(2);
            require(log->toPlainText().contains("Пример ошибки") && !log->toPlainText().contains("Пример события"),
                    "Error filter did not select the correct entries.");
            find<QPushButton>(window, "copyLog")->click();
            require(app.clipboard()->text() == events[2], "Copy did not use the filtered original log.");
            auto *search = find<QLineEdit>(window, "logSearch");
            search->setText("нет такой строки");
            require(log->toPlainText().isEmpty(), "Journal search did not filter entries.");
            search->clear();
            filter->setCurrentIndex(0);
            require(log->toPlainText().contains("Пример предупреждения")
                    && log->toPlainText().contains("Пример события"), "Filtering lost journal entries.");
        }
        require(QDir::setCurrent(previousDirectory), "Cannot restore working directory.");
    } catch (const std::exception &error) {
        QDir::setCurrent(previousDirectory);
        std::cerr << "UI checks failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Responsive layout, file drop, readiness, credentials and journal checks passed.\n";
    return 0;
}
