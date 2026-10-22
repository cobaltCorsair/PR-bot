#include "run_log.h"

#include <QDateTime>
#include <QRegularExpression>
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

RunLog::RunLog(QString path, QStringList hiddenValues)
    : m_file(std::move(path)), m_hiddenValues(std::move(hiddenValues))
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
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        m_writeError = m_file.errorString();
        throw std::runtime_error(QString("Не удалось открыть журнал %1: %2")
                                     .arg(m_file.fileName(), m_writeError).toStdString());
    }
    m_writeError.clear();
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
    if (!m_file.isOpen())
        return false;
    const QByteArray bytes = text.toUtf8();
    if (m_file.write(bytes) == bytes.size() && m_file.flush())
        return true;

    m_writeError = m_file.errorString();
    m_file.close();
    return false;
}
