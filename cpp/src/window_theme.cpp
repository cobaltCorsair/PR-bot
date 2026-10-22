#include "window_theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace UiTheme {
QString stylesheet()
{
    return QStringLiteral(R"QSS(
        QMainWindow, QWidget#workspace { background: #f5f5ef; color: #263b34; }
        QWidget { font-family: "Avenir Next", "Segoe UI", sans-serif; font-size: 13px; }
        QLabel { background: transparent; color: #263b34; }
        QFrame#sidebar { background: #183c34; border: none; }
        QLabel#brandMark { background: #c4dfab; color: #183c34; border-radius: 10px;
            font-size: 19px; font-weight: 700; }
        QLabel#brandName { color: #f1f4e9; font-size: 20px; font-weight: 600; }
        QLabel#sideEyebrow { color: #9bb5a9; font-size: 10px; font-weight: 600; }
        QLabel#sideNote { color: #b4c7ba; font-size: 12px; }
        QLabel#sidePlatform { color: #91b5a4; font-size: 10px; font-weight: 600; }
        QPushButton[role="nav"] { text-align: left; background: transparent;
            color: #c5d5cb; border: 1px solid transparent; padding: 12px 14px; border-radius: 9px; }
        QPushButton[role="nav"]:hover { background: #234b40; }
        QPushButton[role="nav"]:checked { color: #f2f5e8; background: #30594b; border-color: #426b59; }
        QLabel#eyebrow { color: #78877b; font-size: 10px; font-weight: 600; }
        QLabel#pageTitle { font-family: Georgia, serif; font-size: 29px; color: #203e32; }
        QLabel#pageDescription { color: #758074; font-size: 12px; }
        QFrame[role="card"] { background: #ffffff; border: 1px solid #e1e5d9; border-radius: 12px; }
        QFrame#targetsCard { background: #edf1e5; border: 1px solid #dce3d1; border-radius: 12px; }
        QLabel[role="sectionTitle"] { font-size: 15px; font-weight: 600; color: #284d3d; }
        QLabel[role="fieldLabel"] { font-size: 12px; font-weight: 600; color: #415747; }
        QLabel[role="hint"] { color: #6f7d69; font-size: 11px; }
        QLabel[role="hint"][state="ready"] { color: #527347; }
        QLabel[role="hint"][state="attention"] { color: #987242; }
        QLabel#forumFileName { color: #38533e; font-weight: 600; font-size: 12px; }
        QLabel#characterCount { color: #83907e; font-size: 11px; }
        QLabel#formatBadge { background: #edf1e7; color: #6d7d65; border-radius: 5px;
            padding: 4px 7px; font-family: Menlo, monospace; font-size: 10px; }
        QFrame#rule { background: #e8ebdf; border: none; max-height: 1px; }
        QLineEdit, QPlainTextEdit { background: #fbfcf8; color: #293c30;
            border: 1px solid #dce2d3; border-radius: 7px; padding: 9px 11px;
            selection-background-color: #b9d6b8; selection-color: #1b372a; }
        QLineEdit:focus, QPlainTextEdit:focus { border: 1px solid #6c997e; background: #ffffff; }
        QLineEdit:disabled { color: #8f9a8b; background: #f2f4ee; }
        QLineEdit[invalid="true"] { border-color: #c78d66; }
        QPlainTextEdit#advertisement { border: none; border-radius: 9px; background: #f6f8f2;
            font-family: Menlo, Consolas, monospace; font-size: 12px; padding: 14px; }
        QPlainTextEdit#advertisement:focus { background: #f2f6ed; }
        QPushButton { background: #ffffff; color: #3d5948; border: 1px solid #d9dfd1;
            border-radius: 7px; padding: 9px 13px; font-weight: 600; }
        QPushButton:hover { background: #f0f4e9; border-color: #b8c8ae; }
        QPushButton:focus { border: 1px solid #6c997e; }
        QPushButton:pressed { background: #e7eddd; }
        QPushButton:disabled { color: #a2ad9b; background: #eff2e9; border-color: #e2e7da; }
        QPushButton[role="quiet"] { background: transparent; border-color: transparent; color: #6d7e68; }
        QPushButton[role="quiet"]:hover { background: #e9eee0; border-color: #e0e5d8; }
        QPushButton#chooseForums { background: #fafcf6; border: 1px dashed #aebfa1;
            padding: 13px; text-align: center; }
        QPushButton#chooseForums:hover { background: #ffffff; border-color: #6c997e; }
        QPushButton#primaryButton { color: #ffffff; background: #28624c; border-color: #28624c;
            padding: 12px 20px; font-size: 13px; }
        QPushButton#primaryButton:hover { background: #1d503e; border-color: #1d503e; }
        QPushButton#primaryButton:disabled { color: #9aa991; background: #e1e8d9; border-color: #dce3d4; }
        QPushButton#stopButton { color: #935738; background: #fbf6ee; border-color: #dfcdb9;
            padding: 12px 17px; }
        QPushButton#stopButton:hover { background: #f6eadd; }
        QPushButton#advancedToggle { text-align: left; padding: 12px 2px;
            background: transparent; border: none; color: #5c7059; font-size: 12px; }
        QCheckBox { color: #405a43; spacing: 9px; font-weight: 500; background: transparent; }
        QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid #a9bba0;
            border-radius: 4px; background: #ffffff; }
        QCheckBox::indicator:checked { background: #4d7b59; border-color: #4d7b59;
            image: none; }
        QCheckBox::indicator:disabled { background: #e6eadf; border-color: #cbd5c2; }
        QComboBox { background: #ffffff; border: 1px solid #dce2d3; border-radius: 7px;
            padding: 9px 12px; color: #52694b; min-width: 160px; }
        QComboBox::drop-down { border: none; width: 22px; }
        QComboBox QAbstractItemView { background: #ffffff; color: #263b34;
            selection-background-color: #e7efdc; selection-color: #183c34; padding: 4px; }
        QScrollArea, QStackedWidget { border: none; background: transparent; }
        QScrollArea > QWidget > QWidget { background: #f5f5ef; }
        QScrollBar:vertical { background: transparent; width: 9px; margin: 2px; }
        QScrollBar::handle:vertical { background: #c9d2c0; border-radius: 3px; min-height: 35px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
        QFrame#footer { background: #fafbf5; border-top: 1px solid #e0e5d7; }
        QLabel#statusDot { border-radius: 4px; background: #b7c6aa; min-width: 8px; max-width: 8px;
            min-height: 8px; max-height: 8px; }
        QLabel#statusDot[state="ready"], QLabel#statusDot[state="done"] { background: #5e8c59; }
        QLabel#statusDot[state="attention"] { background: #c19152; }
        QLabel#statusDot[state="running"] { background: #428e72; }
        QLabel#status { color: #50644b; font-size: 12px; }
        QLabel#status[state="attention"] { color: #906137; }
        QLabel#progressLabel { color: #7c8d73; font-size: 11px; }
        QProgressBar { background: #e5eadb; border: none; min-height: 3px; max-height: 3px; }
        QProgressBar::chunk { background: #6a9564; }
        QFrame#metrics { background: transparent; border: none; }
        QLabel[role="metricValue"] { font-size: 29px; font-weight: 500; color: #2e4e3d; }
        QLabel[role="metricLabel"] { color: #7a8871; font-size: 11px; }
        QLabel[role="metricValue"][tone="success"] { color: #4c8051; }
        QLabel[role="metricValue"][tone="error"] { color: #a66e4b; }
        QLabel#currentStage { color: #375e47; font-size: 15px; font-weight: 600; }
        QLabel#currentForum { color: #7b8973; font-size: 11px; }
        QTextEdit#log { background: #ffffff; border: 1px solid #e0e5d7; border-radius: 10px;
            padding: 12px 16px; color: #41543c; selection-background-color: #dce8ce;
            font-family: Menlo, Consolas, monospace; font-size: 11px; }
        QToolTip { background: #203f32; color: #f3f5eb; border: none; padding: 7px; }
    )QSS");
}

QIcon icon(Icon type, const QString &color)
{
    QPixmap pixmap(40, 40);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(2, 2);
    p.setPen(QPen(QColor(color), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    switch (type) {
    case Icon::Campaign:
        p.drawRoundedRect(QRectF(4, 3, 12, 14), 2, 2);
        p.drawLine(QPointF(7, 7), QPointF(13, 7)); p.drawLine(QPointF(7, 10), QPointF(13, 10));
        p.drawLine(QPointF(7, 13), QPointF(10, 13)); break;
    case Icon::Journal:
        p.drawLine(QPointF(4, 4), QPointF(4, 16)); p.drawLine(QPointF(4, 16), QPointF(17, 16));
        p.drawLine(QPointF(7, 12), QPointF(10, 8)); p.drawLine(QPointF(10, 8), QPointF(13, 10));
        p.drawLine(QPointF(13, 10), QPointF(17, 4)); break;
    case Icon::Folder: {
        QPainterPath path; path.moveTo(2, 5); path.lineTo(8, 5); path.lineTo(10, 7);
        path.lineTo(18, 7); path.lineTo(17, 16); path.lineTo(2, 16); path.closeSubpath(); p.drawPath(path); break;
    }
    case Icon::Save:
        p.drawRoundedRect(QRectF(3, 3, 14, 14), 1, 1); p.drawRect(QRectF(7, 3, 6, 5));
        p.drawRect(QRectF(6, 12, 8, 5)); break;
    case Icon::Load:
        p.drawLine(QPointF(10, 2), QPointF(10, 12)); p.drawLine(QPointF(6, 8), QPointF(10, 12));
        p.drawLine(QPointF(10, 12), QPointF(14, 8)); p.drawLine(QPointF(3, 13), QPointF(3, 17));
        p.drawLine(QPointF(3, 17), QPointF(17, 17)); p.drawLine(QPointF(17, 17), QPointF(17, 13)); break;
    case Icon::Play: {
        QPainterPath path; path.moveTo(6, 3); path.lineTo(16, 10); path.lineTo(6, 17); path.closeSubpath();
        p.setBrush(QColor(color)); p.drawPath(path); break;
    }
    case Icon::Stop: p.drawRoundedRect(QRectF(5, 5, 10, 10), 1, 1); break;
    case Icon::Copy:
        p.drawRoundedRect(QRectF(6, 6, 11, 12), 1, 1); p.drawLine(QPointF(3, 14), QPointF(3, 3));
        p.drawLine(QPointF(3, 3), QPointF(13, 3)); break;
    case Icon::Search: p.drawEllipse(QRectF(3, 3, 10, 10)); p.drawLine(QPointF(12, 12), QPointF(17, 17)); break;
    case Icon::Arrow:
        p.drawLine(QPointF(5, 15), QPointF(15, 5)); p.drawLine(QPointF(6, 5), QPointF(15, 5));
        p.drawLine(QPointF(15, 5), QPointF(15, 14)); break;
    case Icon::Eye:
        p.drawEllipse(QRectF(2, 5, 16, 10)); p.drawEllipse(QRectF(8, 8, 4, 4)); break;
    }
    QIcon result;
    result.addPixmap(pixmap);
    return result;
}
}
