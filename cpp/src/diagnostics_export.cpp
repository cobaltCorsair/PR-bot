#include "diagnostics_export.h"
#include "run_diagnostics.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>
#include <stdexcept>

void saveDiagnosticsArchive(QWidget *parent, const QString &archiveLogPath)
{
    const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
        .filePath("PR-Bot-diagnostics_" + QFileInfo(archiveLogPath).completeBaseName() + ".zip");
    const QString path = QFileDialog::getSaveFileName(parent, "Сохранить диагностику", suggested, "ZIP (*.zip)");
    if (path.isEmpty()) return;
    // Проверка расширения защищает от случайной замены журнала/настроек.
    // Не добавляем расширение после проверки overwrite в диалоге: показываем
    // ошибку для неверного имени, чтобы подтверждение относилось к тому же пути.
    if (!path.endsWith(".zip", Qt::CaseInsensitive)) {
        QMessageBox::information(parent, "Имя архива", "Укажите имя файла с расширением .zip.");
        return;
    }
    try {
        struct CursorGuard {
            CursorGuard() { QApplication::setOverrideCursor(Qt::WaitCursor); }
            ~CursorGuard() { QApplication::restoreOverrideCursor(); }
        } cursor;
        RunDiagnostics::exportZip(archiveLogPath, path);
    } catch (const std::exception &error) {
        QMessageBox::warning(parent, "Не удалось сохранить диагностику", QString::fromUtf8(error.what()));
        return;
    }
    QMessageBox::information(parent, "Диагностика сохранена", "Архив с журналом, описанием ошибок и доступными снимками сохранён:\n" + path);
}
