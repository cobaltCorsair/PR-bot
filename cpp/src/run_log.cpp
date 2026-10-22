#include "run_log.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace {
QString levelLabel(LogLevel level)
{
    switch (level) {
    case LogLevel::Info: return QStringLiteral("ИНФО");
    case LogLevel::Warning: return QStringLiteral("ВНИМАНИЕ");
    case LogLevel::Error: return QStringLiteral("ОШИБКА");
    }
    return QStringLiteral("ИНФО");
}
}

RunLog::RunLog(QString path, QStringList hiddenValues, bool checkOnly)
    : m_file(std::move(path)), m_checkOnly(checkOnly), m_hiddenValues(std::move(hiddenValues))
{
    m_hiddenValues.removeAll(QString());
    // Сначала скрываем длинные значения, чтобы короткий пароль внутри BBCode
    // не помешал скрыть весь рекламный шаблон, случайно попавший в ошибку сайта.
    std::sort(m_hiddenValues.begin(), m_hiddenValues.end(),
              [](const QString &left, const QString &right) {
                  return left.size() > right.size();
              });
}

void RunLog::open()
{
    const QDir history(historyDirectory(m_file.fileName()));
    // Не создаём отсутствующий родительский каталог рабочего журнала: ошибка
    // пути по-прежнему должна останавливать запуск до обращения к форумам.
    if (!QFileInfo(m_file.fileName()).absoluteDir().exists() || !QDir().mkpath(history.absolutePath()))
        throw std::runtime_error("Не удалось создать каталог истории запусков.");

    const QString stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz");
    const QString unique = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    // Однократно сохраняем последний журнал старой версии перед его заменой.
    if (history.entryList({"*.log"}, QDir::Files).isEmpty() && m_file.exists() && m_file.size() > 0) {
        const QString legacy = history.filePath(stamp + "_imported_" + unique + ".log");
        if (!QFile::copy(m_file.fileName(), legacy))
            throw std::runtime_error("Не удалось сохранить предыдущий журнал в историю.");
    }
    m_archive.setFileName(history.filePath(stamp + (m_checkOnly ? "_check_" : "_publish_") + unique + ".log"));
    if (!m_archive.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::NewOnly))
        throw std::runtime_error(QString("Не удалось открыть журнал истории: %1")
            .arg(m_archive.errorString()).toStdString());
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        m_writeError = m_file.errorString();
        m_archive.close();
        m_archive.remove();
        throw std::runtime_error(QString("Не удалось открыть журнал %1: %2")
                                     .arg(m_file.fileName(), m_writeError).toStdString());
    }
    m_writeError.clear();
}

QString RunLog::historyDirectory(const QString &reportPath)
{
    return QFileInfo(reportPath).absoluteDir().filePath("logs");
}

QString RunLog::append(LogLevel level, const QString &stage, const QString &message,
                       const QString &forumUrl)
{
    QString line = QString("[%1] [%2] [%3]")
                       .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
                            levelLabel(level), singleLine(stage));
    if (!forumUrl.isEmpty())
        line += QString(" [%1]").arg(singleLine(forumUrl));
    line += ' ' + singleLine(message);
    write(line + '\n');
    return line;
}

bool RunLog::appendReport(const QString &report)
{
    return write("\n=== ИТОГИ ПРОХОДА ===\n" + redact(report) + '\n');
}

QString RunLog::errorString() const
{
    return m_writeError;
}

QString RunLog::redact(QString text) const
{
    for (const QString &hidden : m_hiddenValues)
        text.replace(hidden, QStringLiteral("[скрыто]"));
    // В URL может оказаться встроенный логин/пароль; для диагностики достаточно
    // хоста и пути. Обычные ссылки на темы и сообщения остаются читаемыми.
    static const QRegularExpression credentials(QStringLiteral("(https?://)[^\\s/@]+@"),
                                                 QRegularExpression::CaseInsensitiveOption);
    text.replace(credentials, QStringLiteral("\\1[скрыто]@"));
    return text;
}

QString RunLog::singleLine(const QString &text) const
{
    QString result = redact(text);
    // Ошибка от сайта не должна создавать поддельные строки журнала.
    result.replace('\r', ' ');
    result.replace('\n', ' ');
    result.replace('\t', ' ');
    return result;
}

bool RunLog::write(const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    bool saved = true;
    // Независимые записи: ошибка одного файла не лишает второй дальнейших
    // событий. Публикация не повторяется из-за проблемы с журналом.
    for (QFile *file : {&m_file, &m_archive}) {
        if (!file->isOpen()) {
            saved = false;
        } else if (file->write(bytes) != bytes.size() || !file->flush()) {
            m_writeError = file->fileName() + ": " + file->errorString();
            file->close();
            saved = false;
        }
    }
    return saved;
}
