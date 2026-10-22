#pragma once
#include "config.h"
#include "run_state.h"
#include <QAbstractTableModel>

// Настройки строк хранятся отдельно от временных результатов проверки.
class ForumListModel final : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Forum, Status, Detail, Topic, Source, Target, Columns };
    explicit ForumListModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &, int role = Qt::DisplayRole) const override;
    QVariant headerData(int, Qt::Orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &) const override;
    bool setData(const QModelIndex &, const QVariant &, int role = Qt::EditRole) override;
    void setTargets(const QList<ForumTarget> &targets);
    QList<ForumTarget> targets() const;
    QStringList enabledUrls() const;
    bool addUrl(const QString &url);
    void removeRowsAt(QList<int> rows);
    void importFile(const QString &path);
    void exportFile(const QString &path) const;
    void updateResult(const QString &url, const QString &status, const QString &detail,
                      const QString &topic, const QString &source, const QString &target);
    QString link(const QModelIndex &) const;
private:
    struct Row { ForumTarget target; QString status, detail, topic, source, answer; };
    QList<Row> m_rows;
};
