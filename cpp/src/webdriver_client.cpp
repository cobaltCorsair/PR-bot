#include "webdriver_client.h"

#include <QEventLoop>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpSocket>
#include <QTcpServer>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <stdexcept>
#include <utility>

namespace {
WebDriverError driverError(const QString &message)
{
    return WebDriverError(WebDriverError::Kind::Startup, message);
}
}

WebDriverError::WebDriverError(Kind kind, const QString &message, QString code)
    : std::runtime_error(message.toStdString()), m_kind(kind), m_code(std::move(code))
{
}

WebDriverClient::WebDriverClient(QString driverExecutable)
    : m_driverExecutable(std::move(driverExecutable))
{
}

WebDriverClient::~WebDriverClient()
{
    quit();
}

void WebDriverClient::start()
{
    if (m_driverExecutable.isEmpty())
        throw driverError("ChromeDriver не найден. Укажите путь к исполняемому файлу.");

    // У каждого прохода свой локальный порт: второй запуск не подключится
    // к чужому драйверу на стандартном 9515.
    QTcpServer reservation;
    if (!reservation.listen(QHostAddress::LocalHost, 0))
        throw driverError("Не удалось выделить локальный порт для ChromeDriver.");
    m_port = reservation.serverPort();
    m_serverUrl = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_port));
    reservation.close();

    // Объекты Qt Network создаются здесь, уже в том потоке, где идут запросы.
    m_network = std::make_unique<QNetworkAccessManager>();
    m_driverProcess = std::make_unique<QProcess>();
    m_driverProcess->setProcessChannelMode(QProcess::MergedChannels);
    m_driverProcess->start(m_driverExecutable, {QStringLiteral("--port=%1").arg(m_port)});
    if (!m_driverProcess->waitForStarted(5000))
        throw driverError(QString("Не удалось запустить ChromeDriver: %1")
                              .arg(m_driverProcess->errorString()));
    waitForServer();

    const QJsonObject chromeOptions{
        {"args", QJsonArray{
            "--headless=new",
            "--disable-gpu",
            "--no-first-run",
            "--autoplay-policy=no-user-gesture-required",
            "--blink-settings=imagesEnabled=false",
        }},
        {"excludeSwitches", QJsonArray{"enable-logging"}},
    };
    const QJsonObject capabilities{
        {"alwaysMatch", QJsonObject{
             {"browserName", "chrome"},
             {"pageLoadStrategy", "eager"}, // Ждём DOM, не загрузку всех картинок.
             {"goog:chromeOptions", chromeOptions},
         }},
    };
    const QJsonValue response = request("POST", "/session", {{"capabilities", capabilities}});
    const QJsonObject root = response.toObject();
    m_sessionId = root.value("sessionId").toString();
    if (m_sessionId.isEmpty())
        m_sessionId = root.value("value").toObject().value("sessionId").toString();
    if (m_sessionId.isEmpty())
        throw driverError("ChromeDriver не вернул идентификатор браузерной сессии.");
    const QJsonObject actualCapabilities = root.value("capabilities").toObject();
    m_browserVersion = actualCapabilities.value("browserVersion").toString("не сообщена");
    const QString driverVersion = actualCapabilities.value("chrome").toObject()
                                      .value("chromedriverVersion").toString();
    m_driverVersion = driverVersion.isEmpty() ? "не сообщена" : driverVersion.section(' ', 0, 0);
    request("POST", sessionPath("/timeouts"),
            {{"implicit", 0}, {"pageLoad", 10000}, {"script", 10000}});
}

void WebDriverClient::quit()
{
    if (!m_sessionId.isEmpty() && m_network) {
        try {
            request("DELETE", sessionPath());
        } catch (...) {
            // Очистка не должна скрыть первоначальную ошибку сценария или
            // выбросить исключение из деструктора. Процесс завершается ниже.
        }
    }
    m_sessionId.clear();

    if (m_driverProcess) {
        if (m_driverProcess->state() != QProcess::NotRunning) {
            m_driverProcess->terminate();
            if (!m_driverProcess->waitForFinished(1500)) {
                m_driverProcess->kill();
                m_driverProcess->waitForFinished(1500);
            }
        }
        m_driverProcess.reset();
    }
    m_network.reset();
}

void WebDriverClient::navigate(const QString &url)
{
    request("POST", sessionPath("/url"), {{"url", url}});
}

QString WebDriverClient::currentUrl(int timeoutMs)
{
    return request("GET", sessionPath("/url"), {}, timeoutMs).toString();
}

QByteArray WebDriverClient::takeScreenshot(const QStringList &hiddenValues)
{
    // Не трогаем значения форм и не вызываем события input/change/submit.
    // Временный стиль скрывает поля; известные секреты маскируются в тексте.
    const QString key = "__prbotDiagnostic_" + QUuid::createUuid().toString(QUuid::Id128);
    QJsonArray secrets;
    for (const auto &value : hiddenValues) if (!value.isEmpty()) secrets.append(value);
    const QString restore = "if (typeof window[arguments[0]] === 'function') window[arguments[0]](); return true;";
    const auto restorePage = [&] {
        try { request("POST", sessionPath("/execute/sync"), {{"script", restore}, {"args", QJsonArray{key}}}, 2500); }
        catch (...) { /* Закрытая/перешедшая страница уже не содержит временной маски. */ }
    };
    try {
        request("POST", sessionPath("/execute/sync"), {{"script", QStringLiteral(R"JS(
            const key = arguments[0], secrets = arguments[1];
            const changed = [];
            const style = document.createElement('style');
            style.textContent = 'input, textarea, [contenteditable] { visibility: hidden !important; }';
            window[key] = function () {
                style.remove();
                for (const [node, original, masked] of changed)
                    if (node.isConnected && node.nodeValue === masked) node.nodeValue = original;
                delete window[key];
            };
            (document.head || document.documentElement).appendChild(style);
            const walker = document.createTreeWalker(document.body || document.documentElement, NodeFilter.SHOW_TEXT);
            let node;
            while ((node = walker.nextNode())) {
                if (node.parentElement && node.parentElement.closest('script, style, noscript, input, textarea')) continue;
                const original = node.nodeValue;
                let masked = original;
                for (const secret of secrets) masked = masked.split(secret).join('[скрыто]');
                if (masked !== original) { changed.push([node, original, masked]); node.nodeValue = masked; }
            }
            return true;
        )JS")}, {"args", QJsonArray{key, secrets}}}, 2500);
        const QByteArray encoded = request("GET", sessionPath("/screenshot"), {}, 3500).toString().toLatin1();
        const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || !decoded.decoded.startsWith("\x89PNG\r\n\x1a\n")
            || decoded.decoded.size() > 16 * 1024 * 1024)
            throw std::runtime_error("ChromeDriver вернул некорректный или слишком большой снимок страницы.");
        restorePage();
        return decoded.decoded;
    } catch (...) {
        restorePage();
        throw;
    }
}

QJsonValue WebDriverClient::execute(const QString &script, const QJsonArray &arguments)
{
    return request("POST", sessionPath("/execute/sync"),
                   {{"script", script}, {"args", arguments}});
}

QStringList WebDriverClient::windowHandles()
{
    const QJsonArray handles = request("GET", sessionPath("/window/handles")).toArray();
    QStringList result;
    for (const QJsonValue &handle : handles)
        result.append(handle.toString());
    return result;
}

void WebDriverClient::switchToWindow(const QString &handle)
{
    request("POST", sessionPath("/window"), {{"handle", handle}});
}

void WebDriverClient::openTab()
{
    request("POST", sessionPath("/window/new"), {{"type", "tab"}});
}

QString WebDriverClient::browserVersion() const
{
    return m_browserVersion;
}

QString WebDriverClient::driverVersion() const
{
    return m_driverVersion;
}

QJsonValue WebDriverClient::waitFor(const std::function<QJsonValue()> &probe,
                                   const QString &description, int timeoutMs,
                                   const std::function<void()> &checkpoint)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (true) {
        if (checkpoint)
            checkpoint();
        const QJsonValue value = probe();
        if (!value.isNull() && !value.isUndefined()
            && (!value.isBool() || value.toBool())
            && (!value.isString() || !value.toString().isEmpty()))
            return value;
        if (elapsed.elapsed() >= timeoutMs)
            throw WebDriverError(WebDriverError::Kind::Timeout,
                QString("Истекло время ожидания: %1 (лимит %2 мс).")
                    .arg(description).arg(timeoutMs), "timeout");
        QThread::msleep(100);
    }
}

QJsonValue WebDriverClient::request(const QByteArray &method, const QString &path,
                                    const QJsonObject &body, int timeoutMs)
{
    if (!m_network)
        throw driverError("Браузерная сессия ещё не запущена.");

    // В диагностике показываем команду, но не её JSON: args могут содержать
    // пароль или полный рекламный код. ID сессии для поиска ошибки не нужен.
    QString commandPath = path;
    if (!m_sessionId.isEmpty())
        commandPath.replace("/session/" + m_sessionId, "/session/<id>");
    const QString command = QString::fromLatin1(method) + ' ' + commandPath;
    const QUrl url = m_serverUrl.resolved(QUrl(path));
    QNetworkRequest networkRequest(url);
    networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=utf-8");

    QNetworkReply *reply = nullptr;
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    if (method == "GET")
        reply = m_network->get(networkRequest);
    else if (method == "POST")
        reply = m_network->post(networkRequest, payload);
    else
        reply = m_network->sendCustomRequest(networkRequest, method, payload);

    // Локальный event loop обслуживает Qt Network, сохраняя последовательный
    // сценарий. Общий таймер ограничивает и зависший локальный ChromeDriver.
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    if (timer.isActive())
        timer.stop();
    else if (!reply->isFinished()) {
        reply->abort();
        reply->deleteLater();
        throw WebDriverError(WebDriverError::Kind::Timeout,
            QString("Истекло время ожидания ответа ChromeDriver (%1, лимит %2 мс).")
                .arg(command).arg(timeoutMs), "timeout");
    }

    const QByteArray responseBytes = reply->readAll();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorText = reply->errorString();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(responseBytes, &parseError);
    if (networkError != QNetworkReply::NoError && responseBytes.isEmpty())
        throw WebDriverError(WebDriverError::Kind::Connection,
            QString("Ошибка связи с ChromeDriver (%1): %2").arg(command, networkErrorText));
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        throw WebDriverError(WebDriverError::Kind::Protocol,
            QString("ChromeDriver вернул некорректный ответ (%1, HTTP %2).")
                .arg(command).arg(status));

    const QJsonObject root = document.object();
    if (status >= 400 || root.value("value").toObject().contains("error")) {
        const QJsonObject value = root.value("value").toObject();
        const QString message = value.value("message").toString(
            value.value("error").toString(QStringLiteral("HTTP %1").arg(status)));
        const QString code = value.value("error").toString();
        const auto kind = (code == "timeout" || code == "script timeout")
            ? WebDriverError::Kind::Timeout : WebDriverError::Kind::Browser;
        throw WebDriverError(kind, QString("Ошибка WebDriver (%1, HTTP %2): %3")
                                      .arg(command).arg(status).arg(message), code);
    }
    // W3C оборачивает успешный результат любой команды в поле value.
    return root.value("value");
}

QString WebDriverClient::sessionPath(const QString &suffix) const
{
    return QStringLiteral("/session/%1%2").arg(m_sessionId, suffix);
}

void WebDriverClient::waitForServer()
{
    // Запущенный процесс ещё не означает, что локальный HTTP-порт уже готов.
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (m_driverProcess->state() == QProcess::NotRunning)
            throw driverError(QString("ChromeDriver завершился при запуске: %1")
                                  .arg(QString::fromLocal8Bit(m_driverProcess->readAll())));
        QTcpSocket socket;
        socket.connectToHost(QStringLiteral("127.0.0.1"), m_port);
        if (socket.waitForConnected(150)) {
            socket.disconnectFromHost();
            return;
        }
        QThread::msleep(100);
    }
    throw driverError(QString("ChromeDriver не открыл локальный порт %1 за 5 секунд.").arg(m_port));
}
