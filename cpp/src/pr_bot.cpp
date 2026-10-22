#include "pr_bot.h"

#include "webdriver_client.h"
#include "forum_page.h"
#include "forum_links.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QStandardPaths>
#include <QStringConverter>
#include <QScopedValueRollback>
#include <QTextStream>
#include <QUrl>
#include <QUrlQuery>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace {
// Пропуск действует только в текущем проходе: новый пост PR-аккаунта может
// снова привести в рекламную тему. Исходная закрытая тема останавливает обход.
enum class SkipReason { NoElements, WrongTheme, Account, Timeout, AlreadyPosted };

class SkipForum final : public std::runtime_error {
public:
    SkipForum(SkipReason reason, const QString &message)
        : std::runtime_error(message.toStdString()), reason(reason) {}
    SkipReason reason;
};

class ThemeEnded final : public std::runtime_error {
public:
    ThemeEnded() : std::runtime_error("В исходной теме нет активной формы ответа.") {}
};

class BotCancelled final : public std::runtime_error {
public:
    BotCancelled() : std::runtime_error("Обход остановлен пользователем.") {}
};

bool jsBool(WebDriverClient &driver, const QString &script)
{
    return driver.execute(script).toBool();
}

QString jsString(WebDriverClient &driver, const QString &script)
{
    return driver.execute(script).toString();
}

QStringList jsStringList(WebDriverClient &driver, const QString &script)
{
    const QJsonArray values = driver.execute(script).toArray();
    QStringList result;
    for (const QJsonValue &value : values)
        result.append(value.toString());
    return result;
}

QString withoutTrailingSlash(QString value)
{
    while (value.endsWith('/'))
        value.chop(1);
    return value;
}
}

int BotReport::skippedCount() const
{
    return noElements.size() + wrongTheme.size() + accountErrors.size()
        + timeoutErrors.size() + alreadyPosted.size();
}

QString BotReport::toText(int elapsedMinutes) const
{
    QString result;
    QTextStream out(&result);
    const auto writeList = [&out](const QString &label, const QStringList &items) {
        if (!items.isEmpty())
            out << label << ' ' << items.join(", ") << "\n";
    };

    out << "Режим: " << (checkOnly ? "ПРОВЕРКА БЕЗ ПУБЛИКАЦИИ" : "ПУБЛИКАЦИЯ") << "\n";
    if (startedAt.isValid())
        out << "Начало: " << startedAt.toString(Qt::ISODate) << "\n";
    if (finishedAt.isValid())
        out << "Завершение: " << finishedAt.toString(Qt::ISODate) << "\n";
    out << "Форумов в списке: " << plannedForums << "\n";
    out << "Обработано форумов: " << forums.size() << "\n";
    out << "Не обработано: " << qMax(0, plannedForums - int(forums.size())) << "\n";
    writeList(checkOnly ? "Готовы к публикации:" : "Успешно пройдены:", successful);
    writeList("Не найдена форма ответа/картинка/код рекламы:", noElements);
    writeList("Неподходящая тема/ссылка (форум пропущен до следующего прохода):", wrongTheme);
    writeList("Ошибка аккаунта:", accountErrors);
    writeList("Ошибка загрузки форума:", timeoutErrors);
    writeList("Реклама уже есть на последней странице темы:", alreadyPosted);
    out << (checkOnly ? "Проверены и готовы к публикации: " : "Успешно пройдено форумов: ")
        << successful.size() << "\n";
    if (checkOnly)
        out << "Рекламные сообщения в этом проходе не отправлялись.\n";
    out << "Было пропущено форумов: " << skippedCount() << "\n";
    out << "Завершились ошибкой: "
        << qMax(0, int(forums.size()) - int(successful.size()) - skippedCount()
                   - int(cancelledForums.size())) << "\n";
    out << "Прерваны пользователем: " << cancelledForums.size() << "\n";
    if (cancelled)
        out << "Обход остановлен пользователем; оставшиеся форумы не обработаны.\n";
    out << "Затрачено времени (минут): " << elapsedMinutes << "\n";
    out << "Точная длительность (секунд): " << elapsedSeconds << "\n";
    out << (criticalError.isEmpty() ? "Критических ошибок при прохождении не выявлено."
                                    : "Критическая ошибка: " + criticalError) << "\n";
    if (!forums.isEmpty())
        out << "\nПодробности по форумам:\n";
    for (int index = 0; index < forums.size(); ++index) {
        const ForumRunResult &forum = forums.at(index);
        out << index + 1 << ". " << forum.forumUrl << "\n"
            << "   Результат: " << forum.outcome << "; длительность: "
            << forum.elapsedSeconds << " с\n"
            << "   Последний этап: " << forum.stage << "\n"
            << "   Описание: " << forum.detail << "\n";
        if (!forum.topicUrl.isEmpty())
            out << "   Тема по сообщениям PR-аккаунта: " << forum.topicUrl << "\n";
        if (!forum.sourcePageUrl.isEmpty())
            out << "   Подтверждённое сообщение в исходной теме: " << forum.sourcePageUrl << "\n";
        if (!forum.targetPageUrl.isEmpty())
            out << "   Подтверждённое сообщение на целевом форуме: " << forum.targetPageUrl << "\n";
    }
    return result;
}

PrBot::PrBot(BotConfig config, QString reportPath, std::shared_ptr<RunControl> control)
    : m_config(std::move(config)), m_reportPath(std::move(reportPath)),
      m_control(control ? std::move(control) : std::make_shared<RunControl>()),
      m_ancestorTopic(m_config.threadUrl)
{
}

void PrBot::run()
{
    const auto started = std::chrono::steady_clock::now();
    m_report = {};
    m_report.checkOnly = m_config.checkOnly;
    m_report.startedAt = QDateTime::currentDateTime();
    m_currentUrl.clear();
    m_actualSourceForum.clear();
    m_currentResult = {};
    m_stage = "Подготовка";
    m_logWriteWarningSent = false;
    m_log = std::make_unique<RunLog>(m_reportPath,
                                   QStringList{m_config.password, m_config.prCode}, m_config.checkOnly);
    emit statisticsChanged(0, 0, 0, 0);
    try {
        m_log->open();
        log(m_config.checkOnly ? "Начата проверка без публикации. Рекламные сообщения отправляться не будут."
                              : "Начат новый проход с публикацией. Журнал записывается по мере выполнения.");
        log("История запуска: " + m_log->archivePath());
        checkStop();
        emit progressChanged(0);
        const QStringList forums = readForums();
        m_report.plannedForums = int(forums.size());
        log(QString("Загружено форумов: %1. Файл: %2")
                .arg(forums.size()).arg(m_config.forumsFile));
        if (forums.isEmpty())
            throw std::runtime_error("Файл со списком форумов пуст.");
        if (!m_config.checkOnly && m_config.prCode.trimmed().isEmpty())
            throw std::runtime_error("Не заполнен текст рекламы.");
        m_ancestorUrl = ancestorForumUrl();
        log(QString("Исходная тема из настроек: %1").arg(m_ancestorTopic));
        log(QString("Проверка рекламы на последней странице: %1.")
                .arg(m_config.checkLastPage ? "включена" : "выключена"));
        log(QString("Режим входа: %1.")
                .arg(m_config.useAccount ? "указанный аккаунт" : "форумные скрипты PR/PiarIn"));

        QString driverPath = m_config.chromeDriverPath.trimmed();
        if (driverPath.isEmpty())
            driverPath = QStandardPaths::findExecutable("chromedriver");
        if (driverPath.isEmpty())
            throw std::runtime_error("ChromeDriver не найден. Выберите файл chromedriver.");

        setStage("ChromeDriver", QString("Запускаю драйвер: %1").arg(driverPath));
        checkStop();
        WebDriverClient driver(driverPath);
        driver.start();
        log(QString("Браузерная сессия создана. Chrome: %1; ChromeDriver: %2.")
                .arg(driver.browserVersion(), driver.driverVersion()));
        m_parentWindow = driver.windowHandles().value(0);

        try {
            setStage("Исходный форум", QString("Открываю %1").arg(m_ancestorUrl));
            navigate(driver, m_ancestorUrl);
            m_currentUrl = m_ancestorUrl;
            enterForum(driver);
        } catch (const BotCancelled &) {
            throw;
        } catch (const std::exception &error) {
            log(QString("Автоматический поиск исходной темы не удался: %1")
                    .arg(QString::fromUtf8(error.what())), LogLevel::Warning);
            setStage("Исходная тема", QString("Открываю тему из настроек: %1").arg(m_ancestorTopic));
            navigate(driver, m_ancestorTopic);
            waitForTopic(driver);
        }

        if (!jsBool(driver, "return !!document.querySelector('#main-reply');"))
            throw ThemeEnded();
        ForumPage(driver, [this] { checkStop(); }).checkReady();
        m_actualSourceForum = ForumLinks::forumFromTopic(driver.currentUrl());
        log(QString("Исходная тема: %1; форма ответа доступна.").arg(driver.currentUrl()));

        setStage("Вкладки", "Открываю отдельную вкладку для целевых форумов.");
        driver.openTab();
        const QStringList handles = driver.windowHandles();
        if (handles.size() < 2)
            throw std::runtime_error("Не удалось открыть вкладку для обхода форумов.");
        m_forumWindow = handles.last();

        for (int index = 0; index < forums.size(); ++index) {
            checkStop();
            const auto forumStarted = std::chrono::steady_clock::now();
            const QString forumInput = forums.at(index).trimmed();
            QString forumUrl = forumInput;
            m_currentUrl = forumInput;
            m_currentResult = {};
            m_currentResult.forumUrl = forumInput;
            bool stop = false;
            setStage("Открытие форума", QString("Форум %1 из %2.").arg(index + 1).arg(forums.size()));
            try {
                forumUrl = normalizeUrl(forumInput);
                m_currentUrl = forumUrl;
                m_currentResult.forumUrl = forumUrl;
                driver.switchToWindow(m_forumWindow);
                navigate(driver, forumUrl);
                enterForum(driver);
                visitForum(driver, forumUrl, m_ancestorUrl);
                m_report.successful.append(forumUrl);
                m_currentResult.outcome = m_config.checkOnly ? "Готов к публикации" : "Успешно";
                m_currentResult.detail = m_config.checkOnly
                    ? "Вход, тема, форма и рекламный код проверены; рекламные сообщения не отправлялись."
                    : "Появление обоих сообщений подтверждено; ссылки записаны ниже.";
            } catch (const BotCancelled &error) {
                m_report.cancelled = true;
                m_report.cancelledForums.append(forumUrl);
                m_currentResult.outcome = "Остановлен пользователем";
                m_currentResult.detail = QString::fromUtf8(error.what());
                log(m_currentResult.detail, LogLevel::Warning);
                stop = true;
            } catch (const ThemeEnded &error) {
                m_report.criticalError = QString::fromUtf8(error.what());
                m_currentResult.outcome = "Ошибка";
                m_currentResult.detail = m_report.criticalError;
                log("Исходная рекламная тема закончилась. Останавливаю обход.", LogLevel::Error);
                stop = true;
            } catch (const SkipForum &error) {
                switch (error.reason) {
                case SkipReason::NoElements: m_report.noElements.append(forumUrl); break;
                case SkipReason::WrongTheme: m_report.wrongTheme.append(forumUrl); break;
                case SkipReason::Account: m_report.accountErrors.append(forumUrl); break;
                case SkipReason::Timeout: m_report.timeoutErrors.append(forumUrl); break;
                case SkipReason::AlreadyPosted: m_report.alreadyPosted.append(forumUrl); break;
                }
                m_currentResult.outcome = "Пропущен до следующего прохода";
                m_currentResult.detail = QString::fromUtf8(error.what());
                log(m_currentResult.detail, LogLevel::Warning);
            } catch (const WebDriverError &error) {
                const QString message = QString::fromUtf8(error.what());
                if (error.kind() == WebDriverError::Kind::Timeout && m_currentResult.sourcePageUrl.isEmpty()) {
                    m_report.timeoutErrors.append(forumUrl);
                    m_currentResult.outcome = "Пропущен: превышено время ожидания";
                    log(message, LogLevel::Warning);
                } else {
                    if (!m_currentResult.sourcePageUrl.isEmpty())
                        log("Первая публикация подтверждена, вторая не завершена. Обход остановлен без повтора.",
                            LogLevel::Warning);
                    m_report.criticalError = message;
                    m_currentResult.outcome = "Ошибка";
                    log(message, LogLevel::Error);
                    stop = true;
                }
                m_currentResult.detail = message;
            } catch (const std::exception &error) {
                const QString message = QString::fromUtf8(error.what());
                if (!m_currentResult.sourcePageUrl.isEmpty() && m_currentResult.targetPageUrl.isEmpty())
                    log("Первая отправка уже выполнена. Проверьте обе темы перед повторным запуском: "
                        "результат второй отправки неизвестен.", LogLevel::Warning);
                m_report.criticalError = message;
                m_currentResult.outcome = "Ошибка";
                log(QString("Критическая ошибка; обход остановлен: %1").arg(message), LogLevel::Error);
                stop = true;
                m_currentResult.detail = message;
            }
            m_currentResult.stage = m_stage;
            m_currentResult.elapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - forumStarted).count();
            m_report.forums.append(m_currentResult);
            emit statisticsChanged(int(m_report.forums.size()), int(m_report.successful.size()),
                m_report.skippedCount(), qMax(0, int(m_report.forums.size())
                    - int(m_report.successful.size()) - m_report.skippedCount()
                    - int(m_report.cancelledForums.size())));
            log(QString("Результат: %1. Затрачено: %2 с.")
                    .arg(m_currentResult.outcome).arg(m_currentResult.elapsedSeconds));
            // Прогресс отражает завершённые попытки, а не начало обработки.
            emit progressChanged(((index + 1) * 100) / forums.size());
            if (stop)
                break;
        }
        if (m_control->stopRequested.load())
            m_report.cancelled = true;
        m_currentUrl.clear();
        setStage("Завершение", "Закрываю браузерную сессию и ChromeDriver.");
        driver.quit();
    } catch (const BotCancelled &error) {
        m_report.cancelled = true;
        log(QString::fromUtf8(error.what()), LogLevel::Warning);
    } catch (const std::exception &error) {
        m_report.criticalError = QString::fromUtf8(error.what());
        log(QString("Обход остановлен: %1").arg(m_report.criticalError), LogLevel::Error);
    }

    m_report.finishedAt = QDateTime::currentDateTime();
    m_report.elapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started).count();
    const int elapsedMinutes = int(std::chrono::duration_cast<std::chrono::minutes>(
        std::chrono::steady_clock::now() - started).count());
    m_currentUrl.clear();
    setStage("Итоги", QString(m_config.checkOnly
                ? "Проверено %1/%2; готовы: %3; пропущено: %4; длительность: %5 с. Сообщения не отправлялись."
                : "Обработано %1/%2; успешно: %3; пропущено: %4; длительность: %5 с.")
                .arg(m_report.forums.size()).arg(m_report.plannedForums)
                .arg(m_report.successful.size()).arg(m_report.skippedCount()).arg(m_report.elapsedSeconds));
    try {
        writeReport(elapsedMinutes);
        emit reportWritten(m_reportPath);
    } catch (const std::exception &error) {
        log(QString("Не удалось записать итоги: %1").arg(QString::fromUtf8(error.what())), LogLevel::Error);
    }
    emit runCompleted(!m_report.criticalError.isEmpty(), m_report.cancelled);
    emit finished();
}

QStringList PrBot::readForums() const
{
    QFile file(m_config.forumsFile);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        throw std::runtime_error(QString("Не удалось открыть список форумов: %1")
                                     .arg(file.errorString()).toStdString());

    QStringList forums;
    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    while (!in.atEnd()) {
        // Пустые строки и комментарии не входят в число обрабатываемых форумов.
        const QString line = in.readLine().trimmed();
        if (!line.isEmpty() && !line.startsWith('#'))
            forums.append(line);
    }
    return forums;
}

QString PrBot::normalizeUrl(const QString &input) const
{
    QString value = input.trimmed();
    if (!value.contains("://"))
        value.prepend("http://");
    const QUrl url(value);
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https")
        || url.host().isEmpty())
        throw SkipForum(SkipReason::WrongTheme,
                        QString("Некорректная ссылка в списке форумов: %1").arg(input));
    return url.toString(QUrl::FullyEncoded);
}

QString PrBot::ancestorForumUrl() const
{
    const QUrl topic(m_ancestorTopic);
    if (!topic.isValid() || topic.host().isEmpty()
        || (topic.scheme() != "http" && topic.scheme() != "https"))
        throw std::runtime_error("Укажите корректную ссылку на PR-тему (http или https).");

    QString path = topic.path();
    const int topicSegment = path.indexOf("/viewtopic");
    if (topicSegment >= 0)
        path = path.left(topicSegment);
    QUrl forum = topic;
    forum.setPath(path);
    forum.setQuery(QString());
    forum.setFragment(QString());
    return withoutTrailingSlash(forum.toString(QUrl::FullyEncoded));
}

void PrBot::enterForum(WebDriverClient &driver)
{
    try {
        bool ready = false;
        if (m_config.useAccount && !m_config.login.isEmpty() && !m_config.password.isEmpty()) {
            setStage("Вход", "Вхожу с указанными учётными данными.");
            forcedLogin(driver);
            ready = openAccountTopic(driver);
        } else {
            setStage("Вход", "Ищу форумные скрипты PR/PiarIn.");
            ready = firstEnter(driver);
        }
        if (!ready)
            throw SkipForum(SkipReason::Account,
                            QString("Не найден аккаунт с сообщениями для публикации на %1").arg(m_currentUrl));
    } catch (const BotCancelled &) {
        throw;
    } catch (const SkipForum &) {
        throw;
    } catch (const WebDriverError &) {
        throw;
    } catch (const std::exception &error) {
        throw SkipForum(SkipReason::Account,
                        QString("Ошибка входа на %1: %2")
                            .arg(m_currentUrl, QString::fromUtf8(error.what())));
    }

}

bool PrBot::firstEnter(WebDriverClient &driver)
{
    // Скрипты предоставляет сам форум. Порядок попыток сохранён из Python:
    // обычный PR-вход, затем первый и второй аккаунты двойного PR-скрипта.
    QJsonObject available;
    try {
        available = driver.waitFor([&] {
            return driver.execute(
                "const piar = typeof window.PiarIn === 'function'; "
                "const pr = !!window.PR && (typeof PR.in_1 === 'function' || typeof PR.in_2 === 'function'); "
                "return piar || pr ? { piar, pr } : false;");
        }, "форумные скрипты входа", 10000, [this] { checkStop(); }).toObject();
    } catch (const WebDriverError &error) {
        if (error.kind() != WebDriverError::Kind::Timeout)
            throw;
        throw std::runtime_error("На странице не найден скрипт входа PR или PiarIn.");
    }
    const bool hasPiarIn = available.value("piar").toBool();
    const bool hasPr = available.value("pr").toBool();
    log(QString("Доступные скрипты: PiarIn — %1; PR — %2.")
            .arg(hasPiarIn ? "да" : "нет", hasPr ? "да" : "нет"));

    if (!hasPiarIn && !hasPr)
        throw std::runtime_error("На странице не найден скрипт входа PR или PiarIn.");

    QStringList scripts;
    if (hasPiarIn)
        scripts.append("PiarIn();");
    if (hasPr) {
        if (jsBool(driver, "return typeof PR.in_1 === 'function';"))
            scripts.append("PR['in_1']();");
        if (jsBool(driver, "return typeof PR.in_2 === 'function';"))
            scripts.append("PR['in_2']();");
    }
    for (int index = 0; index < scripts.size(); ++index) {
        checkStop();
        setStage("Вход", QString("Пробую %1").arg(scripts.at(index)));
        driver.execute(scripts.at(index));
        // При успехе остаёмся в проверенной теме. Второго такого же обхода
        // профиля/поиска/темы в enterForum() больше нет.
        if (openAccountTopic(driver))
            return true;
        if (index + 1 < scripts.size()
            && jsBool(driver, "return typeof GroupID !== 'undefined' && String(GroupID) !== '3';"))
            forumLogout(driver);
    }
    return false;
}

bool PrBot::openAccountTopic(WebDriverClient &driver)
{
    setStage("Аккаунт", "Ожидаю данные аккаунта после входа.");
    try {
        m_userId = driver.waitFor([&] {
            return driver.execute(
                "return typeof UserID !== 'undefined' && UserID != null && Number(UserID) > 0 "
                "&& (typeof GroupID === 'undefined' || String(GroupID) !== '3') ? String(UserID) : ''; ");
        }, "вход в аккаунт", 10000, [this] { checkStop(); }).toString();
    } catch (const WebDriverError &error) {
        if (error.kind() != WebDriverError::Kind::Timeout)
            throw;
        log("Вход не подтверждён за отведённое время.", LogLevel::Warning);
        return false;
    }
    log(QString("Получен ID аккаунта: %1; открываю его сообщения напрямую.").arg(m_userId));
    return getPrMessages(driver);
}

bool PrBot::getPrMessages(WebDriverClient &driver)
{
    // Не ищем тему по названию раздела: открываем последнее сообщение
    // вошедшего PR-аккаунта, как в исходном сценарии пользователя.
    QUrl messages(m_currentUrl);
    messages.setPath(messages.path() + (messages.path().endsWith('/') ? "" : "/") + "search.php");
    QUrlQuery query;
    query.addQueryItem("action", "show_user_posts");
    query.addQueryItem("user_id", m_userId);
    messages.setQuery(query);
    setStage("Сообщения PR", QString("Открываю сообщения аккаунта: %1")
                                 .arg(messages.toString(QUrl::FullyEncoded)));
    navigate(driver, messages.toString(QUrl::FullyEncoded));
    driver.waitFor([&] {
        return driver.execute("return document.readyState !== 'loading' && !!document.querySelector('#pun-main');");
    }, "результаты поиска сообщений", 10000, [this] { checkStop(); });

    const QString postUrl = jsString(driver,
        "const link=document.evaluate('//*[@id=\"pun-main\"]/div[2]/div[1]/div/div[3]/ul/li/a', "
        "document, null, XPathResult.FIRST_ORDERED_NODE_TYPE, null).singleNodeValue; "
        "return link ? link.href : ''; ");
    if (postUrl.isEmpty()) {
        log("Не найдена ссылка на последнее сообщение аккаунта.", LogLevel::Warning);
        return false;
    }
    setStage("Проверка темы", QString("Последнее сообщение ведёт в %1").arg(postUrl));
    if (withoutTrailingSlash(m_currentUrl) != withoutTrailingSlash(m_ancestorUrl))
        m_currentResult.topicUrl = postUrl;
    navigate(driver, postUrl);
    waitForTopic(driver);
    if (!jsBool(driver, "return !!document.querySelector('#main-reply');")
        && withoutTrailingSlash(m_currentUrl) != withoutTrailingSlash(m_ancestorUrl)) {
        throw SkipForum(
            SkipReason::NoElements,
            QString("В последней теме PR-аккаунта на %1 отсутствует форма ответа; "
                    "публикация недоступна, форум пропущен до следующего прохода.")
                .arg(m_currentUrl));
    }
    if (!checkImageAndFormAnswer(driver)) {
        throw SkipForum(
            SkipReason::WrongTheme,
            QString("Последнее сообщение PR-аккаунта на %1 ведёт в неподходящую тему "
                    "(возможно, партнёрку); форум пропущен до следующего прохода.")
                .arg(m_currentUrl));
    }
    return true;
}

bool PrBot::checkImageAndFormAnswer(WebDriverClient &driver)
{
    // Наличие формы, рекламного кода в первом посте и картинки в последнем —
    // эвристика MyBB/RusFF, а не универсальный признак рекламной темы.
    const QJsonObject elements = driver.execute(
        "return { form: !!document.querySelector('#main-reply'), "
        "code: !!document.querySelector('.post.topicpost .code-box'), "
        "image: !!document.querySelector('.endpost .postimg'), title: document.title };").toObject();
    log(QString("Тема «%1»: форма ответа — %2; код в первом посте — %3; "
                "картинка в последнем посте — %4.")
            .arg(elements.value("title").toString(),
                 elements.value("form").toBool() ? "есть" : "нет",
                 elements.value("code").toBool() ? "есть" : "нет",
                 elements.value("image").toBool() ? "есть" : "нет"));
    const bool valid = elements.value("form").toBool() && elements.value("code").toBool()
        && elements.value("image").toBool();
    if (!valid && withoutTrailingSlash(m_currentUrl) == withoutTrailingSlash(m_ancestorUrl))
        throw std::runtime_error("На исходном форуме не найдена тема с рекламой аккаунта.");
    return valid;
}

void PrBot::forcedLogin(WebDriverClient &driver)
{
    // Аргументы передаются отдельно от JavaScript: кавычки в пароле не могут
    // изменить скрипт, а содержимое запроса не записывается в журнал.
    const QJsonArray args{m_config.login, m_config.password};
    driver.execute(
        "const form = document.createElement('form'); form.id = 'prbot-login'; "
        "form.method = 'post'; form.action = '/login.php?action=in'; "
        "for (const [name,value,type] of [['form_sent','1','hidden'],"
        "['redirect_url','','hidden'],['req_username',arguments[0],'text'],"
        "['req_password',arguments[1],'password']]) { const input=document.createElement('input'); "
        "input.name=name; input.value=value; input.type=type; form.appendChild(input); } "
        "const submit=document.createElement('input'); submit.type='submit'; submit.name='login'; "
        "form.appendChild(submit); const anchor=document.querySelector('#navlogin'); "
        "if (anchor) anchor.after(form); else document.body.appendChild(form); "
        "form.submit();", args);
}

void PrBot::forumLogout(WebDriverClient &driver)
{
    setStage("Вход", "Выхожу из аккаунта перед следующей попыткой PR-входа.");
    QUrl logout(m_currentUrl);
    logout.setPath(logout.path() + (logout.path().endsWith('/') ? "" : "/") + "login.php");
    QUrlQuery query;
    query.addQueryItem("action", "out");
    query.addQueryItem("id", m_userId);
    logout.setQuery(query);
    navigate(driver, logout.toString(QUrl::FullyEncoded));
    driver.waitFor([&] {
        return driver.execute("return typeof GroupID !== 'undefined' && String(GroupID) === '3';");
    }, "выход из аккаунта", 10000, [this] { checkStop(); });
}

void PrBot::visitForum(WebDriverClient &driver, const QString &forumUrl,
                       const QString &ancestorUrl)
{
    QScopedValueRollback<bool> publicationGuard(m_finishingPublication, false);
    checkStop();
    setStage("Рекламный код", "Читаю шаблон из первого сообщения темы.");
    const QJsonArray sources = driver.execute(
        "return Array.from(document.querySelectorAll('.topicpost .code-box pre'))"
        ".map((element) => element.textContent);").toArray();
    if (sources.isEmpty())
        throw SkipForum(SkipReason::NoElements,
                        QString("Не найден код рекламы на форуме %1").arg(forumUrl));

    QString topicPostHtml = sources.first().toString();
    log(QString("Найдено блоков кода: %1; длина выбранного шаблона: %2 символов.")
            .arg(sources.size()).arg(topicPostHtml.size()));
    // HTML-ссылки в коде характерны для баннерообмена. Такой форум временно
    // пропускаем: следующий проход начнётся с актуальных сообщений PR-аккаунта.
    for (const QJsonValue &source : sources) {
        const QString code = source.toString();
        if (code.contains("&lt;/a&gt;") || code.contains("</a>"))
            throw SkipForum(SkipReason::WrongTheme,
                            QString("Тема %1 не похожа на тему для рекламы.").arg(forumUrl));
    }

    if (m_config.checkLastPage) {
        setStage("Проверка повторов", "Ищу рекламу исходного форума на последней странице.");
        const QStringList pages = jsStringList(driver,
            "return Array.from(document.querySelectorAll('.pagelink > a')).map(a => a.href);");
        const bool hasNext = jsBool(driver, "return !!document.querySelector('.pagelink > a.next');");
        if (hasNext && pages.size() >= 2) {
            log(QString("Перехожу на последнюю страницу: %1").arg(pages.at(pages.size() - 2)));
            navigate(driver, pages.at(pages.size() - 2));
            waitForTopic(driver);
        }
        const QStringList references = jsStringList(driver, R"JS(
            return Array.from(document.querySelectorAll('.post-content a[href], .post-content img[src]'))
                .filter(element => !element.closest('.code-box'))
                .map(element => element.href || element.src);
        )JS");
        log(QString("Проверяю адресов в сообщениях на странице: %1.").arg(references.size()));
        if (ForumLinks::containsForum(references, ancestorUrl)
            || ForumLinks::containsForum(references, m_actualSourceForum))
            throw SkipForum(SkipReason::AlreadyPosted,
                            QString("Реклама уже размещена на последней странице %1").arg(forumUrl));
        log("Ссылка на исходный форум среди проверенных сообщений не найдена.");
    } else {
        setStage("Проверка повторов", "Проверка отключена в настройках.");
    }

    const QString actualTarget = ForumLinks::forumFromTopic(driver.currentUrl());
    const QStringList templateLinks = ForumLinks::fromBbCode(topicPostHtml);
    if ((!ForumLinks::containsForum(templateLinks, forumUrl)
         && !ForumLinks::containsForum(templateLinks, actualTarget)) || topicPostHtml.contains("&lt;/"))
        throw SkipForum(SkipReason::WrongTheme,
                        QString("Шаблон рекламы не содержит ссылку на %1.").arg(forumUrl));
    log(QString("В BBCode найдена ссылка на форум %1; полный домен и каталог совпадают.")
            .arg(QUrl(forumUrl).host()));

    // Единственная ветка с публикациями находится ниже этой границы. Проверка
    // использует тот же вход и те же критерии, но не вызывает publish().
    if (m_config.checkOnly) {
        ForumPage(driver, [this] { checkStop(); }).checkReady();
        setStage("Проверка завершена", "Форум готов к публикации. Рекламные сообщения не отправлялись.");
        return;
    }

    // Сначала переносим чужой шаблон в свою тему. Только затем отправляем
    // свой шаблон на целевой форум, добавив ссылку на созданную взаимность.
    setStage("Отправка в исходную тему", "Переключаюсь в исходную рекламную тему.");
    driver.switchToWindow(m_parentWindow);
    if (!jsBool(driver, "return !!document.querySelector('#main-reply');"))
        throw ThemeEnded();

    ForumPage page(driver, [this] { checkStop(); });
    const QString postLink = page.publish(topicPostHtml, {}, [this] {
        checkStop();
        // После начала первой отправки завершаем пару даже при запросе остановки.
        // Принудительное прерывание между отправками оставило бы взаимность без ответа.
        m_finishingPublication = true;
        log("Отправляю сообщение и ожидаю подтверждения его появления.");
    });
    m_currentResult.sourcePageUrl = postLink;
    log(QString("Новое сообщение подтверждено: %1").arg(postLink));

    setStage("Отправка на целевой форум", "Добавляю к нашему шаблону ссылку на взаимную рекламу.");
    driver.switchToWindow(m_forumWindow);
    const QString finalMessage = m_config.prCode + " [url=" + postLink + "]ваша реклама[/url]";
    m_currentResult.targetPageUrl = page.publish(finalMessage, postLink, [this] {
        log("Отправляю ответную рекламу и проверяю новое сообщение вместе со ссылкой на взаимность.");
    });
    log(QString("Новое сообщение подтверждено: %1").arg(m_currentResult.targetPageUrl));
}

void PrBot::checkStop() const
{
    if (!m_finishingPublication && m_control->stopRequested.load())
        throw BotCancelled();
}

void PrBot::navigate(WebDriverClient &driver, const QString &url)
{
    checkStop();
    driver.navigate(url);
    checkStop();
}

void PrBot::waitForTopic(WebDriverClient &driver)
{
    driver.waitFor([&] {
        return driver.execute("return document.readyState !== 'loading' && !!document.querySelector('.topicpost');");
    }, "первое сообщение темы", 10000, [this] { checkStop(); });
}

void PrBot::setStage(const QString &stage, const QString &message)
{
    m_stage = stage;
    emit activityChanged(m_currentUrl, m_stage);
    log(message);
}

void PrBot::log(const QString &message, LogLevel level)
{
    const QString line = m_log->append(level, m_stage, message, m_currentUrl);
    emit logMessage(line);
    // Потеря доступа к файлу после первой отправки не должна превращаться
    // в ошибку публикации: это могло бы спровоцировать повторную отправку.
    if (!m_log->errorString().isEmpty() && !m_logWriteWarningSent) {
        m_logWriteWarningSent = true;
        emit logMessage(m_log->append(LogLevel::Error, "Журнал",
            QString("Запись в файл недоступна: %1. Последующие события видны только в окне.")
                .arg(m_log->errorString())));
    }
}

void PrBot::writeReport(int elapsedMinutes)
{
    // Журнал уже записан; дописываем сводку, сохраняя события даже после ошибки.
    if (!m_log->appendReport(m_report.toText(elapsedMinutes)))
        throw std::runtime_error(m_log->errorString().toStdString());
}
