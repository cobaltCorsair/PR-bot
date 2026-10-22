#include "bot_window.h"

#include <QApplication>
#include <QDir>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("PR-Bot");
    app.setStyle("Fusion");

    // Переносимая Windows-версия хранит данные рядом с exe. Каталог, из
    // которого запущен ярлык/Проводник, не должен менять выбранную кампанию.
#ifdef Q_OS_WIN
    QDir::setCurrent(QCoreApplication::applicationDirPath());
#endif
    // При запуске из исходников на macOS/Linux используется корень проекта.
    const QString iconPath = QDir::current().filePath("src/icon.png");
    app.setWindowIcon(QIcon(iconPath));

    BotWindow window;
    window.show();
    return app.exec();
}
