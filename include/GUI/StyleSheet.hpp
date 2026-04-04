#pragma once
#include <QString>

namespace Scalpel::GUI {

    // Global dark theme QSS stylesheet
    // Optimized for 800x1280 DSI display
    inline const QString DARK_STYLESHEET = R"QSS(

    /* ===== Global Basics ===== */
    QWidget {
        background-color: #1a1a2e;
        color: #e0e0e0;
        font-family: "Noto Sans", "Noto Sans CJK SC", "WenQuanYi Micro Hei", sans-serif;
        font-size: 15px;
    }

    QMainWindow {
        background-color: #0f0f1a;
    }

    /* ===== Left Navigation (touch: 220px wide, 64px items) ===== */
    QListWidget#nav_list {
        background-color: #12122a;
        border: none;
        border-right: 1px solid #2a2a4a;
        outline: none;
        padding: 8px 0px;
    }

    QListWidget#nav_list::item {
        color: #a0a0c0;
        padding: 20px 18px;
        border-left: 4px solid transparent;
        font-size: 15px;
        min-height: 64px;
    }

    QListWidget#nav_list::item:selected {
        background-color: #1e1e3a;
        color: #ffffff;
        border-left: 4px solid #0077ff;
        font-weight: bold;
    }

    /* ===== Header Bar (64px tall) ===== */
    QFrame#header_frame {
        background-color: #12122a;
        border-bottom: 1px solid #2a2a4a;
        min-height: 64px;
    }

    QLabel#header_title {
        color: #ffffff;
        font-size: 16px;
        font-weight: bold;
    }

    /* ===== Status Bar ===== */
    QStatusBar {
        background-color: #12122a;
        border-top: 1px solid #2a2a4a;
        color: #808090;
        font-size: 13px;
        min-height: 32px;
    }

    /* ===== Labels ===== */
    QLabel {
        color: #c0c0d0;
        padding: 2px 0px;
    }

    QLabel#section_title {
        color: #ffffff;
        font-size: 17px;
        font-weight: bold;
        padding: 10px 0px 6px 0px;
        border-bottom: 1px solid #2a2a4a;
        margin-bottom: 10px;
    }

    QLabel#field_label {
        color: #a0a0b0;
        font-size: 14px;
        min-width: 120px;
    }

    /* ===== Text Inputs (touch: min-height 48px) ===== */
    QLineEdit {
        background-color: #22223a;
        border: 1px solid #3a3a5a;
        border-radius: 8px;
        padding: 0px 14px;
        color: #ffffff;
        font-size: 15px;
        min-height: 48px;
    }

    QLineEdit:focus {
        border: 2px solid #0077ff;
    }

    QLineEdit:disabled {
        background-color: #1a1a2e;
        color: #606070;
    }

    /* ===== SpinBox (touch: min-height 48px) ===== */
    QSpinBox, QDoubleSpinBox {
        background-color: #22223a;
        border: 1px solid #3a3a5a;
        border-radius: 8px;
        padding: 0px 10px;
        color: #ffffff;
        font-size: 15px;
        min-height: 48px;
    }

    QSpinBox:focus, QDoubleSpinBox:focus {
        border: 2px solid #0077ff;
    }

    QSpinBox::up-button, QDoubleSpinBox::up-button,
    QSpinBox::down-button, QDoubleSpinBox::down-button {
        width: 36px;
        border-radius: 6px;
        background-color: #2a2a4a;
    }

    QSpinBox::up-button:pressed, QDoubleSpinBox::up-button:pressed,
    QSpinBox::down-button:pressed, QDoubleSpinBox::down-button:pressed {
        background-color: #0055cc;
    }

    /* ===== ComboBox (touch: min-height 48px) ===== */
    QComboBox {
        background-color: #22223a;
        border: 1px solid #3a3a5a;
        border-radius: 8px;
        padding: 0px 14px;
        color: #ffffff;
        font-size: 15px;
        min-height: 48px;
    }

    QComboBox:focus {
        border: 2px solid #0077ff;
    }

    QComboBox::drop-down {
        width: 40px;
        border-left: 1px solid #3a3a5a;
        border-top-right-radius: 8px;
        border-bottom-right-radius: 8px;
    }

    QComboBox QAbstractItemView {
        background-color: #22223a;
        border: 1px solid #3a3a5a;
        selection-background-color: #0077ff;
        font-size: 15px;
    }

    QComboBox QAbstractItemView::item {
        min-height: 48px;
        padding: 0px 14px;
    }

    /* ===== Checkboxes (touch: 26px indicator, 48px row) ===== */
    QCheckBox {
        spacing: 14px;
        color: #c0c0d0;
        font-size: 15px;
        padding: 10px 4px;
        min-height: 48px;
    }

    QCheckBox::indicator {
        width: 26px;
        height: 26px;
        border: 2px solid #4a4a6a;
        border-radius: 6px;
        background-color: #22223a;
    }

    QCheckBox::indicator:checked {
        background-color: #0077ff;
        border-color: #0077ff;
    }

    /* ===== Buttons (touch: min-height 52px) ===== */
    QPushButton {
        background-color: #2a2a4a;
        border: 1px solid #3a3a5a;
        border-radius: 10px;
        padding: 0px 24px;
        color: #e0e0e0;
        font-size: 15px;
        font-weight: bold;
        min-height: 52px;
    }

    QPushButton:pressed {
        background-color: #1a1a3a;
        border-color: #0077ff;
    }

    QPushButton:disabled {
        background-color: #1e1e30;
        border-color: #2a2a3a;
        color: #505060;
    }

    QPushButton#btn_primary {
        background-color: #0077ff;
        border-color: #0066dd;
        color: #ffffff;
    }

    QPushButton#btn_primary:pressed {
        background-color: #0055cc;
    }

    QPushButton#btn_primary:disabled {
        background-color: #1a3a6a;
        border-color: #1a3a6a;
        color: #607090;
    }

    QPushButton#btn_danger {
        background-color: #cc3333;
        border-color: #aa2222;
        color: #ffffff;
    }

    QPushButton#btn_danger:pressed {
        background-color: #aa2222;
    }

    /* ===== Slider (touch: 14px groove, 36px handle) ===== */
    QSlider::groove:horizontal {
        background-color: #2a2a4a;
        height: 14px;
        border-radius: 7px;
    }

    QSlider::sub-page:horizontal {
        background-color: #0077ff;
        border-radius: 7px;
    }

    QSlider::handle:horizontal {
        background-color: #ffffff;
        border: 2px solid #0077ff;
        width: 36px;
        height: 36px;
        margin: -11px 0;
        border-radius: 18px;
    }

    QSlider::handle:horizontal:pressed {
        background-color: #0077ff;
    }

    /* ===== Tables (touch: 52px rows) ===== */
    QTableWidget {
        background-color: #1a1a2e;
        border: 1px solid #2a2a4a;
        gridline-color: #2a2a4a;
        color: #e0e0e0;
        font-size: 15px;
    }

    QTableWidget::item {
        padding: 14px 10px;
        min-height: 52px;
    }

    QTableWidget::item:selected {
        background-color: #0077ff;
    }

    QHeaderView::section {
        background-color: #22223a;
        color: #a0a0b0;
        padding: 0px 10px;
        min-height: 44px;
        border: 1px solid #2a2a4a;
        font-weight: bold;
        font-size: 14px;
    }

    /* ===== Scrollbars (touch: 16px wide, large handle) ===== */
    QScrollBar:vertical {
        background-color: #12122a;
        width: 16px;
        border: none;
        margin: 0px;
    }

    QScrollBar::handle:vertical {
        background-color: #3a3a5a;
        border-radius: 8px;
        min-height: 48px;
        margin: 2px;
    }

    QScrollBar::handle:vertical:pressed {
        background-color: #5a5a7a;
    }

    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
        height: 0px;
    }

    QScrollBar:horizontal {
        background-color: #12122a;
        height: 16px;
        border: none;
    }

    QScrollBar::handle:horizontal {
        background-color: #3a3a5a;
        border-radius: 8px;
        min-width: 48px;
        margin: 2px;
    }

    QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
        width: 0px;
    }

    /* ===== GroupBox ===== */
    QGroupBox {
        border: 1px solid #2a2a4a;
        border-radius: 8px;
        margin-top: 16px;
        padding-top: 20px;
        font-size: 15px;
        font-weight: bold;
        color: #ffffff;
    }

    QGroupBox::title {
        subcontrol-origin: margin;
        left: 14px;
        padding: 0 8px;
    }

    /* ===== Tab Component ===== */
    QTabWidget::pane {
        border: 1px solid #2a2a4a;
        border-radius: 6px;
        background-color: #1a1a2e;
    }

    QTabBar::tab {
        background-color: #22223a;
        border: 1px solid #2a2a4a;
        border-bottom: none;
        padding: 0px 24px;
        min-height: 52px;
        color: #a0a0b0;
        font-size: 15px;
        border-top-left-radius: 6px;
        border-top-right-radius: 6px;
        margin-right: 2px;
    }

    QTabBar::tab:selected {
        background-color: #1a1a2e;
        color: #ffffff;
        font-weight: bold;
        border-bottom: 3px solid #0077ff;
    }

    /* ===== Dialogs ===== */
    QDialog {
        background-color: #1a1a2e;
    }

    QMessageBox {
        background-color: #1a1a2e;
        font-size: 15px;
    }

    QMessageBox QPushButton {
        min-width: 100px;
    }

    /* ===== Bottom Tab Bar ===== */
    QFrame#tab_bar_frame {
        background-color: #12122a;
        border-top: 1px solid #2a2a4a;
    }

    QPushButton#nav_tab_btn {
        background: transparent;
        border: none;
        border-top: 3px solid transparent;
        border-radius: 0px;
        padding: 6px 4px 2px 4px;
        min-height: 72px;
        min-width: 0px;
        color: #606080;
        font-size: 12px;
        font-weight: normal;
    }

    QPushButton#nav_tab_btn:checked {
        color: #0077ff;
        border-top-color: #0077ff;
        font-weight: bold;
    }

    QPushButton#nav_tab_btn:pressed {
        background-color: rgba(0,119,255,25);
    }

    /* ===== Header icon buttons (notif, hamburger) ===== */
    QPushButton#btn_header_icon {
        background: transparent;
        border: none;
        border-radius: 8px;
        min-width: 52px;
        min-height: 52px;
        font-size: 22px;
        color: #c0c0d0;
        padding: 0;
    }

    QPushButton#btn_header_icon:pressed {
        background-color: rgba(255,255,255,18);
    }

    /* ===== Interface role buttons (4-way segmented) ===== */
    QPushButton#role_btn {
        background-color: #22223a;
        border: 1px solid #3a3a5a;
        border-radius: 6px;
        padding: 0;
        min-height: 52px;
        font-size: 14px;
        font-weight: normal;
        color: #a0a0c0;
    }

    QPushButton#role_btn:checked {
        background-color: #0077ff;
        border-color: #0055cc;
        color: #ffffff;
        font-weight: bold;
    }

    QPushButton#role_btn:pressed {
        background-color: #0055cc;
    }

    /* ===== Device ± spinner buttons ===== */
    QPushButton#spin_btn {
        background-color: #2a2a4a;
        border: 1px solid #3a3a5a;
        border-radius: 8px;
        min-width: 48px;
        min-height: 48px;
        max-width: 48px;
        max-height: 48px;
        font-size: 20px;
        font-weight: bold;
        padding: 0;
        color: #c0c0e0;
    }

    QPushButton#spin_btn:pressed {
        background-color: #0055cc;
        border-color: #0077ff;
    }

    QPushButton#spin_btn:disabled {
        color: #404050;
        border-color: #252535;
    }

    )QSS";

} // namespace Scalpel::GUI
