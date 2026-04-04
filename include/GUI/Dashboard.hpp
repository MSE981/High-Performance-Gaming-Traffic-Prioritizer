#pragma once
#include <QMainWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QListWidget>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
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
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QValidator>
#include <QRegularExpressionValidator>
#include <QDialog>
#include <QButtonGroup>
#include <QMenu>
#include <array>
#include <string>
#include <vector>
#include "Telemetry.hpp"
#include "ProbeManager.hpp"

namespace Scalpel::GUI {

// ═══════════════════════════════════════════
// Notification panel: pull-down overlay (iOS-style)
// Slides in from top via spring physics, driven by anim_timer
// ═══════════════════════════════════════════
class NotificationPanel : public QFrame {
    Q_OBJECT
public:
    explicit NotificationPanel(QWidget* parent = nullptr);
    void push_notification(const QString& title, const QString& body);
    void set_expanded(bool expanded);
    bool is_expanded() const { return expanded_; }
    bool is_settled() const;
    void advance_spring();          // called every anim frame (16ms)
    void kick(double initial_vel);  // inject velocity for swipe-triggered motion
    // Backdrop opacity: 0 = transparent, 255 = fully opaque dark
    void set_backdrop_alpha(int alpha);
    int  backdrop_alpha() const { return backdrop_alpha_; }

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    bool    expanded_       = false;
    double  pos_y_          = 0.0;   // current real Y position (float)
    double  vel_y_          = 0.0;   // spring velocity
    int     backdrop_alpha_ = 210;   // default translucency
    int     swipe_start_y_  = 0;     // press Y for swipe gesture detection
    QVBoxLayout* notif_list_;
};

// ═══════════════════════════════════════════
// Real-time plot component (inherited from Phase 3, retains RK4 physics engine)
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
// Overview page: real-time charts + core metrics
// ═══════════════════════════════════════════
class OverviewPage : public QWidget {
    Q_OBJECT
public:
    explicit OverviewPage(QWidget* parent = nullptr);
    void refresh(const Telemetry& tel, const std::array<uint64_t, 4>& last_pkts, const std::array<uint64_t, 4>& last_bytes);
private:
    RealTimePlot* pps_plot;
    RealTimePlot* bps_plot;
    QLabel* core_labels[4];
    QLabel* lbl_cpu_capacity;
    QLabel* lbl_mode;
};

// ═══════════════════════════════════════════
// Interface config page: per-port role assignment
// ═══════════════════════════════════════════
class InterfacePage : public QWidget {
    Q_OBJECT
public:
    explicit InterfacePage(QWidget* parent = nullptr);
    void scan_interfaces();
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

    QCheckBox*       chk_stp;
    QCheckBox*       chk_igmp;
    QPushButton*     btn_refresh = nullptr;
    QSocketNotifier* scan_done_notifier_ = nullptr;
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
private slots:
    void on_edit_whitelist();
    void on_toggle_accel();
    void on_throttle_changed(int value);
private:
    QCheckBox*    chk_acceleration;
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
private slots:
    void on_apply();
    void on_add_record();
    void on_remove_record();
private:
    QLineEdit*    edit_dns_primary;
    QLineEdit*    edit_dns_secondary;
    QCheckBox*    chk_dns_redirect;
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
        QCheckBox*   chk;
        QLabel*      status_label;
        QPushButton* btn_settings = nullptr;  // non-null only for DHCP and DNS rows
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
        uint32_t ip;
        std::array<uint8_t, 6> mac{};
        QCheckBox* chk_allow;
        QCheckBox* chk_rate;
        QLabel*    lbl_dl;
        QLabel*    lbl_ul;
        double     val_dl = 100.0;
        double     val_ul = 10.0;
    };

    QVBoxLayout*         cards_layout;
    std::vector<DeviceRow> rows_;
    uint8_t last_device_count = 255;
};

// ═══════════════════════════════════════════
// System management page
// ═══════════════════════════════════════════
class SystemPage : public QWidget {
    Q_OBJECT
public:
    explicit SystemPage(QWidget* parent = nullptr);
    void refresh_info();
private slots:
    void on_save_config();
    void on_restart_engine();
    void on_speedtest_clicked();
    // Called on Qt thread when speedtest completes (via QueuedConnection)
    void on_speedtest_done(double dl_mbps, double ul_mbps);
private:
    QLabel* lbl_hostname;
    QLabel* lbl_kernel;
    QLabel* lbl_cpu_temp;
    QLabel* lbl_uptime;
    QLabel* lbl_memory;
    QLineEdit* edit_config_path;
    QPushButton* btn_speedtest;
    QLabel*      lbl_speedtest_status;
};

// ═══════════════════════════════════════════
// Main control panel: navigation + stack + status bar
// ═══════════════════════════════════════════
class Dashboard : public QMainWindow {
    Q_OBJECT
public:
    explicit Dashboard(QWidget* parent = nullptr);
protected:
    void timerEvent(QTimerEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
private:
    void setup_ui();
    void setup_tabbar(QBoxLayout* root_layout);

    QStackedWidget* page_stack;
    QFrame*         header_ = nullptr;  // header frame — watched for swipe-down gesture
    int             hdr_swipe_y0_ = -1; // -1 = not tracking

    // Feature pages
    OverviewPage*    page_overview;
    InterfacePage*   page_interfaces;
    QosPage*         page_qos;
    ServicePage*     page_services;
    DevicePage*      page_devices;
    SystemPage*      page_system;

    // Notification panel
    NotificationPanel* notif_panel_;

    // Header info (speed + cpu temp)
    QLabel* hdr_info_ = nullptr;

    // Bottom tab bar — 5 main tabs (index maps to TAB_PAGE_MAP)
    std::array<QPushButton*, 5> tab_btns_{};
    QPushButton* btn_more_ = nullptr;

    // 60Hz unified render + data refresh timer
    int data_timer_id_ = -1;
    std::array<uint64_t, 4> last_pkts  = {};
    std::array<uint64_t, 4> last_bytes = {};
    uint64_t data_tick_ = 0;

private slots:
    void on_tab_clicked(int page_index);
    void on_more_clicked();
    void on_shutdown_clicked();
    void on_notif_toggle_clicked();
};

} // namespace Scalpel::GUI
