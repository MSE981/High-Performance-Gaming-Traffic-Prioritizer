#include "GUI/Dashboard.hpp"
#include "GUI/StyleSheet.hpp"
#include "Config.hpp"
#include "SystemOptimizer.hpp"
#include <QApplication>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QTimerEvent>
#include <QResizeEvent>
#include <QFormLayout>
#include <QScrollArea>
#include <QRegularExpressionValidator>
#include <QDoubleSpinBox>
#include <QMessageBox>
#include <thread>
#include <print>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <arpa/inet.h>
#include <QSignalBlocker>

namespace Scalpel::GUI {

// ═════════════════════════════════════════════════════════════
// NotificationPanel: iOS-style pull-down overlay
// Spring constants tuned for a snappy-but-not-bouncy feel
// ═════════════════════════════════════════════════════════════
NotificationPanel::NotificationPanel(QWidget* parent) : QFrame(parent) {
    setFixedHeight(260);
    setStyleSheet(
        "NotificationPanel {"
        "  background: rgba(20,20,32,220);"
        "  border-bottom-left-radius: 20px;"
        "  border-bottom-right-radius: 20px;"
        "}"
    );

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(8);

    auto* handle = new QFrame();
    handle->setFixedSize(40, 4);
    handle->setStyleSheet("background: rgba(255,255,255,60); border-radius: 2px;");
    auto* handle_row = new QHBoxLayout();
    handle_row->addStretch();
    handle_row->addWidget(handle);
    handle_row->addStretch();
    root->addLayout(handle_row);

    auto* header_lbl = new QLabel("Notification Centre");
    header_lbl->setStyleSheet("color: rgba(255,255,255,180); font-size: 13px; font-weight: bold;");
    root->addWidget(header_lbl);

    notif_list_ = new QVBoxLayout();
    notif_list_->setSpacing(6);
    root->addLayout(notif_list_);
    root->addStretch();

    // Start hidden above screen
    move(0, -height());
}

void NotificationPanel::push_notification(const QString& title, const QString& body) {
    auto* card = new QFrame();
    card->setStyleSheet(
        "QFrame {"
        "  background: rgba(255,255,255,12);"
        "  border-radius: 12px;"
        "  padding: 8px;"
        "}"
    );
    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->setSpacing(2);
    auto* t = new QLabel(title);
    t->setStyleSheet("color: #ffffff; font-size: 13px; font-weight: bold;");
    auto* b = new QLabel(body);
    b->setStyleSheet("color: rgba(255,255,255,160); font-size: 12px;");
    b->setWordWrap(true);
    lay->addWidget(t);
    lay->addWidget(b);
    notif_list_->addWidget(card);
}

void NotificationPanel::set_expanded(bool expanded) {
    expanded_ = expanded;
}

void NotificationPanel::advance_spring() {
    constexpr double k    = 0.18;   // spring stiffness
    constexpr double damp = 0.12;   // damping ratio
    double target = expanded_ ? 0.0 : -(double)height();
    double force  = k * (target - pos_y_);
    vel_y_ = (vel_y_ + force) * (1.0 - damp);
    pos_y_ += vel_y_;
    move(0, (int)pos_y_);
}

bool NotificationPanel::is_settled() const {
    return std::abs(vel_y_) < 0.5 && std::abs(pos_y_ - (expanded_ ? 0.0 : -(double)height())) < 1.0;
}

// ═════════════════════════════════════════════════════════════
// RealTimePlot (retain Phase 3 physics engine)
// ═════════════════════════════════════════════════════════════
RealTimePlot::RealTimePlot(QWidget* parent) : QWidget(parent) {
    setMinimumSize(200, 120);
    shift_buffer.fill(0.0);
}

void RealTimePlot::add_sample(double val) {
    shift_buffer[shift_head] = val;
    shift_head = (shift_head + 1) % SHIFT_BUFFER_SIZE;
}

void RealTimePlot::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QLinearGradient bg(0, 0, 0, height());
    bg.setColorAt(0, QColor(30, 30, 50));
    bg.setColorAt(1, QColor(20, 20, 35));
    painter.fillRect(rect(), bg);
    if (shift_buffer.empty()) return;

    QPainterPath path;
    double x_step = (double)width() / (SHIFT_BUFFER_SIZE - 1);
    double h = height();
    for (int i = 0; i < SHIFT_BUFFER_SIZE; ++i) {
        // Read oldest-to-newest: shift_head points to the next write slot (= oldest unread sample)
        double sample = shift_buffer[(shift_head + i) % SHIFT_BUFFER_SIZE];
        double y = h - (sample / (current_max + 1.0) * h * 0.8) - 10;
        if (i == 0) path.moveTo(0, y); else path.lineTo(i * x_step, y);
    }
    painter.setPen(QPen(QColor(0, 150, 255, 40), 8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);
    painter.setPen(QPen(QColor(200, 240, 255), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);
}

// ═════════════════════════════════════════════════════════════
// OverviewPage: overview page
// ═════════════════════════════════════════════════════════════
OverviewPage::OverviewPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);

    auto* title = new QLabel("System Overview");
    title->setObjectName("section_title");
    layout->addWidget(title);

    // Mode and performance labels
    auto* info_row = new QHBoxLayout();
    lbl_mode = new QLabel("Mode: Acceleration");
    lbl_cpu_capacity = new QLabel("CPU Capacity: --");
    lbl_mode->setStyleSheet("color: #00cc66; font-weight: bold; font-size: 15px;");
    lbl_cpu_capacity->setStyleSheet("color: #ffaa00; font-weight: bold; font-size: 15px;");
    info_row->addWidget(lbl_mode);
    info_row->addStretch();
    info_row->addWidget(lbl_cpu_capacity);
    layout->addLayout(info_row);

    // Dual chart row
    auto* plot_row = new QHBoxLayout();
    auto* pps_group = new QGroupBox("Packet rate (PPS)");
    auto* pps_lay = new QVBoxLayout(pps_group);
    pps_plot = new RealTimePlot();
    pps_lay->addWidget(pps_plot);
    plot_row->addWidget(pps_group);

    auto* bps_group = new QGroupBox("Bandwidth (Bytes/s)");
    auto* bps_lay = new QVBoxLayout(bps_group);
    bps_plot = new RealTimePlot();
    bps_lay->addWidget(bps_plot);
    plot_row->addWidget(bps_group);
    layout->addLayout(plot_row);

    // 4-core status row
    auto* cores_row = new QHBoxLayout();
    for (int i = 0; i < 4; ++i) {
        core_labels[i] = new QLabel(QString("Core %1: idle").arg(i));
        core_labels[i]->setStyleSheet("background-color: #22223a; border: 1px solid #2a2a4a; border-radius: 6px; padding: 10px; font-size: 13px;");
        core_labels[i]->setAlignment(Qt::AlignCenter);
        cores_row->addWidget(core_labels[i]);
    }
    layout->addLayout(cores_row);
    layout->addStretch();
}

void OverviewPage::refresh(const Telemetry& tel, const std::array<uint64_t, 4>& last_p, const std::array<uint64_t, 4>& last_b) {
    double total_pps = 0, total_bps = 0;
    for (int i = 0; i < 4; ++i) {
        uint64_t cp = tel.core_metrics[i].pkts.load(std::memory_order_relaxed);
        uint64_t cb = tel.core_metrics[i].bytes.load(std::memory_order_relaxed);
        uint64_t dp = cp - last_p[i], db = cb - last_b[i];
        total_pps += dp * 60.0;   // 60Hz tick: scale 16ms delta to per-second rate
        total_bps += db * 60.0;
        core_labels[i]->setText(QString("Core %1\n%2 Pkts\n%3 KB")
            .arg(i).arg(cp).arg(cb / 1024));
    }

    // Lock Y-axis ceiling to configured ISP bandwidth limits (Mbps → Bytes/s and PPS)
    double dl = tel.isp_down_limit_mbps.load(std::memory_order_relaxed);
    double ul = tel.isp_up_limit_mbps.load(std::memory_order_relaxed);
    if (dl > 0.0 || ul > 0.0) {
        double total_bytes_s = (dl + ul) * 1e6 / 8.0;
        bps_plot->set_fixed_max(total_bytes_s);
        pps_plot->set_fixed_max(total_bytes_s / 64.0); // estimate: minimum 64-byte packets
    }

    pps_plot->add_sample(total_pps);
    bps_plot->add_sample(total_bps);
    pps_plot->update();
    bps_plot->update();

    lbl_mode->setText(tel.bridge_mode.load(std::memory_order_relaxed) ? "Mode: Bridge" : "Mode: Acceleration");
    double cap = tel.internal_limit_mbps.load(std::memory_order_relaxed);
    if (cap > 0) lbl_cpu_capacity->setText(QString("CPU Capacity: %1 Mbps").arg(cap, 0, 'f', 1));
}

// ═════════════════════════════════════════════════════════════
// InterfacePage: per-port role assignment
// ═════════════════════════════════════════════════════════════
InterfacePage::InterfacePage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);

    auto* title = new QLabel("Network Interface Roles");
    title->setObjectName("section_title");
    layout->addWidget(title);

    auto* desc = new QLabel("Assign a role to each detected interface. Exactly one must be set as Default Gateway.");
    desc->setStyleSheet("color: #707080; font-size: 12px;");
    desc->setWordWrap(true);
    layout->addWidget(desc);

    // Role table: Interface | Role | Link state
    iface_table = new QTableWidget(0, 3);
    iface_table->setHorizontalHeaderLabels({"Interface", "Role", "Link State"});
    iface_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    iface_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    iface_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    iface_table->verticalHeader()->setVisible(false);
    iface_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    iface_table->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(iface_table);

    scan_interfaces();

    // Advanced options
    auto* adv_group = new QGroupBox("Advanced Options");
    auto* adv_form = new QFormLayout(adv_group);
    chk_stp = new QCheckBox("Enable STP");
    chk_stp->setChecked(Config::ENABLE_STP.load(std::memory_order_relaxed));
    adv_form->addRow("Spanning Tree Protocol:", chk_stp);
    chk_igmp = new QCheckBox("Enable IGMP Snooping");
    chk_igmp->setChecked(Config::ENABLE_IGMP_SNOOPING.load(std::memory_order_relaxed));
    adv_form->addRow("IGMP Snooping:", chk_igmp);
    layout->addWidget(adv_group);

    // Button row
    auto* btn_row = new QHBoxLayout();
    btn_refresh = new QPushButton("Refresh");
    connect(btn_refresh, &QPushButton::clicked, this, &InterfacePage::on_refresh_clicked);
    btn_row->addWidget(btn_refresh);
    btn_row->addStretch();
    auto* btn_reset = new QPushButton("Reset");
    connect(btn_reset, &QPushButton::clicked, this, &InterfacePage::on_reset_clicked);
    btn_row->addWidget(btn_reset);
    auto* btn_save = new QPushButton("Save & Apply");
    btn_save->setObjectName("btn_primary");
    connect(btn_save, &QPushButton::clicked, this, &InterfacePage::on_save_clicked);
    btn_row->addWidget(btn_save);
    layout->addLayout(btn_row);

    // Register QSocketNotifier for done_fd — wakes Qt event loop when watchdog completes rescan
    int done_fd = Telemetry::instance().sys_info.done_fd;
    if (done_fd >= 0) {
        scan_done_notifier_ = new QSocketNotifier(done_fd, QSocketNotifier::Read, this);
        connect(scan_done_notifier_, &QSocketNotifier::activated, this, &InterfacePage::on_scan_done);
    }
}

void InterfacePage::scan_interfaces() {
    iface_table->setRowCount(0);
    role_combos_count = 0;

    // Read interface list from Core 1 watchdog pre-cache — zero file I/O on Core 0 (§7.7)
    auto& si = Telemetry::instance().sys_info;
    uint8_t cnt = si.iface_count.load(std::memory_order_acquire);

    using IfacePair = std::pair<std::string, std::string>; // {name, operstate}
    std::array<IfacePair, Telemetry::SystemInfo::MAX_IFACES> buf;
    uint8_t valid = 0;
    if (cnt > 0) {
        for (uint8_t i = 0; i < cnt; ++i)
            buf[valid++] = { si.ifaces[i].name.data(), si.ifaces[i].operstate.data() };
    } else {
        // Watchdog not yet populated (startup race): static fallback
        buf[0] = {"eth0", "unknown"}; buf[1] = {"eth1", "unknown"};
        buf[2] = {"eth2", "unknown"}; buf[3] = {"eth3", "unknown"};
        valid = 4;
    }
    std::sort(buf.begin(), buf.begin() + valid,
        [](const IfacePair& a, const IfacePair& b) { return a.first < b.first; });

    for (uint8_t i = 0; i < valid; ++i) {
        const auto& [name, state] = buf[i];

        Config::IfaceRole role = Config::find_role(name);
        if (role == Config::IfaceRole::DISABLED) {
            if (name == "eth0") role = Config::IfaceRole::GATEWAY;
            else if (name == "eth1") role = Config::IfaceRole::LAN;
        }

        int row = iface_table->rowCount();
        iface_table->insertRow(row);

        auto* name_item = new QTableWidgetItem(QString::fromStdString(name));
        name_item->setTextAlignment(Qt::AlignCenter);
        iface_table->setItem(row, 0, name_item);

        auto* combo = new QComboBox();
        combo->addItems({"WAN (Uplink)", "LAN (Local)", "Default Gateway", "Disabled"});
        combo->setCurrentIndex(static_cast<int>(role));
        if (role_combos_count < role_combos.size()) {
            strncpy(role_combos[role_combos_count].name.data(), name.c_str(), 15);
            role_combos[role_combos_count].name[15] = '\0';
            role_combos[role_combos_count].combo = combo;
            ++role_combos_count;
        }
        iface_table->setCellWidget(row, 1, combo);

        bool is_up = (state == "up");
        auto* state_lbl = new QLabel(is_up ? "● Connected" : "○ Down");
        state_lbl->setAlignment(Qt::AlignCenter);
        state_lbl->setStyleSheet(is_up ? "color: #00cc66;" : "color: #888888;");
        iface_table->setCellWidget(row, 2, state_lbl);

        connect(combo, &QComboBox::currentIndexChanged, this, [this, name](int index) {
            on_role_changed(QString::fromStdString(name), index);
        });
    }
}

void InterfacePage::on_role_changed(const QString& iface, int index) {
    if (index != 2) return; // Only enforce when a combo is set to GATEWAY
    // Demote any other gateway combo to WAN to maintain single-gateway invariant
    std::string iface_str = iface.toStdString();
    for (size_t i = 0; i < role_combos_count; ++i) {
        if (iface_str != role_combos[i].name.data() && role_combos[i].combo->currentIndex() == 2) {
            QSignalBlocker blocker(role_combos[i].combo);
            role_combos[i].combo->setCurrentIndex(0); // demote to WAN
        }
    }
}

void InterfacePage::on_save_clicked() {
    // Validate: exactly one gateway must be assigned
    int gw_count = 0;
    std::string gw_iface;
    for (size_t i = 0; i < role_combos_count; ++i) {
        if (role_combos[i].combo->currentIndex() == 2) { ++gw_count; gw_iface = role_combos[i].name.data(); }
    }
    if (gw_count == 0) {
        QMessageBox::warning(this, "Config Error", "No Default Gateway assigned.");
        return;
    }

    // Persist role table
    Config::clear_roles();
    for (size_t i = 0; i < role_combos_count; ++i)
        Config::set_role(role_combos[i].name.data(), static_cast<Config::IfaceRole>(role_combos[i].combo->currentIndex()));

    // Derive legacy variables consumed by App
    Config::IFACE_GATEWAY = gw_iface;
    Config::IFACE_WAN = gw_iface;
    Config::clear_bridged();
    for (size_t i = 0; i < role_combos_count; ++i)
        if (role_combos[i].combo->currentIndex() == 1) Config::add_bridged(role_combos[i].name.data());
    Config::IFACE_LAN = Config::BRIDGED_IFACES_COUNT == 0 ? "" : std::string(Config::BRIDGED_INTERFACES[0].name.data());

    Config::ENABLE_STP.store(chk_stp->isChecked(), std::memory_order_relaxed);
    Config::ENABLE_IGMP_SNOOPING.store(chk_igmp->isChecked(), std::memory_order_relaxed);
    Config::ENABLE_ACCELERATION.store(true, std::memory_order_relaxed);
    Telemetry::instance().bridge_mode.store(true, std::memory_order_relaxed);

    std::println("[GUI] Interface roles saved. Gateway: {}, LAN interfaces: {}",
        Config::IFACE_GATEWAY, Config::BRIDGED_IFACES_COUNT);
}

void InterfacePage::on_reset_clicked() {
    scan_interfaces(); // Rebuilds table from Config::IFACE_ROLES
    chk_stp->setChecked(Config::ENABLE_STP.load(std::memory_order_relaxed));
    chk_igmp->setChecked(Config::ENABLE_IGMP_SNOOPING.load(std::memory_order_relaxed));
}

void InterfacePage::on_refresh_clicked() {
    int rescan_fd = Telemetry::instance().sys_info.rescan_fd;
    if (rescan_fd < 0) return;
    ::eventfd_write(rescan_fd, 1);   // wake Core 1 watchdog immediately
    btn_refresh->setEnabled(false);
    btn_refresh->setText("Scanning…");
}

void InterfacePage::on_scan_done() {
    // Drain the eventfd counter before reading Telemetry cache
    uint64_t val;
    ::eventfd_read(Telemetry::instance().sys_info.done_fd, &val);
    scan_interfaces();
    btn_refresh->setEnabled(true);
    btn_refresh->setText("Refresh");
}

// ═════════════════════════════════════════════════════════════
// QosPage: QoS traffic control page
// ═════════════════════════════════════════════════════════════
QosPage::QosPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel("QoS & Traffic Control");
    title->setObjectName("section_title");
    layout->addWidget(title);

    // Acceleration toggle
    chk_acceleration = new QCheckBox("Enable Gaming Traffic Acceleration (Heuristic Priority Scheduling)");
    chk_acceleration->setChecked(Config::ENABLE_ACCELERATION.load(std::memory_order_relaxed));
    connect(chk_acceleration, &QCheckBox::toggled, this, &QosPage::on_toggle_accel);
    layout->addWidget(chk_acceleration);

    // Bandwidth limit
    auto* bw_group = new QGroupBox("Global Bandwidth Limits");
    auto* bw_form = new QFormLayout(bw_group);
    edit_dl_limit = new QLineEdit("500");
    edit_ul_limit = new QLineEdit("50");
    bw_form->addRow("Download Limit (Mbps):", edit_dl_limit);
    bw_form->addRow("Upload Limit (Mbps):", edit_ul_limit);
    layout->addWidget(bw_group);

    // Real-time throttle slider
    auto* throttle_group = new QGroupBox("Real-Time Throttle");
    auto* throttle_lay = new QVBoxLayout(throttle_group);

    auto* slider_row = new QHBoxLayout();
    slider_row->addWidget(new QLabel("0%"));
    slider_row->addStretch();
    lbl_throttle = new QLabel("100%");
    slider_row->addWidget(lbl_throttle);

    throttle_slider = new QSlider(Qt::Horizontal);
    throttle_slider->setRange(0, 100);
    throttle_slider->setValue(Telemetry::instance().qos_throttle_pct.load(std::memory_order_relaxed));
    throttle_slider->setTickInterval(10);
    throttle_slider->setTickPosition(QSlider::TicksBelow);
    connect(throttle_slider, &QSlider::valueChanged, this, &QosPage::on_throttle_changed);

    throttle_lay->addWidget(throttle_slider);
    throttle_lay->addLayout(slider_row);
    layout->addWidget(throttle_group);

    // Game port whitelist
    auto* wl_group = new QGroupBox("Game Port Whitelist");
    auto* wl_lay = new QVBoxLayout(wl_group);
    auto* wl_desc = new QLabel("Traffic on these ports receives highest scheduling priority. Supports single ports and ranges (e.g. 27015 or 3478-3480).");
    wl_desc->setStyleSheet("color: #808090; font-size: 12px;");
    wl_desc->setWordWrap(true);
    wl_lay->addWidget(wl_desc);

    whitelist_table = new QTableWidget(0, 3);
    whitelist_table->setHorizontalHeaderLabels({"Port / Range", "Protocol", "Description"});
    whitelist_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    whitelist_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    whitelist_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    whitelist_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    whitelist_table->verticalHeader()->setVisible(false);
    wl_lay->addWidget(whitelist_table);

    struct PortEntry { const char* port; const char* proto; const char* desc; };
    static constexpr PortEntry defaults[] = {
        {"27015",      "UDP",     "Steam / Valve"},
        {"3074",       "UDP",     "Call of Duty"},
        {"3659",       "UDP",     "EA / Origin"},
        {"3478-3480",  "UDP",     "PlayStation Network"},
        {"5223",       "TCP",     "PlayStation Network"},
        {"7777",       "UDP",     "Unreal Engine games"},
        {"25565",      "TCP",     "Minecraft"},
        {"6112",       "TCP/UDP", "Blizzard / StarCraft"},
        {"1119",       "TCP/UDP", "Blizzard Battle.net"},
        {"8080",       "TCP",     "Game HTTP services"},
    };
    for (auto& e : defaults) {
        int row = whitelist_table->rowCount();
        whitelist_table->insertRow(row);
        whitelist_table->setItem(row, 0, new QTableWidgetItem(e.port));
        whitelist_table->setItem(row, 1, new QTableWidgetItem(e.proto));
        whitelist_table->setItem(row, 2, new QTableWidgetItem(e.desc));
    }

    auto* wl_btn_row = new QHBoxLayout();
    auto* btn_add_port    = new QPushButton("+ Add Port");
    auto* btn_remove_port = new QPushButton("- Remove");
    btn_add_port->setObjectName("btn_primary");
    wl_btn_row->addWidget(btn_add_port);
    wl_btn_row->addWidget(btn_remove_port);
    wl_btn_row->addStretch();
    wl_lay->addLayout(wl_btn_row);
    layout->addWidget(wl_group);
    layout->addStretch();

    connect(btn_add_port,    &QPushButton::clicked, this, &QosPage::on_add_port);
    connect(btn_remove_port, &QPushButton::clicked, this, &QosPage::on_remove_port);
}

void QosPage::on_toggle_accel() {
    bool on = chk_acceleration->isChecked();
    Config::ENABLE_ACCELERATION.store(on, std::memory_order_relaxed);
    Telemetry::instance().bridge_mode.store(!on, std::memory_order_relaxed);
    std::println("[GUI] Acceleration mode: {}", on ? "ON" : "OFF");
}

void QosPage::on_add_port() {
    int row = whitelist_table->rowCount();
    whitelist_table->insertRow(row);
    whitelist_table->setItem(row, 0, new QTableWidgetItem(""));
    whitelist_table->setItem(row, 1, new QTableWidgetItem("UDP"));
    whitelist_table->setItem(row, 2, new QTableWidgetItem(""));
    whitelist_table->editItem(whitelist_table->item(row, 0));
}

void QosPage::on_throttle_changed(int value) {
    Telemetry::instance().qos_throttle_pct.store(value, std::memory_order_relaxed);
    lbl_throttle->setText(QString("%1%").arg(value));
}

void QosPage::on_remove_port() {
    int row = whitelist_table->currentRow();
    if (row >= 0) whitelist_table->removeRow(row);
}

// ═════════════════════════════════════════════════════════════
// DhcpConfigDialog
// ═════════════════════════════════════════════════════════════
DhcpConfigDialog::DhcpConfigDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("DHCP Pool Configuration");
    setMinimumWidth(360);
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);

    auto* ip_validator = new QRegularExpressionValidator(
        QRegularExpression(
            R"(^((25[0-5]|2[0-4]\d|1\d{2}|[1-9]\d|\d)\.){3}(25[0-5]|2[0-4]\d|1\d{2}|[1-9]\d|\d)$)"),
        this);

    edit_pool_start = new QLineEdit(QString::fromStdString(Config::DHCP_POOL_START));
    edit_pool_start->setValidator(ip_validator);
    edit_pool_start->setPlaceholderText("e.g. 192.168.1.50");
    form->addRow("Pool Start:", edit_pool_start);

    edit_pool_end = new QLineEdit(QString::fromStdString(Config::DHCP_POOL_END));
    edit_pool_end->setValidator(ip_validator);
    edit_pool_end->setPlaceholderText("e.g. 192.168.1.249");
    form->addRow("Pool End:", edit_pool_end);

    auto* lease_row = new QHBoxLayout();
    spin_days = new QSpinBox(); spin_days->setRange(0, 365); spin_days->setSuffix(" d");
    spin_days->setValue(static_cast<int>(Config::DHCP_LEASE_SECONDS / 86400));
    spin_hours = new QSpinBox(); spin_hours->setRange(0, 24); spin_hours->setSuffix(" h");
    spin_hours->setValue(static_cast<int>((Config::DHCP_LEASE_SECONDS % 86400) / 3600));
    spin_minutes = new QSpinBox(); spin_minutes->setRange(0, 60); spin_minutes->setSuffix(" min");
    spin_minutes->setValue(static_cast<int>((Config::DHCP_LEASE_SECONDS % 3600) / 60));
    lease_row->addWidget(spin_days);
    lease_row->addWidget(spin_hours);
    lease_row->addWidget(spin_minutes);
    form->addRow("Lease Duration:", lease_row);
    layout->addLayout(form);

    auto* btn_row = new QHBoxLayout();
    auto* btn_apply = new QPushButton("Apply");
    btn_apply->setObjectName("btn_primary");
    btn_row->addStretch();
    btn_row->addWidget(btn_apply);
    layout->addLayout(btn_row);

    connect(btn_apply, &QPushButton::clicked, this, &DhcpConfigDialog::on_apply);
}

void DhcpConfigDialog::on_apply() {
    if (!edit_pool_start->hasAcceptableInput() || !edit_pool_end->hasAcceptableInput()) {
        edit_pool_start->setStyleSheet("border: 1px solid #cc3333;");
        edit_pool_end->setStyleSheet("border: 1px solid #cc3333;");
        return;
    }
    edit_pool_start->setStyleSheet("");
    edit_pool_end->setStyleSheet("");

    uint32_t start_ip = Config::parse_ip_str(edit_pool_start->text().toStdString());
    uint32_t end_ip   = Config::parse_ip_str(edit_pool_end->text().toStdString());
    if (ntohl(start_ip) >= ntohl(end_ip)) {
        edit_pool_start->setStyleSheet("border: 1px solid #cc3333;");
        edit_pool_end->setStyleSheet("border: 1px solid #cc3333;");
        return;
    }

    uint32_t secs = static_cast<uint32_t>(spin_days->value())    * 86400u
                  + static_cast<uint32_t>(spin_hours->value())   * 3600u
                  + static_cast<uint32_t>(spin_minutes->value()) * 60u;
    if (secs == 0) secs = 60;

    Config::DHCP_POOL_START    = edit_pool_start->text().toStdString();
    Config::DHCP_POOL_END      = edit_pool_end->text().toStdString();
    Config::DHCP_LEASE_SECONDS = secs;
    Telemetry::instance().dhcp_config_dirty.store(true, std::memory_order_release);
    std::println("[GUI] DHCP pool updated: {} – {}, lease {}s",
        Config::DHCP_POOL_START, Config::DHCP_POOL_END, secs);
    accept();
}

// ═════════════════════════════════════════════════════════════
// DnsConfigDialog
// ═════════════════════════════════════════════════════════════
DnsConfigDialog::DnsConfigDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("DNS Configuration");
    setMinimumWidth(420);
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);

    auto* dns_ip_validator1 = new QRegularExpressionValidator(
        QRegularExpression(
            R"(^((25[0-5]|2[0-4]\d|1\d{2}|[1-9]\d|\d)\.){3}(25[0-5]|2[0-4]\d|1\d{2}|[1-9]\d|\d)$)"),
        this);
    auto* dns_ip_validator2 = new QRegularExpressionValidator(
        QRegularExpression(
            R"(^((25[0-5]|2[0-4]\d|1\d{2}|[1-9]\d|\d)\.){3}(25[0-5]|2[0-4]\d|1\d{2}|[1-9]\d|\d)$)"),
        this);

    edit_dns_primary = new QLineEdit(QString::fromStdString(Config::DNS_UPSTREAM_PRIMARY));
    edit_dns_primary->setValidator(dns_ip_validator1);
    edit_dns_primary->setPlaceholderText("e.g. 8.8.8.8");
    form->addRow("Primary DNS:", edit_dns_primary);

    edit_dns_secondary = new QLineEdit(QString::fromStdString(Config::DNS_UPSTREAM_SECONDARY));
    edit_dns_secondary->setValidator(dns_ip_validator2);
    edit_dns_secondary->setPlaceholderText("e.g. 8.8.4.4");
    form->addRow("Secondary DNS:", edit_dns_secondary);

    chk_dns_redirect = new QCheckBox("Force all DNS queries to upstream server");
    chk_dns_redirect->setChecked(Config::DNS_REDIRECT_ENABLED.load(std::memory_order_relaxed));
    chk_dns_redirect->setToolTip("When enabled, all LAN DNS queries (UDP 53) are forwarded to the configured upstream server");
    form->addRow("", chk_dns_redirect);

    auto* static_dns_label = new QLabel("Static DNS Records (hostname → IP, never expires, overrides cache)");
    static_dns_label->setStyleSheet("color: #808090; font-size: 12px;");
    form->addRow(static_dns_label);

    static_dns_table = new QTableWidget(0, 2);
    static_dns_table->setHorizontalHeaderLabels({"Hostname", "IP Address"});
    static_dns_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    static_dns_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    static_dns_table->setFixedHeight(160);
    static_dns_table->setStyleSheet("QTableWidget { background: #1a1a2e; }");

    for (size_t i = 0; i < Config::STATIC_DNS_COUNT; ++i) {
        int row = static_dns_table->rowCount();
        static_dns_table->insertRow(row);
        static_dns_table->setItem(row, 0, new QTableWidgetItem(
            QString::fromLatin1(Config::STATIC_DNS_TABLE[i].hostname.data())));
        uint32_t ip = Config::STATIC_DNS_TABLE[i].ip;
        static_dns_table->setItem(row, 1, new QTableWidgetItem(
            QString("%1.%2.%3.%4")
                .arg(ip & 0xFF).arg((ip >> 8) & 0xFF)
                .arg((ip >> 16) & 0xFF).arg((ip >> 24) & 0xFF)));
    }
    form->addRow(static_dns_table);

    auto* dns_btn_row = new QHBoxLayout();
    auto* btn_add    = new QPushButton("+ Add");
    auto* btn_remove = new QPushButton("- Remove");
    auto* btn_apply  = new QPushButton("Apply");
    btn_apply->setObjectName("btn_primary");
    btn_apply->setFixedWidth(100);
    dns_btn_row->addWidget(btn_add);
    dns_btn_row->addWidget(btn_remove);
    dns_btn_row->addStretch();
    dns_btn_row->addWidget(btn_apply);
    form->addRow("", dns_btn_row);

    layout->addLayout(form);

    connect(btn_add,    &QPushButton::clicked, this, &DnsConfigDialog::on_add_record);
    connect(btn_remove, &QPushButton::clicked, this, &DnsConfigDialog::on_remove_record);
    connect(btn_apply,  &QPushButton::clicked, this, &DnsConfigDialog::on_apply);
}

void DnsConfigDialog::on_apply() {
    if (!edit_dns_primary->hasAcceptableInput()) {
        edit_dns_primary->setStyleSheet("border: 1px solid #cc3333;");
        return;
    }
    edit_dns_primary->setStyleSheet("");

    if (!edit_dns_secondary->text().isEmpty() && !edit_dns_secondary->hasAcceptableInput()) {
        edit_dns_secondary->setStyleSheet("border: 1px solid #cc3333;");
        return;
    }
    edit_dns_secondary->setStyleSheet("");

    Config::DNS_UPSTREAM_PRIMARY   = edit_dns_primary->text().toStdString();
    Config::DNS_UPSTREAM_SECONDARY = edit_dns_secondary->text().toStdString();
    Config::DNS_REDIRECT_ENABLED.store(chk_dns_redirect->isChecked(), std::memory_order_relaxed);

    Config::STATIC_DNS_COUNT = 0;
    for (int i = 0; i < static_dns_table->rowCount(); ++i) {
        auto* host_item = static_dns_table->item(i, 0);
        auto* ip_item   = static_dns_table->item(i, 1);
        if (!host_item || !ip_item) continue;
        std::string hostname = host_item->text().trimmed().toStdString();
        std::string ip_str   = ip_item->text().trimmed().toStdString();
        if (hostname.empty() || ip_str.empty()) continue;
        Config::upsert_static_dns(hostname, ip_str);
    }

    Telemetry::instance().dns_config_dirty.store(true, std::memory_order_release);
    std::println("[GUI] DNS config updated: upstream {}→{}, redirect={}, static={} records",
        Config::DNS_UPSTREAM_PRIMARY, Config::DNS_UPSTREAM_SECONDARY,
        chk_dns_redirect->isChecked(), Config::STATIC_DNS_COUNT);
    accept();
}

void DnsConfigDialog::on_add_record() {
    int row = static_dns_table->rowCount();
    if (row >= static_cast<int>(Config::MAX_STATIC_DNS)) return;
    static_dns_table->insertRow(row);
    static_dns_table->setItem(row, 0, new QTableWidgetItem(""));
    static_dns_table->setItem(row, 1, new QTableWidgetItem(""));
    static_dns_table->editItem(static_dns_table->item(row, 0));
}

void DnsConfigDialog::on_remove_record() {
    int row = static_dns_table->currentRow();
    if (row >= 0) static_dns_table->removeRow(row);
}

// ═════════════════════════════════════════════════════════════
// ServicePage: service toggle page
// ═════════════════════════════════════════════════════════════
ServicePage::ServicePage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel("Services");
    title->setObjectName("section_title");
    layout->addWidget(title);

    auto* desc = new QLabel("Enable or disable core network services. Changes take effect immediately, no restart required.");
    desc->setStyleSheet("color: #707080; font-size: 13px; margin-bottom: 8px;");
    layout->addWidget(desc);

    struct ServiceDef {
        const char* name;
        const char* description;
        std::atomic<bool>* state;
        const char* settings_label;  // nullptr = no settings button
    };

    ServiceDef defs[5] = {
        {"NAT (Network Address Translation)", "Zero-copy SNAT/DNAT engine",                  &Config::global_state.enable_nat,       nullptr},
        {"DHCP (Dynamic Host Config)",        "Automatically assigns IP addresses to LAN clients", &Config::global_state.enable_dhcp, "Set DHCP"},
        {"DNS Cache",                         "Local DNS cache for fast resolution",           &Config::global_state.enable_dns_cache, "Set DNS"},
        {"Firewall",                          "Inbound traffic filter rule engine",            &Config::global_state.enable_firewall,  nullptr},
        {"UPnP (Plug & Play)",                "Automatic port mapping for NAT traversal",      &Config::global_state.enable_upnp,      nullptr},
    };

    for (int i = 0; i < 5; ++i) {
        auto* row_frame = new QFrame();
        row_frame->setStyleSheet("QFrame { background-color: #22223a; border: 1px solid #2a2a4a; border-radius: 6px; padding: 8px; margin: 2px 0px; }");
        auto* row_lay = new QHBoxLayout(row_frame);

        rows[i].chk = new QCheckBox(defs[i].name);
        rows[i].chk->setChecked(defs[i].state->load(std::memory_order_relaxed));
        rows[i].chk->setStyleSheet("font-size: 15px; font-weight: bold;");

        auto* desc_lbl = new QLabel(defs[i].description);
        desc_lbl->setStyleSheet("color: #808090; font-size: 12px;");

        rows[i].status_label = new QLabel("● Running");
        rows[i].status_label->setStyleSheet("color: #00cc66; font-weight: bold;");

        auto* text_col = new QVBoxLayout();
        text_col->addWidget(rows[i].chk);
        text_col->addWidget(desc_lbl);
        row_lay->addLayout(text_col, 1);

        // Settings button (DHCP / DNS only)
        if (defs[i].settings_label) {
            auto* btn = new QPushButton(defs[i].settings_label);
            btn->setEnabled(defs[i].state->load(std::memory_order_relaxed));
            rows[i].btn_settings = btn;
            row_lay->addWidget(btn);

            if (i == 1) {
                connect(btn, &QPushButton::clicked, this, [this]() {
                    DhcpConfigDialog dlg(this);
                    dlg.exec();
                });
            } else if (i == 2) {
                connect(btn, &QPushButton::clicked, this, [this]() {
                    DnsConfigDialog dlg(this);
                    dlg.exec();
                });
            }
        }

        row_lay->addWidget(rows[i].status_label);

        // Connect toggle: update state + status label + settings button enabled
        auto* state_ptr  = defs[i].state;
        auto* status_lbl = rows[i].status_label;
        auto* btn_cfg    = rows[i].btn_settings;
        connect(rows[i].chk, &QCheckBox::toggled, [state_ptr, status_lbl, btn_cfg](bool checked) {
            state_ptr->store(checked, std::memory_order_relaxed);
            status_lbl->setText(checked ? "● Running" : "○ Stopped");
            status_lbl->setStyleSheet(checked ? "color: #00cc66; font-weight: bold;" : "color: #cc3333; font-weight: bold;");
            if (btn_cfg) btn_cfg->setEnabled(checked);
            std::println("[GUI] Service state updated");
        });

        layout->addWidget(row_frame);
    }

    layout->addStretch();
}

void ServicePage::refresh_status() {
    std::atomic<bool>* states[5] = {
        &Config::global_state.enable_nat,
        &Config::global_state.enable_dhcp,
        &Config::global_state.enable_dns_cache,
        &Config::global_state.enable_firewall,
        &Config::global_state.enable_upnp,
    };
    for (int i = 0; i < 5; ++i) {
        bool on = states[i]->load(std::memory_order_relaxed);
        rows[i].status_label->setText(on ? "● Running" : "○ Stopped");
        rows[i].status_label->setStyleSheet(on ? "color: #00cc66; font-weight: bold;" : "color: #cc3333; font-weight: bold;");
    }
}

// ═════════════════════════════════════════════════════════════
// SystemPage: system management page
// ═════════════════════════════════════════════════════════════
SystemPage::SystemPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel("System Info");
    title->setObjectName("section_title");
    layout->addWidget(title);

    auto* info_group = new QGroupBox("Hardware & Runtime");
    auto* info_form = new QFormLayout(info_group);
    info_form->setSpacing(10);
    lbl_hostname = new QLabel("--");
    lbl_kernel = new QLabel("--");
    lbl_kernel->setWordWrap(true);
    lbl_cpu_temp = new QLabel("--");
    lbl_uptime = new QLabel("--");
    lbl_memory = new QLabel("--");
    info_form->addRow("Hostname:", lbl_hostname);
    info_form->addRow("Kernel:", lbl_kernel);
    info_form->addRow("CPU Temp:", lbl_cpu_temp);
    info_form->addRow("Uptime:", lbl_uptime);
    info_form->addRow("Memory:", lbl_memory);
    layout->addWidget(info_group);

    auto* cfg_group = new QGroupBox("Configuration");
    auto* cfg_lay = new QVBoxLayout(cfg_group);
    auto* cfg_form = new QFormLayout();
    edit_config_path = new QLineEdit("config.txt");
    cfg_form->addRow("Config File:", edit_config_path);
    cfg_lay->addLayout(cfg_form);

    auto* btn_row = new QHBoxLayout();
    auto* btn_save = new QPushButton("Save Config");
    btn_save->setObjectName("btn_primary");
    connect(btn_save, &QPushButton::clicked, this, &SystemPage::on_save_config);
    btn_row->addWidget(btn_save);
    auto* btn_restart = new QPushButton("Restart Engine");
    btn_restart->setObjectName("btn_danger");
    connect(btn_restart, &QPushButton::clicked, this, &SystemPage::on_restart_engine);
    btn_row->addWidget(btn_restart);
    cfg_lay->addLayout(btn_row);
    layout->addWidget(cfg_group);

    // Network speedtest group
    auto* spd_group = new QGroupBox("Speed Test");
    auto* spd_lay   = new QVBoxLayout(spd_group);
    auto* spd_desc  = new QLabel("Runs speedtest-cli asynchronously to measure real ISP throughput.\nThe routing engine continues running during the test.");
    spd_desc->setStyleSheet("color: #808090; font-size: 12px;");
    spd_lay->addWidget(spd_desc);

    auto* spd_btn_row = new QHBoxLayout();
    btn_speedtest = new QPushButton("▶ Run Test");
    btn_speedtest->setObjectName("btn_primary");
    btn_speedtest->setFixedWidth(130);
    lbl_speedtest_status = new QLabel("Ready");
    lbl_speedtest_status->setStyleSheet("color: #808090; font-size: 12px;");
    spd_btn_row->addWidget(btn_speedtest);
    spd_btn_row->addWidget(lbl_speedtest_status);
    spd_btn_row->addStretch();
    spd_lay->addLayout(spd_btn_row);

    connect(btn_speedtest, &QPushButton::clicked, this, &SystemPage::on_speedtest_clicked);
    layout->addWidget(spd_group);
    layout->addStretch();
}

void SystemPage::refresh_info() {
    // All system info is pre-fetched by Core 1 watchdog_loop every 5 seconds into Telemetry::sys_info.
    // This UI callback only reads from atomics and pre-allocated char arrays — no file I/O on Core 0.
    auto& tel = Telemetry::instance();
    auto& si  = tel.sys_info;

    lbl_hostname->setText(si.hostname[0] ? QString(si.hostname.data()) : "--");
    lbl_kernel->setText(si.kernel_short[0] ? QString(si.kernel_short.data()) : "--");

    double t = tel.cpu_temp_celsius.load(std::memory_order_relaxed);
    if (t > 0) {
        lbl_cpu_temp->setText(QString("%1 °C").arg(t, 0, 'f', 1));
        lbl_cpu_temp->setStyleSheet(t > 70 ? "color: #ff4444; font-weight: bold;" : "color: #00cc66;");
    }

    uint64_t secs = si.uptime_seconds.load(std::memory_order_relaxed);
    if (secs > 0) {
        int h = static_cast<int>(secs / 3600);
        int m = static_cast<int>((secs % 3600) / 60);
        lbl_uptime->setText(QString("%1h %2m").arg(h).arg(m));
    }

    uint64_t total = si.mem_total_kb.load(std::memory_order_relaxed);
    uint64_t avail = si.mem_avail_kb.load(std::memory_order_relaxed);
    if (total > 0)
        lbl_memory->setText(QString("%1 MB used / %2 MB total")
            .arg((total - avail) / 1024).arg(total / 1024));
}

void SystemPage::on_save_config() {
    std::string path = edit_config_path->text().toStdString();
    // §3.1: async task with completion callback — notify UI thread on finish (QueuedConnection)
    std::thread([this, path](){
        Scalpel::System::Optimizer::set_current_thread_affinity(1); // Core 1: Control Plane
        Config::save_config(path);
        QMetaObject::invokeMethod(edit_config_path, [this, path](){
            edit_config_path->setPlaceholderText(QString("Saved: %1").arg(QString::fromStdString(path)));
        }, Qt::QueuedConnection);
    }).detach();
}

void SystemPage::on_restart_engine() {
    std::println("[GUI] Engine restart triggered (requires manual execution)");
}

void SystemPage::on_speedtest_clicked() {
    btn_speedtest->setEnabled(false);
    lbl_speedtest_status->setText("Testing, please wait…");
    lbl_speedtest_status->setStyleSheet("color: #f0a500; font-size: 12px;");

    // run_async_real_isp_probe internally detaches a thread; callback fires from that thread.
    // QueuedConnection marshals the result back to the Qt event loop (Core 0).
    Probe::Manager::run_async_real_isp_probe([this](double dl, double ul) {
        QMetaObject::invokeMethod(this, [this, dl, ul]() {
            on_speedtest_done(dl, ul);
        }, Qt::QueuedConnection);
    });
}

void SystemPage::on_speedtest_done(double dl_mbps, double ul_mbps) {
    btn_speedtest->setEnabled(true);
    lbl_speedtest_status->setText(
        QString("↓ %1 Mbps / ↑ %2 Mbps")
            .arg(dl_mbps, 0, 'f', 1)
            .arg(ul_mbps, 0, 'f', 1));
    lbl_speedtest_status->setStyleSheet("color: #00cc66; font-size: 12px;");

    auto* dlg = new QMessageBox(this);
    dlg->setWindowTitle("Speed Test Complete");
    dlg->setText(
        QString("Download: <b>%1 Mbps</b>&nbsp;&nbsp;&nbsp;Upload: <b>%2 Mbps</b>")
            .arg(dl_mbps, 0, 'f', 1)
            .arg(ul_mbps, 0, 'f', 1));
    dlg->setInformativeText("Write these results as the QoS baseline?\n(The throttle slider will be recalculated against the new baseline)");
    dlg->setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    dlg->setDefaultButton(QMessageBox::Yes);
    dlg->button(QMessageBox::Yes)->setText("Apply");
    dlg->button(QMessageBox::No)->setText("Ignore");

    if (dlg->exec() == QMessageBox::Yes) {
        auto& tel = Telemetry::instance();
        tel.isp_down_limit_mbps.store(dl_mbps, std::memory_order_relaxed);
        tel.isp_up_limit_mbps.store(ul_mbps,   std::memory_order_relaxed);
        tel.speedtest_result_ready.store(true,  std::memory_order_release);
        std::println("[SpeedTest] User accepted: DL {:.1f} Mbps / UL {:.1f} Mbps → QoS base updated",
            dl_mbps, ul_mbps);
    }
    dlg->deleteLater();
}

// ═════════════════════════════════════════════════════════════
// DevicePage: per-device access control and rate limiting
// ═════════════════════════════════════════════════════════════
DevicePage::DevicePage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel("Device Management");
    title->setObjectName("section_title");
    layout->addWidget(title);

    auto* desc = new QLabel("Lists all active devices on the subnet. Configure access and rate limits, then click Apply All.");
    desc->setStyleSheet("color: #707080; font-size: 13px; margin-bottom: 8px;");
    layout->addWidget(desc);

    // Scrollable card list
    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea { border: none; }");

    auto* cards_container = new QWidget();
    cards_layout = new QVBoxLayout(cards_container);
    cards_layout->setSpacing(8);
    cards_layout->setContentsMargins(4, 4, 4, 4);
    cards_layout->addStretch();

    scroll->setWidget(cards_container);
    layout->addWidget(scroll, 1);

    // Single apply button at the bottom
    auto* btn_apply_all = new QPushButton("Apply All");
    btn_apply_all->setObjectName("btn_primary");
    btn_apply_all->setFixedHeight(36);
    connect(btn_apply_all, &QPushButton::clicked, this, &DevicePage::on_apply_all);
    layout->addWidget(btn_apply_all);
}

void DevicePage::refresh() {
    auto& tel = Telemetry::instance();
    uint8_t cnt = tel.device_count.load(std::memory_order_acquire);
    if (cnt == last_device_count) return;
    last_device_count = cnt;

    // Remove all existing cards (leave trailing stretch)
    rows_.clear();
    while (cards_layout->count() > 1) {
        QLayoutItem* item = cards_layout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    for (uint8_t i = 0; i < cnt; ++i) {
        uint32_t ip         = tel.device_table[i].ip;
        const char* mac_str = tel.device_table[i].mac.data();

        // Look up existing policy
        bool blocked = false, rate_limited = false;
        double dl = 100.0, ul = 10.0;
        for (size_t j = 0; j < Config::DEVICE_POLICY_COUNT; ++j) {
            if (Config::DEVICE_POLICY_TABLE[j].ip == ip) {
                blocked      = Config::DEVICE_POLICY_TABLE[j].blocked;
                rate_limited = Config::DEVICE_POLICY_TABLE[j].rate_limited;
                dl           = Config::DEVICE_POLICY_TABLE[j].dl_mbps;
                ul           = Config::DEVICE_POLICY_TABLE[j].ul_mbps;
                break;
            }
        }

        // Card frame
        auto* card = new QFrame();
        card->setStyleSheet(
            "QFrame { background:#1e1e3a; border:1px solid #2a2a4a; border-radius:6px; }"
        );
        auto* cl = new QVBoxLayout(card);
        cl->setSpacing(6);
        cl->setContentsMargins(12, 8, 12, 8);

        // Header: IP + MAC
        struct in_addr addr{}; addr.s_addr = ip;
        auto* lbl_info = new QLabel(
            QString("<b>%1</b>  <span style='color:#707080;font-size:12px;'>%2</span>")
                .arg(inet_ntoa(addr)).arg(mac_str));
        lbl_info->setTextFormat(Qt::RichText);
        cl->addWidget(lbl_info);

        // Row 1: checkboxes
        auto* chk_row = new QHBoxLayout();
        auto* chk_allow = new QCheckBox("Allow Access");
        chk_allow->setChecked(!blocked);
        auto* chk_rate = new QCheckBox("Rate Limit");
        chk_rate->setChecked(rate_limited);
        chk_row->addWidget(chk_allow);
        chk_row->addSpacing(24);
        chk_row->addWidget(chk_rate);
        chk_row->addStretch();
        cl->addLayout(chk_row);

        // Row 2: speed spinboxes
        auto* spin_row = new QHBoxLayout();
        auto* spin_dl = new QDoubleSpinBox();
        spin_dl->setRange(0.1, 10000.0);
        spin_dl->setDecimals(1);
        spin_dl->setPrefix("↓ ");
        spin_dl->setSuffix(" Mbps");
        spin_dl->setValue(dl);
        auto* spin_ul = new QDoubleSpinBox();
        spin_ul->setRange(0.1, 10000.0);
        spin_ul->setDecimals(1);
        spin_ul->setPrefix("↑ ");
        spin_ul->setSuffix(" Mbps");
        spin_ul->setValue(ul);
        spin_row->addWidget(spin_dl);
        spin_row->addSpacing(12);
        spin_row->addWidget(spin_ul);
        spin_row->addStretch();
        cl->addLayout(spin_row);

        cards_layout->insertWidget(cards_layout->count() - 1, card);

        // Store widget references for on_apply_all
        DeviceRow r;
        r.ip        = ip;
        r.chk_allow = chk_allow;
        r.chk_rate  = chk_rate;
        r.spin_dl   = spin_dl;
        r.spin_ul   = spin_ul;
        unsigned int m[6]{};
        if (sscanf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                   &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]) == 6)
            for (int k = 0; k < 6; ++k) r.mac[k] = static_cast<uint8_t>(m[k]);
        rows_.push_back(r);
    }
}

void DevicePage::on_apply_all() {
    for (auto& r : rows_) {
        Config::upsert_device_policy(r.ip, r.mac.data(),
            !r.chk_allow->isChecked(),
            r.chk_rate->isChecked(),
            r.spin_dl->value(),
            r.spin_ul->value());
    }
    Config::DEVICE_POLICY_DIRTY.store(true, std::memory_order_release);
    std::println("[GUI] Device policies applied for {} device(s)", rows_.size());
}

// ═════════════════════════════════════════════════════════════
// PlaceholderPage: placeholder (VPN / future features)
// ═════════════════════════════════════════════════════════════
PlaceholderPage::PlaceholderPage(const QString& name, QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel(name);
    title->setObjectName("section_title");
    layout->addWidget(title);
    
    auto* msg = new QLabel(QString("'%1' is under construction.\n\nThis module will include:\n - Container lifecycle management\n - Fast image deployment\n - Virtualised network isolation").arg(name));
    msg->setStyleSheet("color: #707080; font-size: 15px;");
    msg->setAlignment(Qt::AlignCenter);
    layout->addStretch();
    layout->addWidget(msg);
    layout->addStretch();
}

// ═════════════════════════════════════════════════════════════
// Dashboard: main control panel frame
// ═════════════════════════════════════════════════════════════
Dashboard::Dashboard(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("High-Performance Gaming Traffic Prioritizer");
    setStyleSheet(DARK_STYLESHEET);
    setup_ui();
    // Unified 60Hz timer: drives spring animation and data refresh on every frame
    data_timer_id_ = startTimer(16, Qt::PreciseTimer);
}

void Dashboard::setup_ui() {
    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    auto* root_layout = new QVBoxLayout(central);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    // ── Header bar ──
    auto* header = new QFrame();
    header->setObjectName("header_frame");
    auto* header_lay = new QHBoxLayout(header);
    header_lay->setContentsMargins(16, 8, 16, 8);
    auto* title = new QLabel("High-Performance-Gaming-Traffic-Prioritizer");
    title->setObjectName("header_title");
    header_lay->addWidget(title);
    header_lay->addStretch();
    auto* status_dot = new QLabel("● Running");
    status_dot->setStyleSheet("color: #00cc66; font-weight: bold; font-size: 14px;");
    header_lay->addWidget(status_dot);

    auto* btn_notif = new QPushButton("🔔");
    btn_notif->setFixedSize(36, 36);
    btn_notif->setStyleSheet("QPushButton { background: transparent; font-size: 18px; border: none; }");
    connect(btn_notif, &QPushButton::clicked, this, &Dashboard::on_notif_toggle_clicked);
    header_lay->addWidget(btn_notif);

    auto* btn_shutdown = new QPushButton("Shutdown");
    btn_shutdown->setObjectName("btn_danger");
    connect(btn_shutdown, &QPushButton::clicked, this, &Dashboard::on_shutdown_clicked);
    header_lay->addWidget(btn_shutdown);

    root_layout->addWidget(header);

    // ── Center area: navigation + stack ──
    auto* body_layout = new QHBoxLayout();
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(0);

    setup_nav();
    body_layout->addWidget(nav_list);

    page_stack = new QStackedWidget();
    page_overview = new OverviewPage();
    page_interfaces = new InterfacePage();
    page_qos = new QosPage();
    page_services = new ServicePage();
    page_devices = new DevicePage();
    page_vpn = new PlaceholderPage("🔐 VPN / IPsec");
    page_system = new SystemPage();

    // Wrap each page in QScrollArea to prevent oversized content from
    // inflating QStackedWidget's minimumSize and distorting the layout.
    auto wrap = [](QWidget* page) -> QScrollArea* {
        auto* sa = new QScrollArea();
        sa->setWidget(page);
        sa->setWidgetResizable(true);
        sa->setFrameShape(QFrame::NoFrame);
        sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        return sa;
    };
    page_stack->addWidget(wrap(page_overview));
    page_stack->addWidget(wrap(page_interfaces));
    page_stack->addWidget(wrap(page_qos));
    page_stack->addWidget(wrap(page_services));
    page_stack->addWidget(wrap(page_devices));
    page_stack->addWidget(wrap(page_vpn));
    page_stack->addWidget(wrap(page_system));
    body_layout->addWidget(page_stack, 1);

    root_layout->addLayout(body_layout, 1);

    // ── Bottom status bar ──
    setup_statusbar();

    // ── Notification panel: floats above all content, initially hidden above screen ──
    notif_panel_ = new NotificationPanel(centralWidget());
    notif_panel_->setFixedWidth(centralWidget()->width());
    notif_panel_->raise();
    notif_panel_->push_notification("Router Ready", "Data plane Cores 2/3 attached. Forwarding engine running.");
}

void Dashboard::setup_nav() {
    nav_list = new QListWidget();
    nav_list->setObjectName("nav_list");
    nav_list->setFixedWidth(160);
    nav_list->setIconSize(QSize(0, 0));

    QStringList items = {"📊 Overview", "🌐 Interfaces", "⚡ QoS", "🔧 Services", "📡 Devices", "🔐 VPN / IPsec", "💻 System"};
    for (auto& label : items) nav_list->addItem(label);

    nav_list->setCurrentRow(0);
    connect(nav_list, &QListWidget::currentRowChanged, this, &Dashboard::on_nav_changed);
}

void Dashboard::setup_statusbar() {
    auto* bar = statusBar();
    status_cpu = new QLabel("CPU: --");
    status_ram = new QLabel("RAM: --");
    status_uptime = new QLabel("Uptime: --");
    status_dl = new QLabel("↓ 0.00 Mbps");
    status_ul = new QLabel("↑ 0.00 Mbps");

    bar->addWidget(status_cpu);
    bar->addWidget(status_ram);
    bar->addWidget(status_uptime);
    bar->addPermanentWidget(status_dl);
    bar->addPermanentWidget(status_ul);
}

void Dashboard::on_nav_changed(int index) {
    page_stack->setCurrentIndex(index);
    if (index == 4) page_devices->refresh();      // force immediate device list refresh
    if (index == 6) page_system->refresh_info();
}

void Dashboard::on_shutdown_clicked() {
    // QApplication::quit() unblocks qapp.exec(); shutdown sequence in main.cpp
    // calls app.stop() and waits for all jthreads to exit cleanly.
    QApplication::quit();
}

void Dashboard::on_notif_toggle_clicked() {
    notif_panel_->set_expanded(!notif_panel_->is_expanded());
}

void Dashboard::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (notif_panel_)
        notif_panel_->setFixedWidth(centralWidget()->width());
}

void Dashboard::timerEvent(QTimerEvent* event) {
    if (event->timerId() != data_timer_id_) return;

    // 60Hz unified tick: spring animation + data refresh
    data_tick_++;

    // Advance notification panel spring physics every frame (no-op when settled)
    if (!notif_panel_->is_settled()) {
        notif_panel_->advance_spring();
        notif_panel_->update();
    }

    auto& tel = Telemetry::instance();

    // Bandwidth: delta over 16ms tick, scaled to Mbps (×60 = per-second rate)
    uint64_t cur_b2 = tel.core_metrics[2].bytes.load(std::memory_order_relaxed);
    uint64_t cur_b3 = tel.core_metrics[3].bytes.load(std::memory_order_relaxed);
    double dl = (cur_b2 - last_bytes[2]) * 8.0 * 60.0 / 1e6;
    double ul = (cur_b3 - last_bytes[3]) * 8.0 * 60.0 / 1e6;
    status_dl->setText(QString("↓ %1 Mbps").arg(dl, 0, 'f', 2));
    status_ul->setText(QString("↑ %1 Mbps").arg(ul, 0, 'f', 2));

    // CPU temperature from watchdog atomic — no file I/O on UI thread
    double t = tel.cpu_temp_celsius.load(std::memory_order_relaxed);
    if (t > 0) status_cpu->setText(QString("CPU: %1°C").arg(t, 0, 'f', 0));

    // RAM usage — read from Core 1 watchdog pre-cache
    uint64_t mem_total = tel.sys_info.mem_total_kb.load(std::memory_order_relaxed);
    uint64_t mem_avail = tel.sys_info.mem_avail_kb.load(std::memory_order_relaxed);
    if (mem_total > 0)
        status_ram->setText(QString("RAM: %1/%2 MB")
            .arg((mem_total - mem_avail) / 1024).arg(mem_total / 1024));

    // Uptime — read from Core 1 watchdog pre-cache
    uint64_t secs = tel.sys_info.uptime_seconds.load(std::memory_order_relaxed);
    if (secs > 0)
        status_uptime->setText(QString("Up: %1h %2m")
            .arg(secs / 3600).arg((secs % 3600) / 60));

    // Refresh overview plots if visible
    if (page_stack->currentIndex() == 0)
        page_overview->refresh(tel, last_pkts, last_bytes);

    // Sync service page status indicators if visible
    if (page_stack->currentIndex() == 3)
        page_services->refresh_status();

    // Refresh device list if visible (checks device_count change internally, cheap if no change)
    if (page_stack->currentIndex() == 4)
        page_devices->refresh();

    // Snapshot current counters for next delta
    for (int i = 0; i < 4; ++i) {
        last_pkts[i]  = tel.core_metrics[i].pkts.load(std::memory_order_relaxed);
        last_bytes[i] = tel.core_metrics[i].bytes.load(std::memory_order_relaxed);
    }
}

} // namespace Scalpel::GUI
