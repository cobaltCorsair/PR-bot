#pragma once

#include <QString>
#include <QList>
#include <QStringList>
#include <QJsonObject>

struct ForumTarget {
    QString url;
    bool enabled = true;
};

// Снимок формы на один запуск. JSON-ключи совместимы с Python-версией,
// поэтому существующие настройки можно использовать без ручной миграции.
struct BotConfig {
    QString prCode;
    QString threadUrl;
    QString forumsFile;
    QString chromeDriverPath;
    bool useAccount = false;
    QString login;
    QString password; // Только в памяти; не загружается из JSON и не сохраняется.
    static constexpr int MaxPublicationLimit = 1000000;
    int publicationLimit = 0; // Успешные пары за запуск; 0 — без ограничения.
    bool checkLastPage = true;
    bool checkOnly = false; // Режим одного запуска; в настройки не записывается.
    QList<ForumTarget> targets;
    bool useForumTable = false;
    bool resumeRun = false; // Только текущий запуск.
    QStringList onlyForums; // Явный выбор строк, включая выключенные площадки.

    static BotConfig load(const QString &path);
    void save(const QString &path) const;
    QJsonObject toJson() const;
    static BotConfig fromJson(const QJsonObject &json);
};
