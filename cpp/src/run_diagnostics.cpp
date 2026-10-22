#include "run_diagnostics.h"
#include "run_log.h"
#include "webdriver_client.h"
#include "build_info.h"

#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSysInfo>
#include <QSet>
#include <stdexcept>
#include <array>
#include <utility>

namespace {
constexpr qint64 kLogLimit = 8 * 1024 * 1024;
constexpr qint64 kImageLimit = 16 * 1024 * 1024;
constexpr qint64 kBundleLimit = 512 * 1024 * 1024;

void atomicWrite(const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error(QString("Не удалось сохранить %1: %2").arg(QFileInfo(path).fileName(), file.errorString()).toStdString());
}

QByteArray readFile(const QString &path, qint64 limit)
{
    const QFileInfo info(path);
    QFile file(path);
    if (!info.isFile() || info.isSymLink() || info.size() > limit || !file.open(QIODevice::ReadOnly))
        throw std::runtime_error(QString("Не удалось прочитать %1 или превышен размер файла.").arg(info.fileName()).toStdString());
    const auto bytes = file.read(limit + 1);
    if (bytes.size() > limit || file.error() != QFileDevice::NoError)
        throw std::runtime_error("Ошибка чтения диагностического файла.");
    return bytes;
}

QJsonObject readMetadata(const QString &path)
{
    QJsonParseError error{};
    const auto doc = QJsonDocument::fromJson(readFile(path, 4 * 1024 * 1024), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()
        || doc.object().value("format_version").toInt() != 1 || !doc.object().value("failures").isArray())
        throw std::runtime_error("Некорректный файл диагностики запуска.");
    return doc.object();
}

// ZIP32, метод Store: PNG уже сжаты, а журналы невелики. Пишем стандартные
// записи прямо в QSaveFile, без внешнего архиватора и зависимости от ОС.
// Формат: https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT
class ZipFile final {
public:
    explicit ZipFile(const QString &path) : m_file(path), m_out(&m_file)
    {
        m_file.setDirectWriteFallback(false);
        if (!m_file.open(QIODevice::WriteOnly)) throw std::runtime_error("Не удалось открыть ZIP для записи.");
        m_out.setByteOrder(QDataStream::LittleEndian);
        const auto now = QDateTime::currentDateTime();
        const auto date = now.date(); const auto time = now.time();
        m_date = quint16((qBound(1980, date.year(), 2107) - 1980) << 9 | date.month() << 5 | date.day());
        m_time = quint16(time.hour() << 11 | time.minute() << 5 | time.second() / 2);
    }
    void add(const QByteArray &name, const QByteArray &data)
    {
        const qint64 centralBytes = m_directoryBytes + 46 + name.size();
        if (m_entries.size() >= 65000 || m_file.pos() + 30 + name.size() + data.size() + centralBytes + 22 > kBundleLimit)
            throw std::runtime_error("Диагностика превышает допустимый размер ZIP (512 МБ).");
        Entry entry{name, quint32(data.size()), crc32(data), quint32(m_file.pos())};
        m_out << quint32(0x04034b50) << quint16(20) << quint16(0x0800) << quint16(0)
              << m_time << m_date << entry.crc << entry.size << entry.size
              << quint16(name.size()) << quint16(0);
        bytes(name); bytes(data);
        m_entries.append(entry);
        m_directoryBytes = centralBytes;
    }
    void finish()
    {
        const quint32 offset = quint32(m_file.pos());
        for (const auto &entry : m_entries) {
            m_out << quint32(0x02014b50) << quint16(20) << quint16(20) << quint16(0x0800) << quint16(0)
                  << m_time << m_date << entry.crc << entry.size << entry.size
                  << quint16(entry.name.size()) << quint16(0) << quint16(0) << quint16(0)
                  << quint16(0) << quint32(0) << entry.offset;
            bytes(entry.name);
        }
        const quint32 directorySize = quint32(m_file.pos()) - offset;
        m_out << quint32(0x06054b50) << quint16(0) << quint16(0) << quint16(m_entries.size())
              << quint16(m_entries.size()) << directorySize << offset << quint16(0);
        if (m_out.status() != QDataStream::Ok || !m_file.commit())
            throw std::runtime_error("Не удалось завершить запись ZIP. Прежний файл сохранён.");
    }
private:
    struct Entry { QByteArray name; quint32 size, crc, offset; };
    static quint32 crc32(const QByteArray &data)
    {
        static const auto table = [] {
            std::array<quint32, 256> values{};
            for (quint32 i = 0; i < values.size(); ++i) {
                quint32 value = i;
                for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
                values[i] = value;
            }
            return values;
        }();
        quint32 crc = 0xffffffffu;
        for (unsigned char byte : data) crc = (crc >> 8) ^ table[(crc ^ byte) & 0xffu];
        return crc ^ 0xffffffffu;
    }
    void bytes(const QByteArray &data)
    {
        if (m_file.write(data) != data.size()) throw std::runtime_error("Ошибка записи ZIP.");
    }
    QSaveFile m_file;
    QDataStream m_out;
    QList<Entry> m_entries;
    quint16 m_time = 0, m_date = 0;
    qint64 m_directoryBytes = 0;
};
}

RunDiagnostics::RunDiagnostics(QStringList hiddenValues) : m_hiddenValues(std::move(hiddenValues)) {}
QString RunDiagnostics::redact(const QString &text) const { return RunLog::redactText(text, m_hiddenValues); }

QString RunDiagnostics::directoryForLog(const QString &path)
{
    const QFileInfo log(path);
    if (path.isEmpty() || log.suffix() != "log" || log.absoluteDir().dirName() != "logs") return {};
    QDir root = log.absoluteDir(); root.cdUp();
    return root.filePath("diagnostics/" + log.completeBaseName());
}

bool RunDiagnostics::available(const QString &path)
{
    const QString directory = directoryForLog(path);
    return !directory.isEmpty() && QFileInfo::exists(QDir(directory).filePath("run.json"));
}

QString RunDiagnostics::latestLog(const QString &reportPath)
{
    for (const auto &file : QDir(RunLog::historyDirectory(reportPath)).entryInfoList({"*.log"},
             QDir::Files | QDir::NoSymLinks, QDir::Name | QDir::Reversed))
        if (available(file.absoluteFilePath())) return file.absoluteFilePath();
    return {};
}

bool RunDiagnostics::start(const QString &archiveLogPath, bool checkOnly)
{
    m_directory = directoryForLog(archiveLogPath);
    m_metadata = QJsonObject{{"format_version", 1}, {"application", "PR-Bot"},
        {"application_version", QString::fromLatin1(BuildInfo::version)},
        {"revision", QString::fromLatin1(BuildInfo::revision)}, {"qt_version", QString::fromLatin1(qVersion())},
        {"system", QSysInfo::prettyProductName()}, {"architecture", QSysInfo::currentCpuArchitecture()},
        {"started_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"mode", checkOnly ? "check" : "publish"}, {"log_name", QFileInfo(archiveLogPath).fileName()},
        {"browser_version", "недоступна"}, {"chromedriver_version", "недоступна"}, {"failures", QJsonArray{}}};
    if (m_directory.isEmpty() || !QDir().mkpath(m_directory)) {
        m_error = "Не удалось создать папку диагностики.";
        m_directory.clear(); return false;
    }
    return save();
}

bool RunDiagnostics::setVersions(const QString &browser, const QString &driver)
{
    if (!browser.isEmpty()) m_metadata.insert("browser_version", browser);
    if (!driver.isEmpty()) m_metadata.insert("chromedriver_version", driver);
    return save();
}

bool RunDiagnostics::recordFailure(WebDriverClient *driver, const QString &forum, const QString &stage,
                                   const QString &message, const QString &publicationPhase)
{
    if (m_directory.isEmpty()) return false;
    QJsonArray failures = m_metadata.value("failures").toArray();
    QJsonObject failure{{"recorded_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"forum_url", redact(forum)}, {"stage", redact(stage)}, {"error", redact(message)},
        {"publication_phase", publicationPhase}};
    if (driver) {
        if (!driver->browserVersion().isEmpty()) m_metadata.insert("browser_version", driver->browserVersion());
        if (!driver->driverVersion().isEmpty()) m_metadata.insert("chromedriver_version", driver->driverVersion());
    }
    if (driver && driver->hasSession()) {
        try { failure.insert("page_url", redact(driver->currentUrl(2500))); }
        catch (const std::exception &error) { failure.insert("page_url_error", redact(QString::fromUtf8(error.what()))); }
        try {
            const QString name = QString("failure-%1.png").arg(failures.size() + 1, 4, 10, QChar('0'));
            const QByteArray png = driver->takeScreenshot(m_hiddenValues);
            atomicWrite(QDir(m_directory).filePath(name), png);
            failure.insert("screenshot", name);
        } catch (const std::exception &error) {
            failure.insert("screenshot_error", redact(QString::fromUtf8(error.what())));
        }
    } else failure.insert("screenshot_error", "Сессия браузера ещё не создана или уже закрыта; снимок страницы недоступен.");
    m_screenshotError = failure.value("screenshot_error").toString();
    failures.append(failure);
    m_metadata.insert("failures", failures);
    return save();
}

bool RunDiagnostics::finish(bool cancelled, const QString &criticalError)
{
    m_metadata.insert("finished_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    m_metadata.insert("cancelled", cancelled);
    m_metadata.insert("critical_error", redact(criticalError));
    return save();
}

bool RunDiagnostics::save()
{
    if (m_directory.isEmpty()) return false;
    try {
        atomicWrite(QDir(m_directory).filePath("run.json"), QJsonDocument(m_metadata).toJson(QJsonDocument::Indented));
        m_error.clear(); return true;
    } catch (const std::exception &error) { m_error = QString::fromUtf8(error.what()); return false; }
}

void RunDiagnostics::exportZip(const QString &archiveLogPath, const QString &destination)
{
    if (!destination.endsWith(".zip", Qt::CaseInsensitive)) throw std::runtime_error("Укажите файл с расширением .zip.");
    const QString directory = directoryForLog(archiveLogPath);
    if (directory.isEmpty()) throw std::runtime_error("Для этого журнала нет диагностики.");
    const QDir folder(directory);
    const auto metadata = readMetadata(folder.filePath("run.json"));
    if (metadata.value("log_name").toString() != QFileInfo(archiveLogPath).fileName())
        throw std::runtime_error("Диагностика относится к другому журналу.");
    // Нельзя заменить собственные исходные файлы, даже если пользователь
    // вручную ввёл их путь вместо имени ZIP в диалоге сохранения.
    const QString output = QFileInfo(destination).absoluteFilePath();
    if (output == QFileInfo(archiveLogPath).absoluteFilePath() || output.startsWith(folder.absolutePath() + '/'))
        throw std::runtime_error("Сохраните ZIP вне папки этого диагностического запуска.");
    QFile log(archiveLogPath);
    if (QFileInfo(archiveLogPath).isSymLink() || !log.open(QIODevice::ReadOnly))
        throw std::runtime_error("Не удалось прочитать журнал запуска.");
    const bool truncated = log.size() > kLogLimit;
    if (truncated) { log.seek(log.size() - kLogLimit); log.readLine(); }
    const QByteArray logBytes = log.read(kLogLimit);
    if (log.error() != QFileDevice::NoError) throw std::runtime_error("Ошибка чтения журнала.");
    ZipFile zip(destination);
    zip.add("run.json", RunLog::redactText(QString::fromUtf8(QJsonDocument(metadata).toJson(QJsonDocument::Indented))).toUtf8());
    zip.add("journal.log", (truncated ? QByteArray("[Included the last 8 MiB of the log]\n") : QByteArray())
        + RunLog::redactText(QString::fromUtf8(logBytes)).toUtf8());
    QSet<QString> included;
    QStringList omitted;
    const QRegularExpression filename("^failure-[0-9]{4,}\\.png$");
    for (const auto &value : metadata.value("failures").toArray()) {
        const QString name = value.toObject().value("screenshot").toString();
        if (name.isEmpty() || included.contains(name)) continue;
        if (!filename.match(name).hasMatch()) throw std::runtime_error("Некорректное имя снимка в диагностике.");
        QByteArray png;
        try {
            png = readFile(folder.filePath(name), kImageLimit);
            if (!png.startsWith("\x89PNG\r\n\x1a\n")) throw std::runtime_error("Некорректный файл снимка страницы.");
        } catch (const std::exception &error) {
            omitted.append(name + ": " + QString::fromUtf8(error.what()));
            continue;
        }
        zip.add(("screenshots/" + name).toUtf8(), png);
        included.insert(name);
    }
    if (!omitted.isEmpty()) zip.add("export-notes.txt", RunLog::redactText(
        "Некоторые снимки отсутствуют или недоступны; остальные данные сохранены.\n\n" + omitted.join('\n')).toUtf8());
    zip.add("README.txt", QStringLiteral(
        "PR-Bot — диагностика запуска\n\n"
        "run.json — версии программы/Qt/Chrome/ChromeDriver, адрес, этап и описание каждой ошибки.\n"
        "journal.log — журнал именно этого запуска (для больших файлов — последние 8 МБ).\n"
        "screenshots/ — снимки видимой области страницы при ошибках.\n"
        "Если браузер недоступен, причина отсутствия снимка указана в run.json.\n"
        "Поля ввода скрываются перед снимком; известные пароли маскируются в тексте страницы.\n"
        "Снимки показывают остальное видимое содержимое форума.\n"
        "Если часть PNG недоступна, подробности будут в export-notes.txt.\n"
        "Архив содержит снимок данных на момент экспорта; время в run.json указано в UTC.\n").toUtf8());
    zip.finish();
}
