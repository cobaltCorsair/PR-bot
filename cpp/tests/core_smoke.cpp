#include "../src/config.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <exception>
#include <iostream>

namespace {
int fail(const char *message)
{
    std::cerr << message << '\n';
    return 1;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid())
        return fail("Temporary directory could not be created.");

    const QString path = directory.filePath("settings.json");
    BotConfig config;
    config.prCode = "[b]PR[/b]";
    config.threadUrl = "https://forum.example/viewtopic.php?id=14";
    config.forumsFile = "/tmp/forums.txt";
    config.chromeDriverPath = "/usr/local/bin/chromedriver";
    config.useAccount = true;
    config.login = "test-user";
    config.password = "must-not-be-saved";
    config.checkLastPage = false;

    try {
        config.save(path);
        const BotConfig loaded = BotConfig::load(path);
        if (loaded.prCode != config.prCode || loaded.threadUrl != config.threadUrl
            || loaded.forumsFile != config.forumsFile
            || loaded.chromeDriverPath != config.chromeDriverPath
            || loaded.login != config.login || loaded.checkLastPage != config.checkLastPage)
            return fail("Saved settings did not round-trip.");
        if (!loaded.password.isEmpty())
            return fail("A password was restored from settings.");

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return fail("Saved settings could not be read.");
        const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        if (json.contains("password"))
            return fail("Password key was written to settings.");

        QFile legacy(path);
        if (!legacy.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return fail("Legacy settings fixture could not be written.");
        legacy.write("{\"account_check\":true,\"login\":\"legacy-user\","
                     "\"password\":\"legacy-secret\"}");
        legacy.close();
        const BotConfig migrated = BotConfig::load(path);
        if (migrated.login != "legacy-user" || !migrated.password.isEmpty())
            return fail("Legacy password was not safely ignored.");
    } catch (const std::exception &error) {
        std::cerr << "Core test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "Core settings checks passed.\n";
    return 0;
}
