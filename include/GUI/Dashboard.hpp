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
#include <deque>
#include <array>
#include "Telemetry.hpp"

namespace Scalpel::GUI {

    // ═══════════════════════════════════════════
    // Real-time plot component (inherited from Phase 3, retains RK4 physics engine)
    // ═══════════════════════════════════════════
    class RealTimePlot : public QWidget {
        Q_OBJECT
    public:
        explicit RealTimePlot(QWidget* parent = nullptr);
        void add_sample(double val);
        void step_physics();
    protected:
        void paintEvent(QPaintEvent* event) override;
    private:
        static constexpr int SHIFT_BUFFER_SIZE = 100;
        std::deque<double> shift_buffer;
        double current_max = 1.0;
        double target_max = 1.0;
        double current_velocity = 0.0;
    };

    // ═══════════════════════════════════════════
    // Overview page: real-time charts + core metrics
    // ═══════════════════════════════════════════
    class OverviewPage : public QWidget {
        Q_OBJECT
    public:
        explicit OverviewPage(QWidget* parent = nullptr);
        void refresh(const Telemetry& tel, uint64_t last_pkts[4], uint64_t last_bytes[4]);
    private:
        RealTimePlot* pps_plot;
        RealTimePlot* bps_plot;
        QLabel* core_labels[4];
        QLabel* lbl_cpu_capacity;
        QLabel* lbl_mode;
    };

    // ═══════════════════════════════════════════
    // Interface config page: WAN/LAN form editing
    // ═══════════════════════════════════════════
    class InterfacePage : public QWidget {
        Q_OBJECT
    public:
        explicit InterfacePage(QWidget* parent = nullptr);
        void scan_interfaces();
    private slots:
        void on_save_clicked();
        void on_reset_clicked();
    private:
        QTabWidget* tab_widget;
        // WAN form
        QLineEdit* wan_iface_edit;
        // LAN form
        QLineEdit* lan_iface_edit;
        QCheckBox* chk_bridge;
        QCheckBox* chk_stp;
        QCheckBox* chk_igmp;
        QVBoxLayout* iface_list_layout;
        QList<QCheckBox*> iface_checkboxes;
    };

    // ═══════════════════════════════════════════
    // QoS traffic control page
    // ═══════════════════════════════════════════
    class QosPage : public QWidget {
        Q_OBJECT
    public:
        explicit QosPage(QWidget* parent = nullptr);
    private slots:
        void on_add_rule();
        void on_remove_rule();
        void on_toggle_accel();
    private:
        QCheckBox* chk_acceleration;
        QLineEdit* edit_dl_limit;
        QLineEdit* edit_ul_limit;
        QTableWidget* rules_table;
        QPushButton* btn_add_rule;
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
            QCheckBox* chk;
            QLabel* status_label;
        };
        ServiceRow rows[5]; // NAT, DHCP, DNS, Firewall, UPnP
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
    private:
        QLabel* lbl_hostname;
        QLabel* lbl_kernel;
        QLabel* lbl_cpu_temp;
        QLabel* lbl_uptime;
        QLabel* lbl_memory;
        QLineEdit* edit_config_path;
    };

    // ═══════════════════════════════════════════
    // Placeholder pages: Docker / VPN, etc.
    // ═══════════════════════════════════════════
    class PlaceholderPage : public QWidget {
        Q_OBJECT
    public:
        explicit PlaceholderPage(const QString& name, QWidget* parent = nullptr);
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
    private:
        void setup_ui();
        void setup_nav();
        void setup_statusbar();

        // Navigation and pages
        QListWidget*    nav_list;
        QStackedWidget* page_stack;

        // Feature pages
        OverviewPage*    page_overview;
        InterfacePage*   page_interfaces;
        QosPage*         page_qos;
        ServicePage*     page_services;
        PlaceholderPage* page_docker;
        PlaceholderPage* page_vpn;
        SystemPage*      page_system;

        // Status bar labels
        QLabel* status_cpu;
        QLabel* status_ram;
        QLabel* status_uptime;
        QLabel* status_dl;
        QLabel* status_ul;

        // 40ms heartbeat
        int refresh_timer_id;
        uint64_t last_pkts[4] = {};
        uint64_t last_bytes[4] = {};
        uint64_t tick_counter = 0;

    private slots:
        void on_nav_changed(int index);
        void on_shutdown_clicked();
    };

} // namespace Scalpel::GUI
