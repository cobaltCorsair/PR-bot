#include "../src/config.h"
#include "../src/forum_links.h"
#include "../src/forum_list_model.h"
#include "../src/run_state.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool value, const char *why) { if (!value) throw std::runtime_error(why); }
QByteArray read(const QString &path) { QFile f(path); require(f.open(QIODevice::ReadOnly), "Read fixture"); return f.readAll(); }
template<class F> void rejects(F fn, const char *why) {
    bool rejected = false; try { fn(); } catch (const std::exception &) { rejected = true; } require(rejected, why);
}
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    try {
        require(ForumLinks::belongsToForum("https://www.Forum.example/a/viewtopic.php?id=1", "http://forum.example/a"), "Canonical forum mismatch");
        for (const auto &url : {"https://forum.example.evil/a", "https://evil.example/?url=https://forum.example/a",
                               "https://forum.example/ab", "https://forum.example:444/a"})
            require(!ForumLinks::belongsToForum(url, "https://forum.example/a"), "False forum match");
        require(ForumLinks::containsForum(ForumLinks::fromBbCode("[URL='https://forum.example/a'][IMG]https://forum.example/a/x.jpg[/IMG][/URL]"), "https://forum.example/a"), "BBCode URL not found");
        require(ForumLinks::normalizeForum("example.com/") == "https://example.com", "Normalization");
        require(ForumLinks::normalizeForum("https://user:secret@example.com").isEmpty(), "Credential URL accepted");

        BotConfig config;
        config.threadUrl = "https://source.example/viewtopic.php?id=1";
        config.prCode = "[b]Public advertisement[/b]";
        config.password = "secret-never-persist";
        config.useForumTable = true;
        config.targets = {{"https://one.example", true}, {"https://two.example", false}};
        config.onlyForums = {"https://two.example"}; config.resumeRun = true; config.checkOnly = true;
        const auto restored = BotConfig::fromJson(config.toJson());
        require(restored.targets.size() == 2 && !restored.targets[1].enabled && restored.password.isEmpty()
            && restored.onlyForums.isEmpty() && !restored.resumeRun && !restored.checkOnly, "Config runtime fields persisted");
        config.resumeRun = config.checkOnly = false;
        const QString path = directory.filePath("progress.json");
        {
            RunState state(path);
            state.begin(config, {"https://one.example", "https://two.example"});
            require(state.unfinished().size() == 2, "Initial queue missing");
            auto entry = state.entry("https://one.example");
            entry.phase = "source_submitting";
            entry.sourceProof = QJsonObject{{"author", "9"}, {"message", "Public content"}};
            state.store(entry);
            const auto bytes = read(path);
            require(!bytes.contains(config.password.toUtf8()) && !bytes.contains("password"), "Checkpoint contains password");
            RunState competing(path);
            rejects([&] { competing.begin(config, {entry.url}); }, "Concurrent run accepted");
            require(read(path) == bytes, "Rejected writer changed checkpoint");
        }
        {
            RunState state(path);
            state.begin(config, {"https://one.example"});
            require(state.entry("http://www.one.example/").phase == "source_submitting", "Normal retry erased partial pair");
            auto entry = state.entry("https://two.example"); entry.phase = "complete"; state.store(entry);
        }
        {
            RunState state(path); config.resumeRun = true;
            state.begin(config, {"https://one.example", "https://two.example"});
            require(state.unfinished({"https://two.example"}).isEmpty() && state.unfinished().size() == 1,
                    "Resume included a completed or disabled forum");
        }
        {
            auto changed = config; changed.prCode += " changed";
            RunState state(path);
            rejects([&] { state.begin(changed, {"https://one.example"}); }, "Different campaign replaced unfinished state");
        }
        { RunState state(path); state.resolve("https://one.example", false); }
        { RunState state(path); require(state.load() && state.entry("https://one.example").phase == "pending", "Manual absence did not reset"); }
        {
            RunState state(path); config.resumeRun = false; state.begin(config, {"https://one.example"});
            auto entry = state.entry("https://one.example"); entry.phase = "target_submitting"; state.store(entry);
        }
        { RunState state(path); state.resolve("https://one.example", true); }
        { RunState state(path); require(state.load() && !state.hasUnfinished(), "Manual completion failed"); }
        {
            RunState unavailable(directory.filePath("missing/progress.json"));
            rejects([&] { unavailable.begin(config, {"https://one.example"}); }, "Unwritable state accepted");
        }
        QFile broken(path); require(broken.open(QIODevice::WriteOnly | QIODevice::Truncate), "Corruption fixture");
        broken.write("{broken"); broken.close();
        { RunState state(path); rejects([&] { state.load(); }, "Corrupt state accepted"); }

        ForumListModel model;
        model.setTargets({{"https://one.example/", true}, {"http://www.one.example", true},
                          {"broken URL", true}, {"https://two.example", false}});
        require(model.rowCount() == 3 && model.enabledUrls().size() == 1, "Table normalization/deduplication");
        require(!model.setData(model.index(1, 0), Qt::Checked, Qt::CheckStateRole), "Invalid address enabled");
        require(!model.setData(model.index(2, 0), "https://one.example", Qt::EditRole), "Duplicate address edited");
        require(model.setData(model.index(2, 0), Qt::Checked, Qt::CheckStateRole) && model.enabledUrls().size() == 2, "Enable toggle failed");
        model.updateResult("http://www.one.example/", "Успешно", "Pair confirmed", "https://one.example/topic", "https://source.example/#p1", "https://one.example/#p2");
        require(model.data(model.index(0, 1)).toString() == "Успешно" && model.link(model.index(0, 5)).endsWith("#p2"), "Result mapping failed");
        const QString list = directory.filePath("forums.txt"); model.exportFile(list);
        model.importFile(list); require(model.rowCount() == 2, "Import/export did not preserve enabled URLs");
        model.removeRowsAt({0, 0}); require(model.rowCount() == 1, "Row removal failed");
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Forum identities, table operations and durable progress checks passed.\n";
}
