#include "pr_bot.h"

#include "webdriver_client.h"
#include "forum_page.h"
#include "forum_links.h"

#include <QFile>
#include <QJsonArray>
#include <QSet>
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
// Домен-алиас разрешаем только после фактического перехода браузера с
// настроенного адреса. Имя PHP-страницы не является каталогом форума.
QString landingForum(WebDriverClient &driver)
{
    QUrl page(driver.currentUrl());
    QString path = page.path();
    if (path.section('/', -1).endsWith(".php", Qt::CaseInsensitive))
        page.setPath(path.left(path.lastIndexOf('/')));
    return ForumLinks::normalizeForum(page.toString());
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
    if (!checkOnly) out << "Лимит реклам за запуск: "
        << (publicationLimit > 0 ? QString::number(publicationLimit) : QString("без ограничения")) << "\n";
    if (limitReached) out << (plannedForums > forums.size()
        ? "Лимит достигнут. Остальные форумы сохранены для продолжения.\n"
        : "Лимит достигнут. Весь список обработан.\n");
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
    m_report.publicationLimit = m_config.checkOnly ? 0 : m_config.publicationLimit;
    m_report.startedAt = QDateTime::currentDateTime();
    m_currentUrl.clear();
    m_actualSourceForum.clear();
    m_currentResult = {};
    m_progressState = {};
    m_stage = "Подготовка";
    m_logWriteWarningSent = false;
    m_log = std::make_unique<RunLog>(m_reportPath,
                                   QStringList{m_config.password, m_config.prCode}, m_config.checkOnly);
    m_diagnosticWriteWarningSent = false;
    m_diagnostics.reset();
    std::unique_ptr<WebDriverClient> browser;
    emit statisticsChanged(0, 0, 0, 0);
    try {
        m_log->open();
        m_diagnostics = std::make_unique<RunDiagnostics>(QStringList{m_config.password, m_config.prCode});
        if (m_diagnostics->start(m_log->archivePath(), m_config.checkOnly))
            emit diagnosticsAvailable(m_log->archivePath());
        else diagnosticWarning();
        log(m_config.checkOnly ? "Начата проверка без публикации. Рекламные сообщения отправляться не будут."
                              : "Начат новый проход с публикацией. Журнал записывается по мере выполнения.");
        log("История запуска: " + m_log->archivePath());
        checkStop();
        emit progressChanged(0);
        if (m_config.publicationLimit < 0 || m_config.publicationLimit > BotConfig::MaxPublicationLimit)
            throw std::runtime_error("Некорректный лимит рекламы за запуск.");
        if (m_config.checkOnly) log("Лимит рекламы не применяется к проверке без публикации.");
        else log(m_config.publicationLimit > 0
            ? QString("Лимит за запуск: %1 успешно завершённых пар. Пропуски и ошибки в лимит не входят.").arg(m_config.publicationLimit)
            : QString("Лимит рекламы за запуск не задан: обрабатываю весь включённый список."));
        QStringList forums = readForums();
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

        if (!m_config.checkOnly) {
            m_state = std::make_unique<RunState>(RunState::pathForReport(m_reportPath));
            m_state->begin(m_config, forums);
            if (m_config.resumeRun && !forums.isEmpty()) forums = m_state->unfinished(forums);
            log("Прогресс сохраняется в progress.json перед каждой отправкой.");
        }
        m_report.plannedForums = int(forums.size());
        if (forums.isEmpty()) throw std::runtime_error("Все выбранные форумы сохранённого прохода уже завершены.");

        setStage("ChromeDriver", QString("Запускаю драйвер: %1").arg(driverPath));
        checkStop();
        browser = std::make_unique<WebDriverClient>(driverPath);
        auto &driver = *browser;
        driver.start();
        if (!m_diagnostics->setVersions(driver.browserVersion(), driver.driverVersion())) diagnosticWarning();
        log(QString("Браузерная сессия создана. Chrome: %1; ChromeDriver: %2.")
                .arg(driver.browserVersion(), driver.driverVersion()));
        m_parentWindow = driver.windowHandles().value(0);

        m_sourcePrepared = false;
        // При восстановлении читаем прежние темы даже если форма уже закрыта.
        if (m_config.checkOnly || !m_state->entry(forums.first()).isPartial()) prepareSource(driver);

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
                m_progressState = m_state ? m_state->entry(forumUrl) : ForumProgress{};
                m_progressState.url = forumUrl;
                if (!m_config.checkOnly && m_progressState.isPartial()) {
                    recoverForum(driver);
                } else {
                    navigate(driver, forumUrl);
                    enterForum(driver);
                    if (!m_sourcePrepared) {
                        const QString targetUser = m_userId;
                        driver.switchToWindow(m_parentWindow);
                        prepareSource(driver);
                        m_currentUrl = forumUrl;
                        m_userId = targetUser;
                        driver.switchToWindow(m_forumWindow);
                    }
                    visitForum(driver, forumUrl, m_ancestorUrl);
                }
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
                recordFailure(&driver, QString::fromUtf8(error.what()));
                m_report.criticalError = QString::fromUtf8(error.what());
                m_currentResult.outcome = "Ошибка";
                m_currentResult.detail = m_report.criticalError;
                log("Исходная рекламная тема закончилась. Останавливаю обход.", LogLevel::Error);
                stop = true;
            } catch (const SkipForum &error) {
                if (error.reason != SkipReason::AlreadyPosted) recordFailure(&driver, QString::fromUtf8(error.what()));
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
                recordFailure(&driver, message);
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
                recordFailure(&driver, message);
                if (!m_currentResult.sourcePageUrl.isEmpty() && m_currentResult.targetPageUrl.isEmpty())
                    log("Первая отправка уже выполнена. Проверьте обе темы перед повторным запуском: "
                        "результат второй отправки неизвестен.", LogLevel::Warning);
                m_report.criticalError = message;
                m_currentResult.outcome = "Ошибка";
                log(QString("Критическая ошибка; обход остановлен: %1").arg(message), LogLevel::Error);
                stop = true;
                m_currentResult.detail = message;
            }
            if (m_state) {
                if (m_currentResult.outcome == "Успешно") {
                    m_progressState.phase = "complete";
                    m_progressState.review = false;
                } else if (!m_progressState.isPartial()) {
                    m_progressState.phase = m_currentResult.outcome.startsWith("Пропущен") ? "skipped"
                        : m_report.cancelled ? "pending" : "failed";
                } else {
                    // Не теряем сторону и доказательства неоднозначной отправки.
                    m_progressState.review = m_progressState.phase != "source_confirmed";
                }
                m_progressState.detail = m_currentResult.detail;
                saveProgress();
            }
            if (!m_state) emit forumUpdated(m_currentResult.forumUrl, m_currentResult.outcome,
                m_currentResult.detail, m_currentResult.topicUrl,
                m_currentResult.sourcePageUrl, m_currentResult.targetPageUrl);
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
            // Проверяем лимит только после подтверждения и сохранения всей пары.
            // Форумы, до которых не дошли, уже записаны в progress.json как pending.
            if (!m_config.checkOnly && m_config.publicationLimit > 0
                && m_report.successful.size() >= m_config.publicationLimit) {
                m_report.limitReached = true;
                const int remaining = int(forums.size()) - index - 1;
                setStage("Лимит за запуск", QString("Лимит достигнут: размещено реклам %1 из %2; не обработано форумов: %3. Для следующего прохода нажмите «Продолжить».")
                    .arg(m_report.successful.size()).arg(m_config.publicationLimit).arg(remaining));
                emit publicationLimitReached(int(m_report.successful.size()), remaining);
                break;
            }
        }
        if (m_control->stopRequested.load() && !m_report.limitReached)
            m_report.cancelled = true;
        m_currentUrl.clear();
        setStage("Завершение", "Закрываю браузерную сессию и ChromeDriver.");
        driver.quit();
    } catch (const BotCancelled &error) {
        m_report.cancelled = true;
        log(QString::fromUtf8(error.what()), LogLevel::Warning);
    } catch (const std::exception &error) {
        recordFailure(browser.get(), QString::fromUtf8(error.what()));
        m_report.criticalError = QString::fromUtf8(error.what());
        log(QString("Обход остановлен: %1").arg(m_report.criticalError), LogLevel::Error);
    }

    // Захват ошибки выполняется до закрытия драйвера, в том числе при сбое
    // подготовки исходной темы. Исключение диагностики не меняет ход отправки.
    browser.reset();
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
    if (m_diagnostics && !m_diagnostics->finish(m_report.cancelled, m_report.criticalError)) diagnosticWarning();
    emit runCompleted(!m_report.criticalError.isEmpty(), m_report.cancelled);
    m_state.reset();
    emit finished();
}

QStringList PrBot::readForums() const
{
    QStringList input;
    if (!m_config.onlyForums.isEmpty()) input = m_config.onlyForums;
    else if (m_config.useForumTable) {
        for (const auto &row : m_config.targets) if (row.enabled) input.append(row.url);
    } else {
        QFile file(m_config.forumsFile);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            throw std::runtime_error("Не удалось открыть список форумов.");
        QTextStream in(&file);
        in.setEncoding(QStringConverter::Utf8);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (!line.isEmpty() && !line.startsWith('#')) input.append(line);
        }
    }
    QStringList result;
    QSet<QString> seen;
    for (const auto &line : input) {
        const QString url = normalizeUrl(line), key = ForumLinks::forumIdentity(url);
        if (!seen.contains(key)) { seen.insert(key); result.append(url); }
    }
    return result;
}

QString PrBot::normalizeUrl(const QString &input) const
{
    const QString url = ForumLinks::normalizeForum(input);
    if (url.isEmpty()) throw std::runtime_error(QString("Некорректная ссылка форума: %1").arg(input).toStdString());
    return url;
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

void PrBot::prepareSource(WebDriverClient &driver)
{
    try {
        m_currentUrl = m_ancestorUrl;
        setStage("Исходный форум", QString("Открываю %1").arg(m_ancestorUrl));
        navigate(driver, m_ancestorUrl);
        enterForum(driver);
    } catch (const BotCancelled &) {
        throw;
    } catch (const std::exception &error) {
        recordFailure(&driver, QString::fromUtf8(error.what()));
        log(QString("Автоматический поиск исходной темы не удался: %1")
                .arg(QString::fromUtf8(error.what())), LogLevel::Warning);
        setStage("Исходная тема", QString("Открываю тему из настроек: %1").arg(m_ancestorTopic));
        navigate(driver, m_ancestorTopic);
        waitForTopic(driver);
    }
    if (!jsBool(driver, "return !!document.querySelector('#main-reply');")) throw ThemeEnded();
    ForumPage(driver, [this] { checkStop(); }).checkReady();
    m_actualSourceForum = ForumLinks::forumFromTopic(driver.currentUrl());
    const QString suspect = suspiciousForumLinks(driver, {m_ancestorUrl, m_actualSourceForum});
    if (!suspect.isEmpty())
        throw std::runtime_error(("Исходная тема не прошла проверку: " + suspect
            + " Обход остановлен до публикаций.").toStdString());
    log(QString("Исходная тема: %1; форма ответа доступна.").arg(driver.currentUrl()));
    m_sourcePrepared = true;
}

void PrBot::saveProgress()
{
    if (!m_state) return;
    if (!m_config.password.isEmpty()) m_progressState.detail.replace(m_config.password, "[скрыто]");
    m_state->store(m_progressState);
    emit forumUpdated(m_progressState.url, m_progressState.status(), m_progressState.detail,
                      m_progressState.topicUrl, m_progressState.sourceLink.isEmpty()
                          ? m_progressState.sourceProof.value("topic_url").toString() : m_progressState.sourceLink,
                      m_progressState.targetLink);
}

void PrBot::recoveryLogin(WebDriverClient &driver, const QString &forum, const QString &author)
{
    if (author.toLongLong() <= 0) throw std::runtime_error("Не сохранён ID автора для безопасного продолжения.");
    m_currentUrl = forum;
    navigate(driver, forum);
    // Существующая сессия может принадлежать иному PR-аккаунту.
    if (jsString(driver, "return typeof UserID !== 'undefined' && typeof GroupID !== 'undefined' && String(GroupID) !== '3' ? String(UserID) : ''; ") == author) return;
    if (jsBool(driver, "return typeof UserID !== 'undefined' && Number(UserID) > 0 && String(GroupID) !== '3';")) {
        m_userId = jsString(driver, "return String(UserID);");
        forumLogout(driver);
    }
    bool ready;
    if (m_config.useAccount) {
        if (m_config.password.isEmpty()) throw std::runtime_error("Введите пароль для продолжения.");
        forcedLogin(driver);
        ready = openAccountTopic(driver, false, author);
    } else ready = firstEnter(driver, false, author);
    if (!ready) throw std::runtime_error("Не удалось войти в прежний PR-аккаунт. Нужна ручная сверка.");
}

void PrBot::recoverForum(WebDriverClient &driver)
{
    QScopedValueRollback<bool> guard(m_finishingPublication, false);
    auto &saved = m_progressState;
    m_currentResult.topicUrl = saved.topicUrl;
    m_currentResult.sourcePageUrl = saved.sourceLink;
    m_currentResult.targetPageUrl = saved.targetLink;
    const QString target = saved.url;
    auto sourceProof = saved.sourceProof;
    setStage("Восстановление", "Сверяю сохранённую пару по автору, новым ID и содержимому сообщений.");
    driver.switchToWindow(m_parentWindow);
    recoveryLogin(driver, m_ancestorUrl, sourceProof.value("author").toString());
    const QString actualSource = landingForum(driver);
    const auto sourceBelongs = [&](const QString &url) {
        return ForumLinks::belongsToForum(url, m_ancestorUrl) || ForumLinks::belongsToForum(url, actualSource);
    };
    if (!sourceBelongs(sourceProof.value("topic_url").toString())
        || (!saved.sourceLink.isEmpty() && !sourceBelongs(saved.sourceLink)))
        throw std::runtime_error("Сохранённая исходная тема не соответствует форуму. Нужна ручная сверка.");
    if (!saved.sourceLink.isEmpty()) sourceProof.insert("topic_url", saved.sourceLink);
    ForumPage page(driver, [this] { checkStop(); });
    const QString source = page.findPublished(sourceProof);
    if (source.isEmpty()) {
        saved.review = true;
        saveProgress();
        throw std::runtime_error("Первое сообщение не найдено при сверке. Повторная отправка заблокирована; проверьте обе темы вручную.");
    }
    saved.sourceLink = source;
    saved.review = false;
    m_currentResult.sourcePageUrl = source;
    if (saved.phase == "source_submitting") saved.phase = "source_confirmed";
    saveProgress();
    driver.switchToWindow(m_forumWindow);
    recoveryLogin(driver, target, saved.targetAuthor);
    const QString actualTarget = landingForum(driver);
    const auto targetBelongs = [&](const QString &url) {
        return ForumLinks::belongsToForum(url, target) || ForumLinks::belongsToForum(url, actualTarget);
    };
    if (!targetBelongs(saved.topicUrl)) throw std::runtime_error("Сохранённая целевая тема не соответствует форуму.");
    const QString finalMessage = m_config.prCode + " [url=" + source + "]ваша реклама[/url]";
    auto targetProof = saved.targetProof;
    const bool wasSubmitted = saved.phase == "target_submitting";
    if (!wasSubmitted) targetProof = QJsonObject{{"topic_url", saved.topicUrl},
        {"author", saved.targetAuthor}, {"message", finalMessage}, {"backlink", source}};
    if (!targetBelongs(targetProof.value("topic_url").toString()))
        throw std::runtime_error("Некорректная целевая тема для сверки.");
    const QString existing = page.findPublished(targetProof, wasSubmitted);
    if (!existing.isEmpty()) {
        saved.targetLink = existing;
        log("Обе публикации найдены. Повторные сообщения не отправлялись.");
    } else if (wasSubmitted) {
        saved.review = true;
        saveProgress();
        throw std::runtime_error("Результат второй отправки не удалось сверить. Автоматический повтор заблокирован; откройте обе темы из таблицы.");
    } else {
        // Первый ответ подтверждён, второй ещё не отправлялся или был явно отклонён.
        navigate(driver, saved.topicUrl);
        waitForTopic(driver);
        m_finishingPublication = true;
        try {
            saved.targetLink = page.publish(finalMessage, source, {}, 15000, [this](const QJsonObject &proof) {
                if (proof.value("author").toString() != m_progressState.targetAuthor)
                    throw std::runtime_error("Перед отправкой изменился целевой PR-аккаунт. Проход остановлен.");
                m_progressState.targetProof = proof;
                m_progressState.phase = "target_submitting";
                m_progressState.detail = "Вторая отправка подготовлена; ожидается подтверждение ответа со ссылкой на взаимность.";
                saveProgress();
            });
        } catch (const PublicationError &error) {
            if (error.status() == PublicationError::Status::Rejected) {
                saved.phase = "source_confirmed";
                saved.targetProof = {};
                saveProgress();
            }
            throw;
        }
    }
    saved.review = false;
    saved.phase = "complete";
    m_currentResult.targetPageUrl = saved.targetLink;
    saveProgress();
    m_currentUrl = target;
    m_sourcePrepared = false;
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

bool PrBot::firstEnter(WebDriverClient &driver, bool selectTopic, const QString &expectedAuthor)
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
        if (openAccountTopic(driver, selectTopic, expectedAuthor))
            return true;
        if (index + 1 < scripts.size()
            && jsBool(driver, "return typeof GroupID !== 'undefined' && String(GroupID) !== '3';"))
            forumLogout(driver);
    }
    return false;
}

bool PrBot::openAccountTopic(WebDriverClient &driver, bool selectTopic, const QString &expectedAuthor)
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
    if (!expectedAuthor.isEmpty() && m_userId != expectedAuthor) return false;
    return selectTopic ? getPrMessages(driver) : true;
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

QString PrBot::suspiciousForumLinks(WebDriverClient &driver, const QStringList &localForums)
{
    setStage("Анализ ссылок в теме", "Проверяю, на сколько разных форумов ссылаются сообщения текущей страницы.");
    const auto analysis = ForumPage(driver, [this] { checkStop(); }).analyseForumLinks(localForums);
    log(QString("Анализ страницы %1: сообщений без блока шаблона — %2; сообщений с внешними ссылками — %3; разных площадок — %4. Минимум для вывода — %5 сообщения.")
        .arg(driver.currentUrl()).arg(analysis.scannedPosts).arg(analysis.linkedPosts)
        .arg(analysis.postsByForum.size()).arg(ForumLinkAnalysis::MinLinkedPosts));
    int listed = 0;
    for (auto it = analysis.postsByForum.cbegin(); it != analysis.postsByForum.cend(); ++it) {
        if (listed++ == 20) {
            log(QString("Ещё площадок в анализе: %1.").arg(analysis.postsByForum.size() - 20));
            break;
        }
        log(QString("Площадка %1 встречается в %2 сообщениях.").arg(it.key()).arg(it.value()));
    }
    if (!analysis.singleForumOnly()) return {};
    return QString("Возможная партнёрка: во всех %1 сообщениях с внешними ссылками найдена только одна площадка — %2.")
        .arg(analysis.linkedPosts).arg(analysis.postsByForum.firstKey());
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
    const QString suspect = suspiciousForumLinks(driver, {forumUrl, actualTarget});
    if (!suspect.isEmpty())
        throw SkipForum(SkipReason::WrongTheme, suspect + " Форум пропущен до следующего прохода.");
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

    m_progressState.topicUrl = m_currentResult.topicUrl;
    m_progressState.targetAuthor = m_userId;
    ForumPage page(driver, [this] { checkStop(); });
    QString postLink;
    try {
        postLink = page.publish(topicPostHtml, {}, [this] {
            checkStop();
            // После начала первой отправки завершаем пару даже при запросе остановки.
            // Принудительное прерывание между отправками оставило бы взаимность без ответа.
            m_finishingPublication = true;
            log("Отправляю сообщение и ожидаю подтверждения его появления.");
        }, 15000, [this](const QJsonObject &proof) {
            m_progressState.sourceProof = proof;
            m_progressState.phase = "source_submitting";
            m_progressState.detail = "Первая отправка подготовлена; ожидается подтверждение нового сообщения.";
            saveProgress();
        });
    } catch (const PublicationError &error) {
        if (error.status() == PublicationError::Status::Rejected) {
            m_progressState.phase = "failed";
            saveProgress();
        }
        throw;
    }
    m_progressState.sourceLink = postLink;
    m_progressState.phase = "source_confirmed";
    m_progressState.detail = "Взаимная реклама подтверждена; осталось разместить наш ответ.";
    saveProgress();
    m_currentResult.sourcePageUrl = postLink;
    log(QString("Новое сообщение подтверждено: %1").arg(postLink));

    setStage("Отправка на целевой форум", "Добавляю к нашему шаблону ссылку на взаимную рекламу.");
    driver.switchToWindow(m_forumWindow);
    const QString finalMessage = m_config.prCode + " [url=" + postLink + "]ваша реклама[/url]";
    try {
        m_currentResult.targetPageUrl = page.publish(finalMessage, postLink, [this] {
            log("Отправляю ответную рекламу и проверяю новое сообщение вместе со ссылкой на взаимность.");
        }, 15000, [this](const QJsonObject &proof) {
            if (proof.value("author").toString() != m_progressState.targetAuthor)
                throw std::runtime_error("Перед отправкой изменился целевой PR-аккаунт. Проход остановлен.");
            m_progressState.targetProof = proof;
            m_progressState.phase = "target_submitting";
            m_progressState.detail = "Вторая отправка подготовлена; ожидается подтверждение ответа со ссылкой на взаимность.";
            saveProgress();
        });
    } catch (const PublicationError &error) {
        if (error.status() == PublicationError::Status::Rejected) {
            m_progressState.phase = "source_confirmed";
            m_progressState.targetProof = {};
            saveProgress();
        }
        throw;
    }
    m_progressState.targetLink = m_currentResult.targetPageUrl;
    m_progressState.phase = "complete";
    saveProgress();
    log(QString("Новое сообщение подтверждено: %1").arg(m_currentResult.targetPageUrl));
}

void PrBot::diagnosticWarning()
{
    if (m_diagnosticWriteWarningSent || !m_diagnostics) return;
    m_diagnosticWriteWarningSent = true;
    log("Диагностика недоступна: " + m_diagnostics->errorString(), LogLevel::Warning);
}

void PrBot::recordFailure(WebDriverClient *driver, const QString &message)
{
    if (!m_diagnostics) return;
    // Снимок — наблюдение за ошибкой. Даже недоступный диск или закрытый
    // браузер не должны менять решение о продолжении/остановке публикаций.
    try {
        if (!m_diagnostics->recordFailure(driver, m_currentUrl, m_stage, message,
                m_config.checkOnly ? QString("check") : m_progressState.phase)) diagnosticWarning();
        else if (m_diagnostics->screenshotError().isEmpty())
            log("Диагностика ошибки сохранена: адрес, этап и снимок страницы.");
        else log("Диагностика ошибки сохранена. Снимок недоступен: " + m_diagnostics->screenshotError(), LogLevel::Warning);
    } catch (const std::exception &error) {
        log("Не удалось собрать диагностику: " + QString::fromUtf8(error.what()), LogLevel::Warning);
    }
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
