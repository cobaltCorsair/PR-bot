#pragma once

#include <QDialog>

class QLabel;
class QListWidget;
class QPushButton;
class QTextEdit;

// Читает сохранённые журналы как обычный текст. История не запускает обход и
// не восстанавливает публикацию по одному лишь наличию записи в журнале.
class RunHistoryDialog final : public QDialog {
public:
    explicit RunHistoryDialog(const QString &reportPath, QWidget *parent = nullptr);

private:
    void reload();
    void showSelected();
    QString selectedPath() const;

    QString m_directory;
    QListWidget *m_runs = nullptr;
    QTextEdit *m_preview = nullptr;
    QLabel *m_description = nullptr;
    QPushButton *m_openFile = nullptr;
    QPushButton *m_exportDiagnostics = nullptr;
};
