#pragma once
#include <QString>
class QWidget;

// Диалог выбирает только место сохранения готовой диагностики; браузер
// здесь не запускается, сведения относятся к выбранному архивному журналу.
void saveDiagnosticsArchive(QWidget *parent, const QString &archiveLogPath);
