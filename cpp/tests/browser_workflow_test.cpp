#include "../src/bot_window.h"
#include "../src/forum_page.h"
#include "../src/pr_bot.h"
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
                + message(site).toHtmlEscaped() + "</pre></div></div></div>");
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
            const QString report = run(fixture, driver);
            require(fixture.source.attempts == 0 && fixture.target.attempts == 0
                    && report.contains("Пропущен до следующего прохода"),
                    "A partnership topic was not temporarily skipped.");
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
