#pragma once

#include <QStringList>

// Сравниваем адреса по полному хосту, порту и границе каталога форума.
// Случайное упоминание короткого имени в тексте/чужом URL не является ссылкой.
namespace ForumLinks {
QStringList fromBbCode(const QString &message);
bool belongsToForum(const QString &reference, const QString &forumUrl);
bool containsForum(const QStringList &references, const QString &forumUrl);
QString forumFromTopic(const QString &topicUrl);
}
