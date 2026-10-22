#pragma once

#include <QIcon>
#include <QString>

namespace UiTheme {
enum class Icon { Campaign, Journal, Folder, Save, Load, Play, Stop, Copy, Search, Arrow, Eye };
QString stylesheet();
QIcon icon(Icon type, const QString &color = "#315c52");
}
