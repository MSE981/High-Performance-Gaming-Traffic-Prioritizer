#pragma once
#include <QString>

namespace Scalpel::GUI {

    // 全局暗色主题 QSS 样式表
    // 针对 800x1280 DSI 触摸屏优化：大按钮(48px)、大字体、高对比度
    inline const QString DARK_STYLESHEET = R"QSS(

    /* ===== 全局基础 ===== */
    QWidget {
        background-color: #1a1a2e;
        color: #e0e0e0;
        font-family: "Noto Sans CJK SC", "WenQuanYi Micro Hei", sans-serif;
        font-size: 14px;
    }

    QMainWindow {
        background-color: #0f0f1a;
    }

    /* ===== 左侧导航栏 ===== */
    QListWidget#nav_list {
        background-color: #12122a;
        border: none;
        border-right: 1px solid #2a2a4a;
        outline: none;
        padding: 4px 0px;
    }

    QListWidget#nav_list::item {
        color: #a0a0c0;
        padding: 14px 16px;
        border-left: 3px solid transparent;
        font-size: 15px;
    }

    QListWidget#nav_list::item:selected {
        background-color: #1e1e3a;
        color: #ffffff;
        border-left: 3px solid #0077ff;
        font-weight: bold;
    }

    QListWidget#nav_list::item:hover:!selected {
        background-color: #1a1a35;
        color: #c0c0e0;
    }

    /* ===== 标题栏 ===== */
    QFrame#header_frame {
        background-color: #12122a;
        border-bottom: 1px solid #2a2a4a;
        min-height: 48px;
    }

    QLabel#header_title {
        color: #ffffff;
        font-size: 18px;
        font-weight: bold;
    }

    /* ===== 状态栏 ===== */
    QStatusBar {
        background-color: #12122a;
        border-top: 1px solid #2a2a4a;
        color: #808090;
        font-size: 12px;
    }

    /* ===== 表单控件 ===== */
    QLabel {
        color: #c0c0d0;
        padding: 2px 0px;
    }

    QLabel#section_title {
        color: #ffffff;
        font-size: 16px;
        font-weight: bold;
        padding: 8px 0px 4px 0px;
        border-bottom: 1px solid #2a2a4a;
        margin-bottom: 8px;
    }

    QLabel#field_label {
        color: #a0a0b0;
        font-size: 13px;
        min-width: 120px;
    }

    QLineEdit {
        background-color: #22223a;
        border: 1px solid #3a3a5a;
        border-radius: 4px;
        padding: 8px 12px;
        color: #ffffff;
        font-size: 14px;
        min-height: 20px;
    }

    QLineEdit:focus {
        border: 1px solid #0077ff;
    }

    QLineEdit:disabled {
        background-color: #1a1a2e;
        color: #606070;
    }

    /* ===== 复选框 (触控友好 48px) ===== */
    QCheckBox {
        spacing: 10px;
        color: #c0c0d0;
        font-size: 14px;
        padding: 6px 4px;
    }

    QCheckBox::indicator {
        width: 22px;
        height: 22px;
        border: 2px solid #4a4a6a;
        border-radius: 4px;
        background-color: #22223a;
    }

    QCheckBox::indicator:checked {
        background-color: #0077ff;
        border-color: #0077ff;
    }

    QCheckBox::indicator:hover {
        border-color: #0077ff;
    }

    /* ===== 按钮 (触控友好) ===== */
    QPushButton {
        background-color: #2a2a4a;
        border: 1px solid #3a3a5a;
        border-radius: 6px;
        padding: 10px 20px;
        color: #e0e0e0;
        font-size: 14px;
        font-weight: bold;
        min-height: 28px;
    }

    QPushButton:hover {
        background-color: #3a3a5a;
        border-color: #0077ff;
    }

    QPushButton:pressed {
        background-color: #0055cc;
    }

    QPushButton#btn_primary {
        background-color: #0077ff;
        border-color: #0066dd;
        color: #ffffff;
    }

    QPushButton#btn_primary:hover {
        background-color: #0088ff;
    }

    QPushButton#btn_primary:pressed {
        background-color: #0055cc;
    }

    QPushButton#btn_danger {
        background-color: #cc3333;
        border-color: #aa2222;
        color: #ffffff;
    }

    QPushButton#btn_danger:hover {
        background-color: #dd4444;
    }

    /* ===== Tab 组件 ===== */
    QTabWidget::pane {
        border: 1px solid #2a2a4a;
        border-radius: 4px;
        background-color: #1a1a2e;
    }

    QTabBar::tab {
        background-color: #22223a;
        border: 1px solid #2a2a4a;
        border-bottom: none;
        padding: 10px 20px;
        color: #a0a0b0;
        font-size: 14px;
        border-top-left-radius: 4px;
        border-top-right-radius: 4px;
        margin-right: 2px;
    }

    QTabBar::tab:selected {
        background-color: #1a1a2e;
        color: #ffffff;
        font-weight: bold;
        border-bottom: 2px solid #0077ff;
    }

    QTabBar::tab:hover:!selected {
        background-color: #2a2a4a;
        color: #c0c0d0;
    }

    /* ===== 表格 ===== */
    QTableWidget {
        background-color: #1a1a2e;
        border: 1px solid #2a2a4a;
        gridline-color: #2a2a4a;
        color: #e0e0e0;
        font-size: 13px;
    }

    QTableWidget::item {
        padding: 6px;
    }

    QTableWidget::item:selected {
        background-color: #0077ff;
    }

    QHeaderView::section {
        background-color: #22223a;
        color: #a0a0b0;
        padding: 8px;
        border: 1px solid #2a2a4a;
        font-weight: bold;
        font-size: 13px;
    }

    /* ===== 滚动条 (触控友好加宽) ===== */
    QScrollBar:vertical {
        background-color: #12122a;
        width: 12px;
        border: none;
    }

    QScrollBar::handle:vertical {
        background-color: #3a3a5a;
        border-radius: 6px;
        min-height: 40px;
    }

    QScrollBar::handle:vertical:hover {
        background-color: #4a4a6a;
    }

    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
        height: 0px;
    }

    /* ===== 分组框 ===== */
    QGroupBox {
        border: 1px solid #2a2a4a;
        border-radius: 6px;
        margin-top: 12px;
        padding-top: 16px;
        font-size: 14px;
        font-weight: bold;
        color: #ffffff;
    }

    QGroupBox::title {
        subcontrol-origin: margin;
        left: 12px;
        padding: 0 6px;
    }

    )QSS";

} // namespace Scalpel::GUI
