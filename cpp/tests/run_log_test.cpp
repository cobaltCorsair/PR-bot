#include "../src/run_log.h"
#include "../src/pr_bot.h"

#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <exception>
#include <iostream>

namespace {
int fail(const char *message)
{
    std::cerr << message << '\n';
    return 1;
}

QString readText(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid())
        return fail("Temporary directory could not be created.");

    const QString path = directory.filePath("log.txt");
    const QString password = "sensitive-password";
    const QString advertising = "[b]first line\nsecond line[/b]";
    QString firstArchive;
    QString firstContents;
    try {
        {
            RunLog journal(path, {password, advertising});
            journal.open();
            firstArchive = journal.archivePath();
            const QString first = journal.append(LogLevel::Info, "Проверка темы",
                "Открыта рекламная тема", "https://forum.example/");
            // Файл доступен ещё во время прохода, до закрытия журнала.
            if (readText(path) != first + '\n')
                return fail("Live log and emitted line differ, or the log was not flushed.");

            const QString entry = journal.append(LogLevel::Error, "Отправка",
                QString("Ошибка: %1\n%2\r\nhttps://user:other-secret@forum.example/topic?id=4#p7")
                    .arg(password, advertising));
            if (entry.contains(password) || entry.contains(advertising)
                || entry.contains("other-secret") || entry.contains('\n') || entry.contains('\r'))
                return fail("A log line leaked a secret or allowed multiline injection.");
            if (!entry.contains("[ОШИБКА]") || !entry.contains("[Отправка]")
                || !entry.contains("forum.example/topic?id=4#p7"))
                return fail("Diagnostic context or the useful part of a URL was lost.");

            if (!journal.appendReport("Успешно: 0\nПричина: " + password + '\n' + advertising))
                return fail("Could not append the summary.");
            const QString saved = readText(path);
            firstContents = saved;
            if (readText(firstArchive) != saved) return fail("Archive differs from current log.");
            if (!saved.startsWith(first) || !saved.contains("Успешно: 0\nПричина:")
                || saved.contains(password) || saved.contains(advertising))
                return fail("Appending the summary lost earlier events or exposed a secret.");
        }
        {
            RunLog nextRun(path, {});
            nextRun.open();
            const QString line = nextRun.append(LogLevel::Warning, "Старт", "Новый проход");
            if (readText(firstArchive) != firstContents || nextRun.archivePath() == firstArchive
                || QDir(RunLog::historyDirectory(path)).entryList({"*.log"}, QDir::Files).size() != 2)
                return fail("New run overwrote or lost the previous archive.");
            if (readText(path) != line + '\n')
                return fail("A new run mixed events with the previous run.");
        }
        bool rejected = false;
        try {
            RunLog unavailable(directory.filePath("missing/log.txt"), {});
            unavailable.open();
        } catch (const std::exception &) {
            rejected = true;
        }
        if (!rejected)
            return fail("An unavailable log destination was silently accepted.");

        // Проверяем связь сценария с журналом на ошибке подготовки. Несуществующий
        // список останавливает run() раньше запуска ChromeDriver и любых сайтов.
        BotConfig config;
        config.forumsFile = directory.filePath("missing-forums.txt");
        config.password = password;
        const QString failedRunPath = directory.filePath("failed-run.txt");
        PrBot bot(config, failedRunPath);
        QStringList displayed;
        bool finished = false;
        bool reportWritten = false;
        QObject::connect(&bot, &PrBot::logMessage, &app,
                         [&displayed](const QString &line) { displayed.append(line); });
        QObject::connect(&bot, &PrBot::finished, &app, [&finished] { finished = true; });
        QObject::connect(&bot, &PrBot::reportWritten, &app,
                         [&reportWritten](const QString &) { reportWritten = true; });
        bot.run();
        const QString failedRun = readText(failedRunPath);
        if (!finished || !reportWritten || !failedRun.contains("Не удалось открыть список форумов")
            || !failedRun.contains("=== ИТОГИ ПРОХОДА ===")
            || !failedRun.contains("Критическая ошибка:") || failedRun.contains("Запускаю драйвер"))
            return fail("A failed preflight did not preserve diagnostics or finish cleanly.");
        for (const QString &line : displayed) {
            if (!failedRun.contains(line))
                return fail("The UI received an event absent from the saved journal.");
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Run log checks passed.\n";
    return 0;
}
