#include "forum_list_model.h"
#include "forum_links.h"
#include <QFile>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <algorithm>
#include <stdexcept>

ForumListModel::ForumListModel(QObject *parent) : QAbstractTableModel(parent) {}
int ForumListModel::rowCount(const QModelIndex &p) const { return p.isValid() ? 0 : int(m_rows.size()); }
int ForumListModel::columnCount(const QModelIndex &p) const { return p.isValid() ? 0 : Columns; }
QVariant ForumListModel::headerData(int col, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole) return {};
    if (orientation == Qt::Vertical) return col + 1;
    const QStringList names{"Форум", "Состояние", "Причина / результат", "Тема", "Взаимность", "Наш ответ"};
    return names.value(col);
}
QString ForumListModel::link(const QModelIndex &index) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) return {};
    const auto &r = m_rows[index.row()];
    switch (index.column()) { case Topic: return r.topic; case Source: return r.source;
    case Target: return r.answer; default: return {}; }
}
QVariant ForumListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) return {};
    const auto &r = m_rows[index.row()];
    if (role == Qt::CheckStateRole && index.column() == Forum) return r.target.enabled ? Qt::Checked : Qt::Unchecked;
    if (role == Qt::ToolTipRole) return index.column() >= Topic ? link(index) : r.detail;
    if (role != Qt::DisplayRole && role != Qt::EditRole) return {};
    switch (index.column()) {
    case Forum: return r.target.url;
    case Status: return r.status.isEmpty() ? QString("Ожидает") : r.status;
    case Detail: return r.detail;
    default: return link(index).isEmpty() ? QString() : QString("Открыть ↗");
    }
}
Qt::ItemFlags ForumListModel::flags(const QModelIndex &index) const
{
    auto result = QAbstractTableModel::flags(index);
    if (index.isValid() && index.column() == Forum) result |= Qt::ItemIsEditable | Qt::ItemIsUserCheckable;
    return result;
}
bool ForumListModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() >= m_rows.size() || index.column() != Forum) return false;
    auto &row = m_rows[index.row()];
    if (role == Qt::CheckStateRole) {
        if (ForumLinks::forumIdentity(row.target.url).isEmpty()) return false;
        row.target.enabled = value.toInt() == Qt::Checked;
    } else if (role == Qt::EditRole) {
        const QString url = ForumLinks::normalizeForum(value.toString());
        if (url.isEmpty()) return false;
        for (int i = 0; i < m_rows.size(); ++i)
            if (i != index.row() && ForumLinks::forumIdentity(m_rows[i].target.url) == ForumLinks::forumIdentity(url)) return false;
        if (row.target.url == url) return true;
        row = Row{{url, row.target.enabled}, {}, {}, {}, {}, {}};
    } else return false;
    emit dataChanged(this->index(index.row(), 0), this->index(index.row(), Columns - 1));
    return true;
}
void ForumListModel::setTargets(const QList<ForumTarget> &targets)
{
    beginResetModel();
    m_rows.clear();
    QSet<QString> seen;
    for (auto target : targets) {
        const QString normalized = ForumLinks::normalizeForum(target.url);
        const QString key = ForumLinks::forumIdentity(normalized);
        if (!key.isEmpty() && seen.contains(key)) continue;
        Row row;
        row.target = target;
        if (normalized.isEmpty()) {
            row.target.enabled = false;
            row.status = "Некорректный адрес";
            row.detail = "Исправьте адрес, затем включите строку.";
        } else { row.target.url = normalized; seen.insert(key); }
        m_rows.append(row);
    }
    endResetModel();
}
QList<ForumTarget> ForumListModel::targets() const
{
    QList<ForumTarget> result;
    for (const auto &r : m_rows) result.append(r.target);
    return result;
}
QStringList ForumListModel::enabledUrls() const
{
    QStringList result;
    for (const auto &r : m_rows) if (r.target.enabled) result.append(r.target.url);
    return result;
}
bool ForumListModel::addUrl(const QString &url)
{
    const QString normalized = ForumLinks::normalizeForum(url);
    if (normalized.isEmpty()) return false;
    for (const auto &r : m_rows)
        if (ForumLinks::forumIdentity(r.target.url) == ForumLinks::forumIdentity(normalized)) return false;
    const int last = rowCount();
    beginInsertRows({}, last, last);
    Row row;
    row.target = {normalized, true};
    m_rows.append(row);
    endInsertRows();
    return true;
}
void ForumListModel::removeRowsAt(QList<int> rows)
{
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    for (int row : rows) if (row >= 0 && row < m_rows.size()) {
        beginRemoveRows({}, row, row); m_rows.removeAt(row); endRemoveRows();
    }
}
void ForumListModel::importFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) throw std::runtime_error("Не удалось прочитать список форумов.");
    QList<ForumTarget> rows;
    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    while (!in.atEnd()) {
        const QString text = in.readLine().trimmed();
        if (!text.isEmpty() && !text.startsWith('#')) rows.append({text, true});
    }
    setTargets(rows);
}
void ForumListModel::exportFile(const QString &path) const
{
    QSaveFile file(path);
    const auto data = (enabledUrls().join('\n') + '\n').toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        throw std::runtime_error("Не удалось сохранить список форумов.");
}
void ForumListModel::updateResult(const QString &url, const QString &status, const QString &detail,
                                const QString &topic, const QString &source, const QString &target)
{
    for (int i = 0; i < m_rows.size(); ++i) {
        auto &r = m_rows[i];
        if (ForumLinks::forumIdentity(r.target.url) != ForumLinks::forumIdentity(url)) continue;
        r.status = status; r.detail = detail; r.topic = topic; r.source = source; r.answer = target;
        emit dataChanged(index(i, Status), index(i, Columns - 1));
        return;
    }
}
