#include "run_state.h"
#include "forum_links.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <stdexcept>
#include <utility>

bool ForumProgress::isPartial() const
{
    return phase == "source_submitting" || phase == "source_confirmed" || phase == "target_submitting";
}
bool ForumProgress::isFinished() const { return phase == "complete" || phase == "skipped"; }
QString ForumProgress::status() const
{
    if (review || phase == "source_submitting" || phase == "target_submitting") return "Нужна сверка";
    if (phase == "complete") return "Успешно";
    if (phase == "skipped") return "Пропущен";
    if (phase == "failed") return "Ошибка";
    if (phase == "source_confirmed") return "Взаимность размещена";
    return "Ожидает";
}

RunState::RunState(QString path) : m_path(std::move(path)) {}
RunState::~RunState() = default;
QString RunState::pathForReport(const QString &path) { return QFileInfo(path).absoluteDir().filePath("progress.json"); }

QString RunState::fingerprint(const BotConfig &config)
{
    const QJsonObject identity{{"code", config.prCode}, {"topic", config.threadUrl.trimmed()},
        {"account", config.useAccount}, {"login", config.useAccount ? config.login : QString()}};
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact),
                                                       QCryptographicHash::Sha256).toHex());
}
bool RunState::matches(const BotConfig &config) const { return m_fingerprint == fingerprint(config); }

bool RunState::load()
{
    QFile file(m_path);
    if (!file.exists()) return false;
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Не удалось прочитать progress.json.");
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    const auto root = document.object();
    if (error.error != QJsonParseError::NoError || !document.isObject()
        || root.value("version").toInt() != 1 || !root.value("entries").isArray())
        throw std::runtime_error("Некорректный progress.json. Сохраните его копию и проверьте незавершённые публикации.");
    m_campaign = BotConfig::fromJson(root.value("campaign").toObject());
    m_fingerprint = root.value("fingerprint").toString();
    if (!matches(m_campaign)) throw std::runtime_error("Настройки в progress.json не соответствуют его контрольной сумме.");
    m_entries.clear();
    QSet<QString> identities;
    const QStringList phases{"pending", "failed", "skipped", "complete", "source_submitting",
                             "source_confirmed", "target_submitting"};
    for (const auto &value : root.value("entries").toArray()) {
        const auto item = value.toObject();
        ForumProgress entry;
        entry.url = item.value("url").toString();
        entry.phase = item.value("phase").toString();
        entry.detail = item.value("detail").toString();
        entry.topicUrl = item.value("topic").toString();
        entry.sourceLink = item.value("source_link").toString();
        entry.targetLink = item.value("target_link").toString();
        entry.targetAuthor = item.value("target_author").toString();
        entry.sourceProof = item.value("source_proof").toObject();
        entry.targetProof = item.value("target_proof").toObject();
        entry.review = item.value("review").toBool();
        const QString key = ForumLinks::forumIdentity(entry.url);
        if (key.isEmpty() || identities.contains(key) || !phases.contains(entry.phase))
            throw std::runtime_error("В progress.json есть некорректная или повторная запись форума.");
        identities.insert(key);
        m_entries.append(entry);
    }
    return true;
}

void RunState::lock()
{
    if (m_lock) return;
    m_lock = std::make_unique<QLockFile>(m_path + ".lock");
    // Проход может длиться часами; возраст файла не разрешает захватить живую блокировку.
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock(0)) {
        m_lock.reset();
        throw std::runtime_error("Этот проход уже выполняется в другом экземпляре программы.");
    }
}

void RunState::begin(const BotConfig &config, const QStringList &forums)
{
    lock();
    const bool exists = load();
    if (exists && !matches(config) && hasPartial())
        throw std::runtime_error("Есть незавершённый проход с другими настройками. Нажмите «Продолжить» для их восстановления.");
    if (config.resumeRun && (!exists || !matches(config)))
        throw std::runtime_error("Нет сохранённого прохода с этими настройками.");
    if (!exists || !matches(config)) m_entries.clear();
    m_campaign = config;
    m_campaign.password.clear();
    if (!m_campaign.useForumTable) {
        m_campaign.useForumTable = true;
        m_campaign.targets.clear();
        for (const auto &url : forums) m_campaign.targets.append({url, true});
    }
    m_fingerprint = fingerprint(config);
    if (!config.resumeRun) {
        for (const auto &url : forums) {
            const auto previous = entry(url);
            if (previous.isPartial()) continue; // Сверяем такую пару при любом повторном запуске.
            ForumProgress fresh;
            fresh.url = url;
            bool found = false;
            for (auto &item : m_entries) {
                if (ForumLinks::forumIdentity(item.url) == ForumLinks::forumIdentity(url)) {
                    item = fresh;
                    found = true;
                    break;
                }
            }
            if (!found) m_entries.append(fresh);
        }
    }
    save();
}

ForumProgress RunState::entry(const QString &url) const
{
    const QString key = ForumLinks::forumIdentity(url);
    for (const auto &item : m_entries)
        if (ForumLinks::forumIdentity(item.url) == key) return item;
    ForumProgress fresh;
    fresh.url = url;
    return fresh;
}

void RunState::store(const ForumProgress &entry)
{
    if (!m_lock) throw std::runtime_error("Нет блокировки записи прогресса.");
    for (auto &item : m_entries) {
        if (ForumLinks::forumIdentity(item.url) == ForumLinks::forumIdentity(entry.url)) {
            item = entry;
            save();
            return;
        }
    }
    m_entries.append(entry);
    save();
}

void RunState::resolve(const QString &url, bool completed)
{
    lock();
    if (!load()) throw std::runtime_error("Сохранённый проход не найден.");
    auto item = entry(url);
    if (!item.isPartial()) throw std::runtime_error("Для этой строки нет незавершённой пары.");
    if (completed) {
        item.phase = "complete";
        item.review = false;
        item.detail = "Пара сверена пользователем вручную: обе публикации размещены.";
    } else {
        item = ForumProgress{};
        item.url = url;
        item.detail = "Пользователь проверил обе темы и подтвердил отсутствие публикаций этой попытки.";
    }
    store(item);
}

void RunState::resolveReplyMissing(const QString &url)
{
    lock();
    if (!load()) throw std::runtime_error("Сохранённый проход не найден.");
    auto item = entry(url);
    if (!item.isPartial() || item.sourceProof.isEmpty())
        throw std::runtime_error("Нет сохранённых данных первой публикации для сверки.");
    item.phase = "source_confirmed";
    item.targetProof = {};
    item.targetLink.clear();
    item.review = false;
    item.detail = "Пользователь сверил обе темы: взаимность размещена, нашего ответа нет. Перед продолжением бот повторно проверит первое сообщение.";
    store(item);
}

QStringList RunState::unfinished(const QStringList &allowed) const
{
    QSet<QString> active;
    for (const auto &url : allowed) active.insert(ForumLinks::forumIdentity(url));
    QStringList partial, pending;
    for (const auto &entry : m_entries)
        if (!entry.isFinished() && (allowed.isEmpty() || active.contains(ForumLinks::forumIdentity(entry.url)))) {
            if (entry.isPartial()) partial.append(entry.url);
            else pending.append(entry.url);
        }
    return partial + pending;
}
bool RunState::hasUnfinished() const { return !unfinished().isEmpty(); }
bool RunState::hasPartial() const
{
    for (const auto &entry : m_entries) if (entry.isPartial()) return true;
    return false;
}

void RunState::save()
{
    QJsonArray entries;
    for (const auto &entry : m_entries) {
        entries.append(QJsonObject{{"url", entry.url}, {"phase", entry.phase}, {"detail", entry.detail},
            {"topic", entry.topicUrl}, {"source_link", entry.sourceLink}, {"target_link", entry.targetLink},
            {"target_author", entry.targetAuthor}, {"source_proof", entry.sourceProof},
            {"target_proof", entry.targetProof}, {"review", entry.review}});
    }
    const QJsonObject root{{"version", 1}, {"fingerprint", m_fingerprint}, {"campaign", m_campaign.toJson()},
        {"entries", entries}, {"updated_at", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)}};
    QSaveFile file(m_path);
    file.setDirectWriteFallback(false);
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        throw std::runtime_error("Не удалось сохранить progress.json. Проход остановлен до следующей отправки.");
}
