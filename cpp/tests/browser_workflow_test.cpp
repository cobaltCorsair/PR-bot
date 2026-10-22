#include "../src/bot_window.h"
#include "../src/forum_page.h"
#include "../src/pr_bot.h"
#include "../src/run_state.h"
#include "../src/webdriver_client.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <iostream>
#include <functional>
#include <stdexcept>

namespace {
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

QString readFile(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read test report.");
    return QString::fromUtf8(file.readAll());
}

struct Post { int id; int author; QString message; };
struct Site {
    int user = 0;
    int logins = 0;
    int searches = 0;
    int profiles = 0;
    int attempts = 0;
    int requests = 0;
    bool reject = false;
    bool silent = false;
    bool wrongAuthor = false;
    bool firstAccountEmpty = false;
    bool delayed = false;
    bool partnership = false;
    bool closed = false;
    QString lastPageLink;
    QString templateOverride;
    QString messageOverride;
    QList<Post> posts;
};

// Сайты существуют только на 127.0.0.1. Сервер воспроизводит DOM MyBB,
// PR-вход и редирект после POST; ни один сценарий не обращается к форумам.
class Fixtures {
public:
    Fixtures()
    {
        require(server.listen(QHostAddress::LocalHost, 0), "Cannot start local fixture server.");
        QObject::connect(&server, &QTcpServer::newConnection, [&] {
            while (server.hasPendingConnections()) {
                QTcpSocket *socket = server.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray bytes = socket->property("buffer").toByteArray() + socket->readAll();
                    socket->setProperty("buffer", bytes);
                    const int headerEnd = bytes.indexOf("\r\n\r\n");
                    if (headerEnd < 0 || socket->property("handled").toBool())
                        return;
                    int contentLength = 0;
                    const auto lines = bytes.left(headerEnd).split('\n');
                    for (const auto &line : lines) {
                        if (line.toLower().startsWith("content-length:"))
                            contentLength = line.mid(15).trimmed().toInt();
                    }
                    if (bytes.size() < headerEnd + 4 + contentLength)
                        return;
                    socket->setProperty("handled", true);
                    route(socket, lines.first().split(' ').value(1),
                          bytes.mid(headerEnd + 4, contentLength));
                });
            }
        });
    }

    QString root(const QString &site) const
    {
        return QString("http://127.0.0.1:%1/%2").arg(server.serverPort()).arg(site);
    }
    QString topic(const QString &site) const { return root(site) + "/viewtopic.php?id=1"; }
    QString message(const QString &site) const
    {
        return "[url=" + root(site) + "][img]" + root(site) + "/pixel.svg[/img][/url]";
    }

    Site source;
    Site target;
    Site other;
    std::function<void(const QString &)> onSubmitted;
    std::function<void(const QString &)> onPage;

private:
    static void respond(QTcpSocket *socket, const QByteArray &body, const QByteArray &extra = {},
                        const QByteArray &status = "200 OK")
    {
        socket->write("HTTP/1.1 " + status + "\r\nContent-Type: text/html; charset=utf-8\r\n"
                      "Content-Length: " + QByteArray::number(body.size())
                      + "\r\nConnection: close\r\n" + extra + "\r\n" + body);
        socket->disconnectFromHost();
    }

    QString render(QString bbcode) const
    {
        bbcode = bbcode.toHtmlEscaped();
        bbcode.replace(QRegularExpression(R"(\[img\]([^\[]+)\[/img\])"),
                       "<img class=postimg src=\"\\1\">");
        bbcode.replace(QRegularExpression(R"(\[url=([^\]]+)\](.*?)\[/url\])",
                                           QRegularExpression::DotMatchesEverythingOption),
                       "<a href=\"\\1\">\\2</a>");
        bbcode.remove(QRegularExpression(R"(\[/?[a-z][^\]]*\])"));
        return bbcode;
    }

    QString postHtml(const QString &site, const Post &post, const QString &classes = {}) const
    {
        return QString("<div class='post %1' id='p%2'><h3><a href='%3#p%2'>%2</a></h3>"
                       "<div class=post-author><a href='%4/profile.php?id=%5'>PR</a></div>"
                       "<div class=post-content>%6</div></div>")
            .arg(classes).arg(post.id).arg(topic(site), root(site)).arg(post.author).arg(render(post.message));
    }

    QString page(const QString &site, const Site &state, QString content) const
    {
        return QString("<!doctype html><meta charset=utf-8><title>Локальный форум</title>"
                       "<script>var UserID=%1; var GroupID=%2; var PR={"
                       "in_1:function(){location.href='%3/login.php?action=in&account=1';},"
                       "in_2:function(){location.href='%3/login.php?action=in&account=2';}};</script>"
                       "<div id=pun-main>%4</div>")
            .arg(state.user).arg(state.user ? 2 : 3).arg(root(site), content);
    }

    QString topicPage(const QString &site, const Site &state) const
    {
        QString content = "<div id=topic>";
        QString pinned = postHtml(site, {1, 2, message(site)}, "topicpost");
        if (!state.partnership)
            pinned.replace("</div></div>", "<div class=code-box><pre>"
                + (state.templateOverride.isEmpty() ? message(site) : state.templateOverride).toHtmlEscaped() + "</pre></div></div></div>");
        content += pinned;
        content += postHtml(site, {2, site == "source" ? 9 : 17, message(site)}, "endpost");
        QString delayed;
        for (const auto &post : state.posts) {
            if (state.delayed)
                delayed += postHtml(site, post, "endpost");
            else
                content += postHtml(site, post, "endpost");
        }
        content += "</div><form method=post action='" + root(site) + "/post.php'>"
            "<textarea id=main-reply name=req_message></textarea>"
            "<input type=submit class='button submit' value=Отправить></form>";
        if (state.closed) content.remove(QRegularExpression("<form.*?</form>", QRegularExpression::DotMatchesEverythingOption));
        if (!state.lastPageLink.isEmpty()) content += "<div class=post-content><a href='" + state.lastPageLink + "'>Existing advertisement</a></div>";
        if (!delayed.isEmpty()) {
            // Асинхронное появление результата проверяет ожидание по DOM.
            const QByteArray json = QJsonDocument(QJsonArray{delayed}).toJson(QJsonDocument::Compact);
            content += "<script>setTimeout(function(){document.querySelector('#topic')"
                       ".insertAdjacentHTML('beforeend'," + QString::fromUtf8(json)
                + "[0]);},200);</script>";
        }
        return page(site, state, content);
    }

    void route(QTcpSocket *socket, const QByteArray &requestTarget, const QByteArray &body)
    {
        const QUrl url = QUrl::fromEncoded(requestTarget);
        const auto segments = url.path().split('/', Qt::SkipEmptyParts);
        const QString site = segments.value(0);
        if (site.endsWith("-alias")) {
            QString redirected = url.toString(); redirected.replace(site, site.left(site.size() - 6));
            respond(socket, {}, "Location: " + redirected.toUtf8() + "\r\n", "302 Found");
            return;
        }
        if (site != "source" && site != "target" && site != "other") {
            respond(socket, {}, {}, "204 No Content");
            return;
        }
        Site &state = site == "source" ? source : site == "target" ? target : other;
        ++state.requests;
        const QString endpoint = segments.value(1);
        const QUrlQuery query(url);
        if (endpoint == "login.php") {
            if (query.queryItemValue("action") == "out") {
                state.user = 0;
            } else {
                ++state.logins;
                state.user = state.firstAccountEmpty && query.queryItemValue("account") == "1"
                    ? 11 : site == "source" ? 9 : 17;
            }
            respond(socket, {}, "Location: " + root(site).toUtf8() + "/\r\n", "302 Found");
        } else if (endpoint == "search.php") {
            ++state.searches;
            QString results;
            if (state.user != 11) {
                // Разметка соответствует XPath последнего сообщения в PrBot.
                results = "<div></div><div><div><div><div></div><div></div><div><ul><li><a href='"
                    + topic(site) + "#p2'>Перейти к теме</a></li></ul></div></div></div></div>";
            }
            respond(socket, page(site, state, results).toUtf8());
        } else if (endpoint == "viewtopic.php") {
            respond(socket, topicPage(site, state).toUtf8());
        } else if (endpoint == "profile.php") {
            ++state.profiles;
            respond(socket, page(site, state, "Профиль").toUtf8());
        } else if (endpoint == "post.php") {
            ++state.attempts;
            if (state.reject) {
                respond(socket, page(site, state,
                    "<ul class=errorlist><li>Слишком быстро: сообщение не принято</li></ul>").toUtf8());
                return;
            }
            if (!state.silent) {
                const int id = (site == "source" ? 100 : 200) + int(state.posts.size()) + 1;
                const QString submitted = QUrlQuery(QString::fromUtf8(body).replace('+', ' '))
                                              .queryItemValue("req_message", QUrl::FullyDecoded);
                state.posts.append({id, state.wrongAuthor ? 999 : state.user,
                                    state.messageOverride.isEmpty() ? submitted : state.messageOverride});
                if (onSubmitted)
                    onSubmitted(site);
            }
            const QString fragment = state.posts.isEmpty() ? "p2" : "p" + QString::number(state.posts.last().id);
            respond(socket, {}, "Location: " + (topic(site) + '#' + fragment).toUtf8() + "\r\n", "302 Found");
        } else {
            respond(socket, page(site, state, "Главная").toUtf8());
            if (onPage)
                onPage(site);
        }
    }
    QTcpServer server;
};

BotConfig configuration(Fixtures &fixtures, const QString &driver, QTemporaryDir &directory,
                        bool twoForums = false)
{
    BotConfig config;
    config.threadUrl = fixtures.topic("source");
    config.prCode = "[b]Наша реклама[/b]";
    config.chromeDriverPath = driver;
    config.forumsFile = directory.filePath("forums.txt");
    QFile file(config.forumsFile);
    require(file.open(QIODevice::WriteOnly), "Cannot create local forum list.");
    file.write((fixtures.root("target") + '\n'
                + (twoForums ? fixtures.root("other") + '\n' : QString())).toUtf8());
    return config;
}

QString run(Fixtures &fixtures, const QString &driver,
            std::shared_ptr<RunControl> control = {}, bool twoForums = false)
{
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create local test directory.");
    PrBot bot(configuration(fixtures, driver, directory, twoForums), directory.filePath("log.txt"), control);
    bool finished = false;
    QObject::connect(&bot, &PrBot::finished, [&] { finished = true; });
    bot.run();
    require(finished, "Bot did not finish.");
    return readFile(directory.filePath("log.txt"));
}
QString execute(const BotConfig &config, const QTemporaryDir &directory)
{
    PrBot bot(config, directory.filePath("log.txt"));
    bot.run();
    return readFile(directory.filePath("log.txt"));
}

// Создаём состояние именно тем callback, который сохраняется до клика.
// Новый PrBot затем воспроизводит запуск после потери процесса/подтверждения.
void interruptedPair(Fixtures &fixture, const QString &driver, const QTemporaryDir &directory,
                     const BotConfig &config, bool secondAttempt, bool targetMissing = false, bool aliases = false)
{
    RunState state(RunState::pathForReport(directory.filePath("log.txt")));
    const QString target = fixture.root(aliases ? "target-alias" : "target");
    state.begin(config, {target});
    ForumProgress entry;
    entry.url = target; entry.topicUrl = fixture.topic("target"); entry.targetAuthor = "17";
    WebDriverClient browser(driver); browser.start();
    browser.navigate(fixture.root("source") + "/login.php?action=in");
    browser.navigate(fixture.topic("source"));
    ForumPage page(browser);
    const QString first = page.publish(fixture.message("target"), {}, {}, 15000, [&](const QJsonObject &proof) {
        entry.sourceProof = proof; entry.phase = "source_submitting"; state.store(entry);
    });
    if (secondAttempt) {
        entry.sourceLink = first;
        browser.navigate(fixture.root("target") + "/login.php?action=in");
        browser.navigate(fixture.topic("target"));
        fixture.target.silent = targetMissing;
        try {
            page.publish(config.prCode + " [url=" + first + "]ваша реклама[/url]", first, {},
                targetMissing ? 350 : 15000, [&](const QJsonObject &proof) {
                    entry.targetProof = proof; entry.phase = "target_submitting"; state.store(entry);
                });
        } catch (const PublicationError &error) {
            require(targetMissing && error.status() == PublicationError::Status::Unknown, "Unexpected seeded publication error");
        }
    }
    browser.quit();
    fixture.source.user = fixture.target.user = 0;
}

}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    if (argc != 2)
        return 2;
    const QString driver = QString::fromLocal8Bit(argv[1]);
    try {
        {
            Fixtures fixture;
            QTemporaryDir directory;
            auto config = configuration(fixture, driver, directory);
            config.checkOnly = true;
            const QString report = execute(config, directory);
            require(fixture.source.attempts == 0 && fixture.target.attempts == 0
                && report.contains("Готов к публикации")
                && !QFile::exists(directory.filePath("progress.json")), "Dry run posted or changed durable state");
            std::cout << "Read-only campaign check passed.\n";
        }
        {
            Fixtures fixture;
            QTemporaryDir directory;
            auto config = configuration(fixture, driver, directory, true);
            config.useForumTable = true;
            config.targets = {{fixture.root("target"), false}, {fixture.root("other"), false}};
            config.onlyForums = {fixture.root("target")};
            execute(config, directory);
            require(fixture.target.posts.size() == 1 && fixture.other.requests == 0, "Selective run visited an unselected forum");
            std::cout << "Selected forum override passed.\n";
        }
        {
            Fixtures fixture;
            QTemporaryDir directory;
            auto config = configuration(fixture, driver, directory);
            fixture.target.reject = true;
            execute(config, directory);
            RunState snapshot(directory.filePath("progress.json"));
            require(snapshot.load() && snapshot.entry(fixture.root("target")).phase == "source_confirmed", "Rejected second reply checkpoint missing");
            fixture.target.reject = false;
            fixture.source.closed = true;
            fixture.source.user = fixture.target.user = 0;
            config.resumeRun = true;
            const QString report = execute(config, directory);
            require(fixture.source.attempts == 1 && fixture.target.attempts == 2 && fixture.target.posts.size() == 1
                && report.contains("Успешно пройдено форумов: 1"), "Resume did not complete only the missing reply on a closed source topic");
            const int attempts = fixture.source.attempts + fixture.target.attempts;
            execute(config, directory);
            require(attempts == fixture.source.attempts + fixture.target.attempts, "Completed resume reposted");
            std::cout << "Rejected reply, closed source topic and completed resume passed.\n";
        }
        for (bool second : {false, true}) {
            Fixtures fixture;
            QTemporaryDir directory;
            auto config = configuration(fixture, driver, directory);
            interruptedPair(fixture, driver, directory, config, second);
            const QByteArray checkpoint = readFile(directory.filePath("progress.json")).toUtf8();
            config.checkOnly = true;
            execute(config, directory);
            require(readFile(directory.filePath("progress.json")).toUtf8() == checkpoint, "Dry run altered partial publication proof");
            config.checkOnly = false; config.resumeRun = true;
            fixture.source.firstAccountEmpty = true;
            const QString report = execute(config, directory);
            require(fixture.source.attempts == 1 && fixture.target.attempts == 1
                && report.contains("Успешно пройдено форумов: 1"), "Crash reconciliation duplicated or lost a publication");
            std::cout << "Interrupted phase " << (second ? "target" : "source") << " reconciled without duplicate.\n";
        }
        {
            Fixtures fixture;
            QTemporaryDir directory;
            auto config = configuration(fixture, driver, directory);
            interruptedPair(fixture, driver, directory, config, true, true);
            fixture.target.silent = false;
            config.resumeRun = true;
            const QString report = execute(config, directory);
            {
                RunState state(directory.filePath("progress.json")); state.load();
                require(fixture.source.attempts == 1 && fixture.target.attempts == 1
                    && state.entry(fixture.root("target")).review && report.contains("Автоматический повтор заблокирован"),
                    "Unknown target result was automatically resent");
                state.resolveReplyMissing(fixture.root("target"));
            }
            const QString resumed = execute(config, directory);
            require(fixture.source.attempts == 1 && fixture.target.attempts == 2
                && fixture.target.posts.size() == 1 && resumed.contains("Успешно пройдено форумов: 1"),
                "Manual target absence did not allow only the missing reply");
            std::cout << "Unknown result blocks resend; manual review allows missing reply.\n";
        }
        {
            Fixtures fixture;
            QTemporaryDir directory;
            auto config = configuration(fixture, driver, directory);
            config.threadUrl = fixture.topic("source-alias");
            config.useForumTable = true;
            config.targets = {{fixture.root("target-alias"), true}};
            interruptedPair(fixture, driver, directory, config, true, false, true);
            config.resumeRun = true;
            const QString report = execute(config, directory);
            require(fixture.source.attempts == 1 && fixture.target.attempts == 1
                && report.contains("Успешно пройдено форумов: 1"), "Verified forum redirects blocked recovery");
            std::cout << "Recovery through verified forum aliases passed.\n";
        }
        {
            Fixtures fixture;
            WebDriverClient browser(driver); browser.start();
            browser.navigate(fixture.root("source") + "/login.php?action=in");
            browser.navigate(fixture.topic("source"));
            bool failed = false;
            try { ForumPage(browser).publish("Public text", {}, {}, 350, [](const QJsonObject &) { throw std::runtime_error("disk full"); }); }
            catch (const std::exception &) { failed = true; }
            require(failed && fixture.source.attempts == 0, "Failed checkpoint still clicked submit");
            std::cout << "Checkpoint failure before click passed.\n";
        }
        {
            Fixtures fixture;
            fixture.target.lastPageLink = fixture.root("source") + ".evil.invalid";
            const QString report = run(fixture, driver);
            require(fixture.target.posts.size() == 1, "Similar host caused a false duplicate skip");
            std::cout << "Exact forum matching passed.\n";
        }
        {
            Fixtures fixture;
            fixture.source.delayed = fixture.target.delayed = true;
            const QString report = run(fixture, driver);
            require(fixture.source.posts.size() == 1 && fixture.target.posts.size() == 1,
                    "Reciprocal publication did not complete.");
            require(fixture.source.searches == 1 && fixture.target.searches == 1
                    && fixture.source.profiles == 0 && fixture.target.profiles == 0,
                    "Login performed duplicate searches or unnecessary profile navigation.");
            require(report.contains("Успешно пройдено форумов: 1")
                    && report.contains(fixture.topic("source") + "#p101")
                    && report.contains(fixture.topic("target") + "#p201"),
                    "Confirmed permalinks or success are absent from the report.");
            std::cout << "Delayed DOM confirmation and single account lookup passed.\n";
        }
        {
            Fixtures fixture;
            fixture.target.firstAccountEmpty = true;
            const QString report = run(fixture, driver);
            require(fixture.target.logins == 2 && fixture.target.searches == 2
                    && report.contains("Успешно пройдено форумов: 1"),
                    "Fallback to the second PR account failed.");
            std::cout << "Second PR account fallback passed.\n";
        }
        {
            Fixtures fixture;
            fixture.source.reject = true;
            const QString report = run(fixture, driver);
            require(fixture.source.attempts == 1 && fixture.target.attempts == 0
                    && report.contains("отклонил отправку")
                    && report.contains("Успешно пройдено форумов: 0"),
                    "A rejected first publication was treated as success or retried.");
            std::cout << "First publication rejection passed.\n";
        }
        {
            Fixtures fixture;
            fixture.target.reject = true;
            const QString report = run(fixture, driver);
            require(fixture.source.posts.size() == 1 && fixture.target.posts.isEmpty()
                    && fixture.source.attempts == 1 && fixture.target.attempts == 1
                    && report.contains(fixture.topic("source") + "#p101")
                    && report.contains("Успешно пройдено форумов: 0"),
                    "Partial publication was lost, duplicated or counted as success.");
            std::cout << "Partial publication diagnostics passed.\n";
        }
        {
            Fixtures fixture;
            fixture.target.partnership = true;
            QTemporaryDir directory;
            auto config = configuration(fixture, driver, directory);
            const QString report = execute(config, directory);
            require(fixture.source.attempts == 0 && fixture.target.attempts == 0
                    && report.contains("Пропущен до следующего прохода"),
                    "A partnership topic was not temporarily skipped.");
            fixture.target.partnership = false;
            execute(config, directory);
            require(fixture.target.posts.size() == 1, "Temporary skip was not retried in the next normal run");
            std::cout << "Temporary partnership skip passed.\n";
        }
        {
            Fixtures fixture;
            auto control = std::make_shared<RunControl>();
            fixture.onSubmitted = [control](const QString &site) {
                if (site == "source")
                    control->stopRequested.store(true);
            };
            const QString report = run(fixture, driver, control, true);
            require(fixture.source.posts.size() == 1 && fixture.target.posts.size() == 1
                    && fixture.other.requests == 0 && report.contains("Обход остановлен пользователем")
                    && report.contains("Успешно пройдено форумов: 1"),
                    "Stopping interrupted a publication pair or processed another forum.");
            std::cout << "Stop after a completed pair passed.\n";
        }
        {
            Fixtures fixture;
            WebDriverClient browser(driver);
            browser.start();
            browser.navigate(fixture.root("source") + "/login.php?action=in");
            browser.navigate(fixture.topic("source"));
            ForumPage page(browser);
            page.publish("[b]Same message[/b]");
            fixture.source.silent = true;
            bool unknown = false;
            try {
                page.publish("[b]Same message[/b]", {}, {}, 350);
            } catch (const PublicationError &error) {
                unknown = error.status() == PublicationError::Status::Unknown;
            }
            require(unknown && fixture.source.posts.size() == 1 && fixture.source.attempts == 2,
                    "An old identical post was mistaken for a new publication.");
            fixture.source.silent = false;
            fixture.source.wrongAuthor = true;
            unknown = false;
            try {
                page.publish("Other author's message", {}, {}, 350);
            } catch (const PublicationError &error) {
                unknown = error.status() == PublicationError::Status::Unknown;
            }
            require(unknown, "A new post by another author was accepted.");
            fixture.source.wrongAuthor = false;
            const QString backlink = fixture.topic("source") + "#p101";
            const QString expected = fixture.message("source") + " [url=" + backlink + "]ваша реклама[/url]";
            fixture.source.messageOverride = "[url=" + backlink + "]ваша реклама[/url]";
            unknown = false;
            try {
                page.publish(expected, backlink, {}, 350);
            } catch (const PublicationError &error) {
                unknown = error.status() == PublicationError::Status::Unknown;
            }
            require(unknown, "A backlink without the advertisement was accepted.");
            fixture.source.messageOverride = fixture.message("source")
                + " [url=" + fixture.topic("source") + "#p2]ваша реклама[/url]";
            unknown = false;
            try {
                page.publish(expected, backlink, {}, 350);
            } catch (const PublicationError &error) {
                unknown = error.status() == PublicationError::Status::Unknown;
            }
            require(unknown, "A message with the wrong reciprocal link was accepted.");
            bool typedTimeout = false;
            try {
                browser.waitFor([] { return QJsonValue(false); }, "тест", 1);
            } catch (const WebDriverError &error) {
                typedTimeout = error.kind() == WebDriverError::Kind::Timeout;
            }
            require(typedTimeout, "A localized timeout lost its structured type.");
            std::cout << "Old posts, authors, content, backlinks and typed timeouts passed.\n";
        }
        {
            Fixtures fixture;
            QTemporaryDir directory;
            const QString previousDirectory = QDir::currentPath();
            configuration(fixture, driver, directory).save(directory.filePath("settings.json"));
            require(QDir::setCurrent(directory.path()), "Cannot enter GUI test directory.");
            {
                BotWindow window;
                window.show();
                QEventLoop loop;
                bool requested = false;
                bool deferred = false;
                fixture.onPage = [&](const QString &) {
                    if (!requested) {
                        requested = true;
                        window.close();
                        deferred = window.isVisible();
                    }
                };
                QTimer watchdog;
                watchdog.setSingleShot(true);
                QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
                QTimer poll;
                QObject::connect(&poll, &QTimer::timeout, [&] {
                    if (requested && !window.isVisible())
                        loop.quit();
                });
                watchdog.start(25000);
                poll.start(20);
                require(QMetaObject::invokeMethod(&window, "startBot", Qt::DirectConnection),
                        "Cannot start GUI worker.");
                loop.exec();
                require(requested && deferred && !window.isVisible()
                        && fixture.source.attempts == 0 && fixture.target.attempts == 0,
                        "Window close was not deferred until the worker stopped.");
            }
            require(QDir::setCurrent(previousDirectory), "Cannot restore working directory.");
            std::cout << "Closing the active window safely passed.\n";
        }
    } catch (const std::exception &error) {
        std::cerr << "Local browser test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "All local browser workflow checks passed.\n";
    return 0;
}
