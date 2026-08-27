#pragma once
#include <QMainWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QListWidget>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QAbstractButton>
#include <QStatusBar>
#include <QFrame>
#include <QLineEdit>
#include <QCheckBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QGroupBox>
#include <QScrollArea>
#include <QHeaderView>
#include <QComboBox>
#include <QProgressBar>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QValidator>
#include <QRegularExpressionValidator>
#include <QDialog>
#include <QEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QButtonGroup>
#include <QTime>
#include <QMenu>
#include <array>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include "Telemetry.hpp"
#include "SelfTest.hpp"

namespace HPGTP::GUI {

// ═══════════════════════════════════════════
// Pill-style on/off switch (track #0077ff on / #2a2a4a off, border #3a3a5a)
class SwitchToggle : public QWidget {
    Q_OBJECT
public:
    explicit SwitchToggle(QWidget* parent = nullptr);
    bool isChecked() const { return checked_; }
public slots:
    void setChecked(bool on);
signals:
    void toggled(bool checked);
protected:
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
private:
    bool checked_ = false;
};

// Real-time plot: shift-buffer samples, redrawn on the plot timer.
// ═══════════════════════════════════════════
class RealTimePlot : public QWidget {
    Q_OBJECT
public:
    explicit RealTimePlot(QWidget* parent = nullptr);
    void add_sample(double val);
    void set_fixed_max(double max) { fixed_max_ = max; current_max = max; }
    void setLineColor(const QColor& color);
protected:
    void paintEvent(QPaintEvent* event) override;
private:
    static constexpr int SHIFT_BUFFER_SIZE = 1000;
    std::array<double, SHIFT_BUFFER_SIZE> shift_buffer{};
    size_t shift_head = 0;   // circular write index
    double current_max = 1.0;
    double fixed_max_ = 0.0;
    QColor line_color_{200, 240, 255};
};

// ═══════════════════════════════════════════
// Overview + System page (merged)
// ═══════════════════════════════════════════
class OverviewPage : public QWidget {
    Q_OBJECT
public:
    explicit OverviewPage(QWidget* parent = nullptr);
    void refresh(const Telemetry& tel, double high_mbps, double normal_mbps);
    void refresh_info();
private:
    // Overview section
    RealTimePlot* high_plot;
    RealTimePlot* normal_plot;
    QLabel* core_labels[4];
    QLabel* lbl_mode;
    // System info section
    QLabel* lbl_hostname;
    QLabel* lbl_kernel;
    QLabel* lbl_cpu_temp;
    QLabel* lbl_uptime;
    QLabel* lbl_memory;

};


// ═══════════════════════════════════════════
// Numeric virtual keypad dialog
// ═══════════════════════════════════════════
class NumPadDialog : public QDialog {
    Q_OBJECT
public:
    static std::optional<int> get_int(QWidget* parent, const QString& title,
                                       int initial, int min, int max);
    static std::optional<double> get_double(QWidget* parent, const QString& title,
                                            double initial, double min, double max);
private:
    // Whitelist (get_int): allow_negative, no decimal. Bandwidth (get_double): decimal, no minus.
    explicit NumPadDialog(const QString& title, QString initial_text,
                          double min_val, double max_val,
                          bool allow_negative, bool allow_decimal,
                          QWidget* parent = nullptr);
    void push_digit(char d);
    void on_minus();
    void on_dot();
    void do_backspace();
    void update_display();

    QLabel*       display_;
    QPushButton*  btn_ok_;
    QPushButton*  btn_minus_ = nullptr;
    QPushButton*  btn_dot_   = nullptr;
    QString       text_;
    double        min_;
    double        max_;
    bool          allow_negative_;
    bool          allow_decimal_;
};


// ═══════════════════════════════════════════
// QoS traffic control page
// ═══════════════════════════════════════════
class QosPage : public QWidget {
    Q_OBJECT
public:
    explicit QosPage(QWidget* parent = nullptr);
    void refresh_from_backend();
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private slots:
    void on_toggle_accel();
    void on_apply_global_bw();
private:
    SwitchToggle* sw_acceleration;
    QLineEdit*    edit_dl_limit;
    QLineEdit*    edit_ul_limit;
};

// ═══════════════════════════════════════════
// DHCP pool configuration dialog
// ═══════════════════════════════════════════
class DhcpConfigDialog : public QDialog {
    Q_OBJECT
public:
    explicit DhcpConfigDialog(std::function<void()> apply_callback = {},
                              QWidget* parent = nullptr);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private slots:
    void on_apply();
private:
    QLineEdit* edit_pool_start;
    QLineEdit* edit_pool_end;
    QSpinBox*  spin_days;
    QSpinBox*  spin_hours;
    QSpinBox*  spin_minutes;
    std::function<void()> apply_callback_;
};


// ═══════════════════════════════════════════
// Service control page: DHCP
// ═══════════════════════════════════════════
class ServicePage : public QWidget {
    Q_OBJECT
public:
    explicit ServicePage(std::function<void()> dhcp_apply_callback = {},
                         QWidget* parent = nullptr);
    void refresh_status();
private:
    struct ServiceRow {
        SwitchToggle* sw;
        QLabel*       status_label;
        QPushButton*  btn_settings = nullptr;  // non-null only for DHCP and DNS rows
    };
    ServiceRow rows[1]; // DHCP
    std::function<void()> dhcp_apply_callback_;
};

// ═══════════════════════════════════════════
// Device list page: read-only ARP table display.
// ============================================================================
class DevicePage : public QWidget {
    Q_OBJECT
public:
    explicit DevicePage(QWidget* parent = nullptr);
    void refresh();
private:
    QVBoxLayout* cards_layout;
    uint8_t last_device_count = 255;
    uint64_t last_device_table_revision_ = 0;
};
// Main control panel: navigation + stack + status bar
// ═══════════════════════════════════════════
class Dashboard : public QMainWindow {
    Q_OBJECT
public:
    using ShutdownCallback = std::function<void()>;
    using DhcpApplyCallback = std::function<void()>;
    explicit Dashboard(ShutdownCallback shutdown_callback = {},
                       DhcpApplyCallback dhcp_apply_callback = {},
                       QWidget* parent = nullptr);
    ~Dashboard();
    // Thread-safe: callable from any thread (engine cores, network threads)
protected:
    void timerEvent(QTimerEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
private:
    void setup_ui();
    void setup_tabbar(QBoxLayout* root_layout);
    // Refresh matrix: startup uses load_config; after that, each page entry pulls from process state
    void run_page_enter_refresh(int page_index);

    QStackedWidget* page_stack       = nullptr;
    QFrame*         header_          = nullptr;

    // Feature pages
    OverviewPage*    page_overview   = nullptr;
    QosPage*         page_qos        = nullptr;
    ServicePage*     page_services   = nullptr;
    DevicePage*      page_devices    = nullptr;


    // Header info 
    QLabel* hdr_info_  = nullptr;


    // Bottom tab bar 
    std::array<QAbstractButton*, 4> tab_btns_{};

    // 60Hz data refresh timer 
    int plot_timer_id_ = -1;

    std::array<uint64_t, 4> last_bytes = {};
    uint64_t last_high_bytes = 0;
    uint64_t last_normal_bytes = 0;
    uint64_t plot_tick_ = 0;
    std::chrono::steady_clock::time_point plot_last_tick_ = std::chrono::steady_clock::now();

    ShutdownCallback shutdown_callback_;
    DhcpApplyCallback dhcp_apply_callback_;


private slots:
    void on_tab_clicked(int page_index);
    void on_shutdown_clicked();
};

} // namespace HPGTP::GUI
