#include "config.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSaveFile>
#include <stdexcept>

BotConfig BotConfig::load(const QString &path)
{
    QFile file(path);
    if (!file.exists())
        return {};
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error(QString("Не удалось открыть настройки: %1")
                                     .arg(file.errorString()).toStdString());

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error(QString("Некорректный settings.json: %1")
                                     .arg(parseError.errorString()).toStdString());

    return fromJson(document.object());
}

BotConfig BotConfig::fromJson(const QJsonObject &json)
{
    // Сохраняем старые имена ключей, хотя в C++ поля названы иначе.
    BotConfig config;
    config.prCode = json.value("pr_code").toString();
    config.threadUrl = json.value("thread_link").toString();
    config.forumsFile = json.value("list_forums").toString();
    config.chromeDriverPath = json.value("chromedriver_path").toString();
    config.useAccount = json.value("account_check").toBool(false);
    config.login = json.value("login").toString();
    config.checkLastPage = json.value("check_repeat_state").toBool(true);
    config.useForumTable = json.contains("forums");
    for (const auto &value : json.value("forums").toArray()) {
        const auto item = value.toObject();
        config.targets.append({item.value("url").toString(), item.value("enabled").toBool(true)});
    }
    // Старый файл мог содержать пароль открытым текстом. Это поле игнорируем.
    return config;
}

QJsonObject BotConfig::toJson() const
{
    QJsonObject json{
        {"pr_code", prCode},
        {"thread_link", threadUrl},
        {"list_forums", forumsFile},
        {"chromedriver_path", chromeDriverPath},
        {"account_check", useAccount},
        {"login", useAccount ? QJsonValue(login) : QJsonValue(QJsonValue::Null)},
        {"check_repeat_state", checkLastPage},
    };
    if (useForumTable) {
        QJsonArray entries;
        for (const auto &target : targets)
            entries.append(QJsonObject{{"url", target.url}, {"enabled", target.enabled}});
        json.insert("forums", entries);
    }
    return json;
}

void BotConfig::save(const QString &path) const
{

    // QSaveFile заменяет файл только после успешной записи: прежние настройки
    // не потеряются, если сохранение прервётся посередине.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error(QString("Не удалось сохранить настройки: %1")
                                     .arg(file.errorString()).toStdString());
    file.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
    if (!file.commit())
        throw std::runtime_error(QString("Не удалось завершить сохранение настроек: %1")
                                     .arg(file.errorString()).toStdString());
}
