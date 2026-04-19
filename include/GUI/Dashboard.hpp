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
#include <QSocketNotifier>
#include <QSlider>
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
#include <string>
#include <thread>
#include <vector>
#include "Telemetry.hpp"
#include "SelfTest.hpp"

namespace HPGTP::GUI {

// ═══════════════════════════════════════════
// Notification panel: pull-down overlay (iOS-style)
// Slides in from top via spring physics, driven by anim_timer
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

class NotificationPanel : public QFrame {
    Q_OBJECT
public:
    explicit NotificationPanel(QWidget* parent = nullptr);
    void push_notification(const QString& title, const QString& body);
    void clear_all();
    void set_expanded(bool expanded);
    bool is_expanded() const { return expanded_; }
    bool is_settled() const;
    void advance_spring();
    void kick(double initial_vel);
    void set_backdrop_alpha(int alpha_0_255);
    int  backdrop_alpha() const { return backdrop_alpha_; }
    int  unread_count()   const { return unread_count_; }

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    bool    expanded_       = false;
    double  pos_y_          = 0.0;
    double  vel_y_          = 0.0;
    int     backdrop_alpha_ = 210;
    int     swipe_start_y_  = 0;
    int     unread_count_   = 0;
    QVBoxLayout* notif_list_;
};

// ═══════════════════════════════════════════
// Real-time plot: shift-buffer samples; RK4 spring-damper smoothing (buffer on sample, redraw on timer).
// ═══════════════════════════════════════════
class RealTimePlot : public QWidget {
    Q_OBJECT
public:
    explicit RealTimePlot(QWidget* parent = nullptr);
    void add_sample(double val);
    void set_fixed_max(double max) { fixed_max_ = max; current_max = max; }
protected:
    void paintEvent(QPaintEvent* event) override;
private:
    static constexpr int SHIFT_BUFFER_SIZE = 100;
    std::array<double, SHIFT_BUFFER_SIZE> shift_buffer{};
    size_t shift_head = 0;   // circular write index
    double current_max = 1.0;
    double fixed_max_ = 0.0;
};

// ═══════════════════════════════════════════
// Overview + System page (merged)
// ═══════════════════════════════════════════
class OverviewPage : public QWidget {
    Q_OBJECT
public:
    explicit OverviewPage(QWidget* parent = nullptr);
    void refresh(const Telemetry& tel, const std::array<uint64_t, 4>& last_pkts, const std::array<uint64_t, 4>& last_bytes);
    void refresh_info();
private:
    // Overview section
    RealTimePlot* pps_plot;
    RealTimePlot* bps_plot;
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
// Interface config page: per-port role assignment
// ═══════════════════════════════════════════
class InterfacePage : public QWidget {
    Q_OBJECT
public:
    explicit InterfacePage(QWidget* parent = nullptr);
    void scan_interfaces();
    void refresh_from_backend();
private slots:
    void on_save_clicked();
    void on_reset_clicked();
    void on_refresh_clicked();
    void on_scan_done();
private:
    void on_role_changed(const QString& iface, int index);

    QVBoxLayout* iface_cards_layout_ = nullptr;
    struct RoleEntry {
        std::array<char, 16> name{};
        QButtonGroup* group = nullptr;
    };
    std::array<RoleEntry, Telemetry::SystemInfo::MAX_IFACES> role_entries_{};
    size_t role_entries_count_ = 0;

    QPushButton*     btn_refresh = nullptr;
    QSocketNotifier* scan_done_notifier_ = nullptr;
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
// Game port whitelist editor dialog
// ═══════════════════════════════════════════
class PortWhitelistDialog : public QDialog {
    Q_OBJECT
public:
    explicit PortWhitelistDialog(QWidget* parent = nullptr);
    QTableWidget* table() { return table_; }
private slots:
    void on_add_port();
    void on_remove_port();
    void on_cell_edit(int row, int col);
private:
    QTableWidget* table_;
};

// ═══════════════════════════════════════════
// QoS traffic control page
// ═══════════════════════════════════════════
class QosPage : public QWidget {
    Q_OBJECT
public:
    explicit QosPage(QWidget* parent = nullptr);
    void refresh_whitelist_from_config();
    void refresh_from_backend();
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private slots:
    void on_edit_whitelist();
    void on_toggle_accel();
    void on_throttle_label_only(int value_pct);
    void on_throttle_committed();
    void on_apply_global_bw();
private:
    SwitchToggle* sw_acceleration;
    QLineEdit*    edit_dl_limit;
    QLineEdit*    edit_ul_limit;
    QTableWidget* whitelist_table;
    QSlider*      throttle_slider;
    QLabel*       lbl_throttle;
};

// ═══════════════════════════════════════════
// DHCP pool configuration dialog
// ═══════════════════════════════════════════
class DhcpConfigDialog : public QDialog {
    Q_OBJECT
public:
    explicit DhcpConfigDialog(QWidget* parent = nullptr);
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
};

// ═══════════════════════════════════════════
// DNS upstream / static records configuration dialog
// ═══════════════════════════════════════════
class DnsConfigDialog : public QDialog {
    Q_OBJECT
public:
    explicit DnsConfigDialog(QWidget* parent = nullptr);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private slots:
    void on_apply();
    void on_add_record();
    void on_remove_record();
private:
    QLineEdit*    edit_dns_primary;
    QLineEdit*    edit_dns_secondary;
    SwitchToggle*  sw_dns_redirect;
    QTableWidget* static_dns_table;
};

// ═══════════════════════════════════════════
// Service control page: NAT / DHCP / DNS / Firewall / UPnP
// ═══════════════════════════════════════════
class ServicePage : public QWidget {
    Q_OBJECT
public:
    explicit ServicePage(QWidget* parent = nullptr);
    void refresh_status();
private:
    struct ServiceRow {
        SwitchToggle* sw;
        QLabel*       status_label;
        QPushButton*  btn_settings = nullptr;  // non-null only for DHCP and DNS rows
    };
    ServiceRow rows[5]; // NAT, DHCP, DNS, Firewall, UPnP
};

// ═══════════════════════════════════════════
// Device management page: per-device access control + rate limiting
// ═══════════════════════════════════════════
class DevicePage : public QWidget {
    Q_OBJECT
public:
    explicit DevicePage(QWidget* parent = nullptr);
    void refresh();
private:
    void on_apply_all();

    struct DeviceRow {
        Net::IPv4Net ip{};
        SwitchToggle* sw_allow;
        SwitchToggle* sw_rate;
        QPushButton* lbl_dl;
        QPushButton* lbl_ul;
        double     val_dl = 100.0;
        double     val_ul = 10.0;
    };

    QVBoxLayout*         cards_layout;
    std::vector<DeviceRow> rows_;
    uint8_t last_device_count = 255;
    uint64_t last_device_policy_revision_ = 0;
};

// ═══════════════════════════════════════════
// Main control panel: navigation + stack + status bar
// ═══════════════════════════════════════════
class Dashboard : public QMainWindow {
    Q_OBJECT
public:
    explicit Dashboard(QWidget* parent = nullptr);
    ~Dashboard();
    // Thread-safe: callable from any thread (engine cores, network threads)
    static void post_notification(const QString& title, const QString& body);
    // Called on Qt main thread after async self-test completes
    static void on_selftest_done(const HPGTP::SelfTest::Report& r);
protected:
    void timerEvent(QTimerEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
private:
    void setup_ui();
    void setup_tabbar(QBoxLayout* root_layout);
    // Refresh matrix: startup uses load_config; after that, each page entry pulls
    // from process state (Config / Telemetry). User edits on the active page stay
    // until the user leaves and re-enters the page.
    void run_page_enter_refresh(int page_index);

    QStackedWidget* page_stack       = nullptr;
    QFrame*         header_          = nullptr;  // header frame — watched for swipe-down gesture
    int             hdr_swipe_y0_    = -1;       // -1 = not tracking
    int             notif_pull_y0_  = -1;       // global Y at press for 15% pull zone + interrupt
    int             notif_pull_x0_  = -1;       // global X at same press (horizontal intent guard)

    // Feature pages
    OverviewPage*    page_overview   = nullptr;
    InterfacePage*   page_interfaces = nullptr;
    QosPage*         page_qos        = nullptr;
    ServicePage*     page_services   = nullptr;
    DevicePage*      page_devices    = nullptr;

    // Notification panel
    NotificationPanel* notif_panel_ = nullptr;

    // Header info (speed + cpu temp) + unread badge
    QLabel* hdr_info_  = nullptr;
    QLabel* hdr_badge_ = nullptr;

    static Dashboard* instance_;

    // Bottom tab bar — 5 tabs
    std::array<QAbstractButton*, 5> tab_btns_{};

    // 60Hz UI animation timer (spring, badge, selftest overlay)
    int ui_timer_id_   = -1;
    // 25Hz data refresh timer (plots, header rates, page refreshes)
    int plot_timer_id_ = -1;

    // Self-test overlay (visible until on_selftest_done is called)
    QWidget*      testing_overlay_  = nullptr;
    QProgressBar* testing_progress_ = nullptr;
    int           selftest_tick_    = 0;
    static constexpr int SELFTEST_TICKS = 300; // 5s × 60Hz
    std::array<uint64_t, 4> last_pkts  = {};
    std::array<uint64_t, 4> last_bytes = {};
    uint64_t ui_tick_   = 0;
    uint64_t plot_tick_ = 0;
    std::chrono::steady_clock::time_point plot_last_tick_ = std::chrono::steady_clock::now();

    // Startup log reader: async file I/O off the GUI thread.
    // Auto-joins on Dashboard destruction; worker posts each line via invokeMethod.
    std::jthread startup_log_reader_;

    static void run_startup_log_reader(std::stop_token st);

private slots:
    void on_tab_clicked(int page_index);
    void on_shutdown_clicked();
};

} // namespace HPGTP::GUI
