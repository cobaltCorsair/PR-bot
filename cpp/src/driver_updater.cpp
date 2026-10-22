#include "driver_updater.h"

#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QVersionNumber>
#include <utility>

namespace {
QString versionIn(const QByteArray &output)
{
    static const QRegularExpression pattern(R"(\b(\d+\.\d+\.\d+\.\d+)\b)");
    return pattern.match(QString::fromUtf8(output)).captured(1);
}

QString platformName()
{
    // currentCpuArchitecture учитывает архитектуру самого приложения, в том
    // числе запуск x64-сборки через Rosetta на Mac с Apple Silicon.
    const QString cpu = QSysInfo::currentCpuArchitecture();
#if defined(Q_OS_MACOS)
    if (cpu == "arm64") return "mac-arm64";
    if (cpu == "x86_64") return "mac-x64";
#elif defined(Q_OS_WIN)
    if (cpu == "x86_64") return "win64";
    if (cpu == "i386") return "win32";
#elif defined(Q_OS_LINUX)
    if (cpu == "arm64") return "linux-arm64";
    if (cpu == "x86_64") return "linux64";
#endif
    return {};
}

QString chromeExecutable()
{
#ifdef Q_OS_MACOS
    for (const QString &prefix : {QString("/Applications"), QDir::home().filePath("Applications")}) {
        const QString path = prefix + "/Google Chrome.app/Contents/MacOS/Google Chrome";
        if (QFileInfo(path).isExecutable()) return path;
    }
#elif defined(Q_OS_WIN)
    for (const char *variable : {"PROGRAMFILES", "PROGRAMFILES(X86)", "LOCALAPPDATA"}) {
        const QString prefix = qEnvironmentVariable(variable);
        if (prefix.isEmpty()) continue;
        const QString path = QDir(prefix).filePath("Google/Chrome/Application/chrome.exe");
        if (QFileInfo(path).isExecutable()) return path;
    }
#endif
    for (const QString &name : {QString("google-chrome"), QString("google-chrome-stable"),
                                QString("chromium"), QString("chromium-browser"), QString("chrome")}) {
        const QString path = QStandardPaths::findExecutable(name);
        if (!path.isEmpty()) return path;
    }
    return {};
}

QString driverFileName()
{
#ifdef Q_OS_WIN
    return "chromedriver.exe";
#else
    return "chromedriver";
#endif
}

constexpr qint64 kArchiveLimit = 64 * 1024 * 1024;
constexpr qint64 kExecutableLimit = 100 * 1024 * 1024;
}

DriverUpdater::DriverUpdater(QObject *parent) : QObject(parent), m_network(new QNetworkAccessManager(this))
{
}

DriverUpdater::~DriverUpdater()
{
    // Все операции, кроме короткой атомарной записи, асинхронны. Закрытие
    // окна отменяет загрузку; проверенный старый драйвер остаётся на месте.
    ++m_generation;
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(1000);
    }
    releaseFiles();
}

QString DriverUpdater::localDriverPath()
{
#ifdef Q_OS_WIN
    // В готовом ZIP нет дерева исходников и Conda-окружения.
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("drivers/" + driverFileName());
#else
    return QDir::current().absoluteFilePath("cpp/.env/bin/" + driverFileName());
#endif
}

bool DriverUpdater::isKnownIncompatible(const QString &path) const
{
    if (path != m_checkedPath || m_chromeVersion.isEmpty() || m_driverVersion.isEmpty())
        return false;
    // Подобранный через официальный milestone fallback драйвер может иметь
    // другой BUILD. Принимаем именно эту версию, а не любой одинаковый major.
    if (m_driverVersion == m_candidateVersion && m_chromeVersion == m_candidateChromeVersion)
        return false;
    return m_driverVersion.section('.', 0, 2) != m_chromeVersion.section('.', 0, 2);
}

void DriverUpdater::clear()
{
    if (m_busy) return;
    m_checkedPath.clear();
    m_chromeVersion.clear();
    m_driverVersion.clear();
    m_candidateVersion.clear();
    m_candidateChromeVersion.clear();
    m_downloadUrl.clear();
    m_status.clear();
    emit changed();
}

void DriverUpdater::setStatus(const QString &message)
{
    m_status = message;
    emit changed();
}

void DriverUpdater::finish(const QString &message, bool error)
{
    m_busy = false;
    releaseFiles();
    setStatus(message);
    emit logMessage(message, error);
}

void DriverUpdater::releaseFiles()
{
    m_temporary.reset();
    m_installLock.reset();
}

void DriverUpdater::cancel()
{
    if (!m_busy) return;
    ++m_generation; // Запоздалые сигналы отменённой операции игнорируются.
    if (m_reply) {
        m_reply->abort();
        m_reply = nullptr;
    }
    if (m_process) {
        m_process->kill();
        m_process->waitForFinished(1000);
        m_process = nullptr;
    }
    finish("Обновление отменено. Установленный драйвер сохранён.");
}

void DriverUpdater::runCommand(const QString &program, const QStringList &arguments,
                               int timeoutMs, CommandResult done, const QString &outputFile)
{
    auto *process = new QProcess(this);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    m_process = process;
    const quint64 generation = m_generation;
    if (!outputFile.isEmpty()) process->setStandardOutputFile(outputFile);
    auto completed = std::make_shared<bool>(false);
    auto complete = [this, process, timer, generation, completed, done](bool ok, const QString &error) {
        if (std::exchange(*completed, true)) return;
        timer->stop();
        const QByteArray output = process->readAllStandardOutput();
        const QString details = QString::fromUtf8(process->readAllStandardError()).trimmed().left(500);
        if (m_process == process) m_process = nullptr;
        process->deleteLater();
        if (generation == m_generation)
            done(ok, output, error.isEmpty() ? details : error);
    };
    connect(process, &QProcess::finished, this, [process, complete](int code, QProcess::ExitStatus status) {
        const bool timeout = process->property("timedOut").toBool();
        complete(!timeout && code == 0 && status == QProcess::NormalExit,
                 timeout ? "Превышено время ожидания команды." : QString());
    });
    connect(process, &QProcess::errorOccurred, this, [process, complete](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) complete(false, process->errorString());
    });
    connect(timer, &QTimer::timeout, process, [process] {
        process->setProperty("timedOut", true);
        process->kill();
    });
    process->start(program, arguments);
    timer->start(timeoutMs);
}

void DriverUpdater::readChromeVersion(std::function<void(const QString &)> done)
{
    const QString chrome = chromeExecutable();
    if (chrome.isEmpty()) {
        finish("Chrome не найден. Установите Google Chrome и повторите проверку.", true);
        return;
    }
#ifdef Q_OS_WIN
    // chrome.exe --version на Windows может открыть окно вместо вывода версии.
    // PowerShell читает версию файла, сам браузер не запускается.
    const QString script = "(Get-Item -LiteralPath '" + QString(chrome).replace('\'', "''")
        + "').VersionInfo.ProductVersion";
    const QString program = QStandardPaths::findExecutable("powershell.exe");
    const QStringList args = {"-NoProfile", "-NonInteractive", "-Command", script};
#else
    const QString program = chrome;
    const QStringList args = {"--version"};
#endif
    runCommand(program, args, 5000, [this, done](bool ok, const QByteArray &output, const QString &error) {
        const QString version = versionIn(output);
        if (!ok || version.isEmpty()) {
            finish("Не удалось прочитать версию Chrome. " + error, true);
            return;
        }
        done(version);
    });
}

void DriverUpdater::check(const QString &driverPath)
{
    if (m_busy) return;
    clear();
    ++m_generation;
    m_checkedPath = driverPath;
    m_platform = platformName();
    if (m_platform.isEmpty()) {
        finish("Автообновление не поддерживает эту ОС или архитектуру.", true);
        return;
    }
    m_busy = true;
    setStatus("Определяю версии Chrome и ChromeDriver…");
    readChromeVersion([this](const QString &version) {
        m_chromeVersion = version;
        emit changed();
        emit logMessage("Обнаружен Chrome " + version + ".", false);
        if (version.section('.', 0, 0).toInt() < 115) {
            finish("Для автоматического подбора нужен Chrome 115 или новее.", true);
            return;
        }
        if (!QFileInfo(m_checkedPath).isExecutable()) {
            requestRelease();
            return;
        }
        runCommand(m_checkedPath, {"--version"}, 5000,
                   [this](bool ok, const QByteArray &output, const QString &) {
            if (ok && output.startsWith("ChromeDriver ")) m_driverVersion = versionIn(output);
            emit changed();
            emit logMessage(m_driverVersion.isEmpty() ? "Версия выбранного ChromeDriver не определена."
                : "Обнаружен ChromeDriver " + m_driverVersion + ".", false);
            requestRelease();
        });
    });
}

void DriverUpdater::fetch(const QUrl &url, qint64 limit, DownloadResult done, bool archive)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setTransferTimeout(20000);
    auto *reply = m_network->get(request);
    reply->setReadBufferSize(64 * 1024);
    m_reply = reply;
    const quint64 generation = m_generation;
    auto data = std::make_shared<QByteArray>();
    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, reply, [reply] {
        reply->setProperty("timedOut", true);
        reply->abort();
    });
    timer->start(archive ? 120000 : 30000);
    auto read = [reply, data, limit] {
        data->append(reply->readAll());
        if (data->size() > limit) {
            reply->setProperty("tooLarge", true);
            reply->abort();
        }
    };
    connect(reply, &QIODevice::readyRead, this, read);
    if (archive) connect(reply, &QNetworkReply::downloadProgress, this,
                         [this, generation](qint64 received, qint64 total) {
        if (generation == m_generation && total > 0)
            setStatus(QString("Загружаю ChromeDriver… %1% (%2 МБ)")
                .arg(received * 100 / total).arg(received / 1048576.0, 0, 'f', 1));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, timer, generation, data, read, done] {
        timer->stop();
        read();
        if (m_reply == reply) m_reply = nullptr;
        reply->deleteLater();
        if (generation != m_generation) return;
        if (reply->property("tooLarge").toBool()) {
            finish("Ответ сервера превысил допустимый размер. Установленный драйвер сохранён.", true);
        } else if (reply->property("timedOut").toBool()) {
            finish("Сервер обновлений не ответил вовремя. Попробуйте ещё раз.", true);
        } else if (reply->error() != QNetworkReply::NoError
                   || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
            finish("Не удалось получить обновление: " + reply->errorString(), true);
        } else {
            done(*data);
        }
    });
}

void DriverUpdater::requestRelease(bool milestone)
{
    setStatus("Ищу совместимый драйвер в каталоге Google…");
    const QString endpoint = milestone ? "latest-versions-per-milestone-with-downloads.json"
                                      : "latest-patch-versions-per-build-with-downloads.json";
    fetch(QUrl("https://googlechromelabs.github.io/chrome-for-testing/" + endpoint), 16 * 1024 * 1024,
          [this, milestone](const QByteArray &data) {
        QJsonParseError error{};
        const auto document = QJsonDocument::fromJson(data, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            finish("Каталог Google вернул некорректные данные. Повторите проверку позже.", true);
            return;
        }
        const QString key = m_chromeVersion.section('.', 0, milestone ? 0 : 2);
        const auto record = document.object().value(milestone ? "milestones" : "builds")
                                .toObject().value(key).toObject();
        const QString version = record.value("version").toString();
        const auto downloads = record.value("downloads").toObject().value("chromedriver").toArray();
        QUrl url;
        for (const auto &entry : downloads) {
            const auto item = entry.toObject();
            if (item.value("platform").toString() == m_platform)
                url = QUrl(item.value("url").toString());
        }
        if ((record.isEmpty() || downloads.isEmpty()) && !milestone) {
            // Рекомендованный Google fallback, когда новый BUILD ещё не внесён.
            requestRelease(true);
            return;
        }
        if (versionIn(version.toUtf8()) != version
            || version.section('.', 0, 0) != m_chromeVersion.section('.', 0, 0)
            || (!milestone && version.section('.', 0, 2) != key) || url.isEmpty()) {
            finish("Для Chrome " + m_chromeVersion + " пока нет драйвера для этой платформы. Повторите позже.", true);
            return;
        }
        const QUrl expected(QString("https://storage.googleapis.com/chrome-for-testing-public/%1/%2/chromedriver-%2.zip")
                                .arg(version, m_platform));
        if (url != expected) {
            finish("Каталог вернул неожиданный адрес архива. Обновление остановлено.", true);
            return;
        }
        m_candidateChromeVersion = m_chromeVersion;
        const bool sameBuild = m_driverVersion.section('.', 0, 2) == m_chromeVersion.section('.', 0, 2);
        const bool installedIsNewer = !m_driverVersion.isEmpty()
            && QVersionNumber::fromString(m_driverVersion) > QVersionNumber::fromString(version);
        if (version == m_driverVersion || (sameBuild && installedIsNewer)) {
            // Храним версию также для проверки совместимости milestone fallback.
            m_candidateVersion = version;
            m_downloadUrl.clear();
            finish("ChromeDriver " + m_driverVersion + " подходит установленному Chrome. Обновление не требуется.");
            return;
        }
        m_candidateVersion = version;
        m_downloadUrl = url;
        finish("Доступен ChromeDriver " + version + ". Нажмите «Установить», чтобы обновить драйвер.");
    });
}

void DriverUpdater::install()
{
    if (m_busy || m_downloadUrl.isEmpty() || m_candidateVersion.isEmpty()) return;
    ++m_generation;
    m_busy = true;
    setStatus("Повторно проверяю версию Chrome…");
    readChromeVersion([this](const QString &version) {
        if (version != m_candidateChromeVersion) {
            m_chromeVersion = version;
            m_candidateVersion.clear();
            m_downloadUrl.clear();
            finish("Chrome обновился после проверки. Нажмите «Проверить обновления» ещё раз.", true);
            return;
        }
        const QString directory = QFileInfo(localDriverPath()).absolutePath();
        if (!QDir().mkpath(directory)) {
            finish("Не удалось создать каталог драйвера: " + directory, true);
            return;
        }
        m_installLock = std::make_unique<QLockFile>(directory + "/.chromedriver-update.lock");
        if (!m_installLock->tryLock(0)) {
            finish("Драйвер уже обновляется в другом окне. Повторите позже.", true);
            return;
        }
        m_temporary = std::make_unique<QTemporaryDir>();
        if (!m_temporary->isValid()) {
            finish("Не удалось создать временный каталог для обновления.", true);
            return;
        }
        downloadDriver();
    });
}

void DriverUpdater::downloadDriver()
{
    setStatus("Загружаю ChromeDriver " + m_candidateVersion + "…");
    emit logMessage("Загрузка ChromeDriver " + m_candidateVersion + " с официального сервера Google.", false);
    fetch(m_downloadUrl, kArchiveLimit, [this](const QByteArray &data) {
        QFile archive(m_temporary->filePath("driver.zip"));
        if (!archive.open(QIODevice::WriteOnly) || archive.write(data) != data.size()) {
            finish("Не удалось сохранить загруженный архив. Установленный драйвер сохранён.", true);
            return;
        }
        archive.close();
        extractDriver();
    }, true);
}

void DriverUpdater::extractDriver()
{
    setStatus("Распаковываю ChromeDriver…");
    const QString archive = m_temporary->filePath("driver.zip");
    const QString member = "chromedriver-" + m_platform + '/' + driverFileName();
    const QString extracted = m_temporary->filePath(driverFileName());
#ifdef Q_OS_WIN
    const QString program = QStandardPaths::findExecutable("tar.exe");
    const QStringList arguments = {"-xOf", archive, member};
#else
    const QString program = QStandardPaths::findExecutable("unzip");
    const QStringList arguments = {"-p", archive, member};
#endif
    if (program.isEmpty()) {
        finish("Не найдена программа распаковки ZIP (unzip / tar.exe). Установленный драйвер сохранён.", true);
        return;
    }
    // Из архива извлекается только один ожидаемый файл, без путей из ZIP.
    runCommand(program, arguments, 30000, [this, extracted](bool ok, const QByteArray &, const QString &error) {
        const QFileInfo file(extracted);
        if (!ok || file.size() <= 0 || file.size() > kExecutableLimit
            || !QFile::setPermissions(extracted, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                                      | QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther)) {
            finish("Не удалось распаковать ChromeDriver. " + error, true);
            return;
        }
        setStatus("Проверяю загруженный исполняемый файл…");
        runCommand(extracted, {"--version"}, 5000,
                   [this](bool valid, const QByteArray &output, const QString &details) {
            if (!valid || !output.startsWith("ChromeDriver ") || versionIn(output) != m_candidateVersion) {
                finish("Проверка нового ChromeDriver не пройдена. Старый файл сохранён. " + details, true);
                return;
            }
            // Chrome может автоматически обновиться, пока скачивается архив.
            // Повторная проверка не даёт заменить драйвер уже устаревшей парой.
            readChromeVersion([this](const QString &version) {
                if (version != m_candidateChromeVersion) {
                    m_chromeVersion = version;
                    m_candidateVersion.clear();
                    m_downloadUrl.clear();
                    finish("Chrome обновился во время загрузки. Старый драйвер сохранён; повторите проверку.", true);
                    return;
                }
                commitDriver();
            });
        });
    }, extracted);
}

void DriverUpdater::commitDriver()
{
    QFile source(m_temporary->filePath(driverFileName()));
    QSaveFile destination(localDriverPath());
    // Прямую запись запрещаем: обрыв/ошибка записи не повреждает старый файл.
    destination.setDirectWriteFallback(false);
    if (!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly)) {
        source.close();
        finish("Не удалось подготовить замену драйвера. Старый файл сохранён.", true);
        return;
    }
    if (!destination.setPermissions(source.permissions())) {
        source.close();
        destination.cancelWriting();
        finish("Не удалось задать права новому драйверу. Старый файл сохранён.", true);
        return;
    }
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(1024 * 1024);
        if (chunk.isEmpty() || destination.write(chunk) != chunk.size()) {
            source.close();
            destination.cancelWriting();
            finish("Не удалось записать новый драйвер. Старый файл сохранён.", true);
            return;
        }
    }
    source.close();
    if (!destination.commit()) {
        finish("Не удалось заменить драйвер. Закройте другие экземпляры бота и повторите установку. "
               + destination.errorString(), true);
        return;
    }
    m_driverVersion = m_candidateVersion;
    m_checkedPath = localDriverPath();
    m_downloadUrl.clear();
    const QString path = m_checkedPath;
    finish("Установлен ChromeDriver " + m_driverVersion + ". Драйвер выбран для этой кампании.");
    emit installed(path);
}
