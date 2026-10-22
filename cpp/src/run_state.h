#pragma once

#include "config.h"
#include <QJsonObject>
#include <memory>

class QLockFile;

struct ForumProgress {
    QString url;
    QString phase = "pending";
    QString detail;
    QString topicUrl;
    QString sourceLink;
    QString targetLink;
    QString targetAuthor;
    QJsonObject sourceProof;
    QJsonObject targetProof;
    bool review = false;
    bool isPartial() const;
    bool isFinished() const;
    QString status() const;
};

// Атомарная запись до каждого клика и после подтверждения. Пароля в JSON нет.
// Блокировка не позволяет двум экземплярам одновременно вести один проход.
class RunState final {
public:
    explicit RunState(QString path);
    ~RunState();
    bool load();
    void begin(const BotConfig &config, const QStringList &forums);
    void store(const ForumProgress &entry);
    void resolve(const QString &url, bool completed);
    void resolveReplyMissing(const QString &url);
    ForumProgress entry(const QString &url) const;
    QList<ForumProgress> entries() const { return m_entries; }
    QStringList unfinished(const QStringList &allowed = {}) const;
    bool hasUnfinished() const;
    bool hasPartial() const;
    BotConfig campaign() const { return m_campaign; }
    bool matches(const BotConfig &config) const;
    static QString fingerprint(const BotConfig &config);
    static QString pathForReport(const QString &reportPath);

private:
    void lock();
    void save();
    QString m_path;
    BotConfig m_campaign;
    QString m_fingerprint;
    QList<ForumProgress> m_entries;
    std::unique_ptr<QLockFile> m_lock;
};
