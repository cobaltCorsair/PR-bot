#pragma once

#include <QFile>
#include <QStringList>

enum class LogLevel { Info, Warning, Error };

// Один журнал прохода: строки сразу пишутся на диск и передаются в окно без
// повторного форматирования. Тело WebDriver-запросов сюда не передаётся.
class RunLog final {
public:
    RunLog(QString path, QStringList hiddenValues, bool checkOnly = false);

    // log.txt содержит текущий проход, logs/ — отдельный файл каждого запуска.
    // Оба файла открываются до браузера; сбой записи не вызывает повтор отправки.
    void open();
    QString append(LogLevel level, const QString &stage, const QString &message,
                   const QString &forumUrl = {});
    bool appendReport(const QString &report);
    QString errorString() const;
    QString archivePath() const { return m_archive.fileName(); }
    static QString historyDirectory(const QString &reportPath);

private:
    QString redact(QString text) const;
    QString singleLine(const QString &text) const;
    bool write(const QString &text);

    QFile m_file;
    QFile m_archive;
    bool m_checkOnly = false;
    QStringList m_hiddenValues;
    QString m_writeError;
};
