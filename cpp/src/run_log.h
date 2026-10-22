#pragma once

#include <QFile>
#include <QStringList>

enum class LogLevel { Info, Warning, Error };

// Один журнал прохода: строки сразу пишутся на диск и передаются в окно без
// повторного форматирования. Тело WebDriver-запросов сюда не передаётся.
class RunLog final {
public:
    RunLog(QString path, QStringList hiddenValues);

    // Новый запуск заменяет предыдущий log.txt. Ошибка открытия останавливает
    // подготовку до запуска браузера; сбой записи позднее не прерывает отправку.
    void open();
    QString append(LogLevel level, const QString &stage, const QString &message,
                   const QString &forumUrl = {});
    bool appendReport(const QString &report);
    QString errorString() const;

private:
    QString redact(QString text) const;
    QString singleLine(const QString &text) const;
    bool write(const QString &text);

    QFile m_file;
    QStringList m_hiddenValues;
    QString m_writeError;
};
