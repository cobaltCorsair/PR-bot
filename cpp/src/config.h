#pragma once

#include <QString>

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
    bool checkLastPage = true;

    static BotConfig load(const QString &path);
    void save(const QString &path) const;
};
