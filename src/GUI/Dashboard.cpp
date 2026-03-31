#include "GUI/Dashboard.hpp"
#include "GUI/StyleSheet.hpp"
#include "Config.hpp"
#include "SystemOptimizer.hpp"
#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QTimerEvent>
#include <QResizeEvent>
#include <QFormLayout>
#include <QScrollArea>
#include <QMessageBox>
#include <thread>
#include <print>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/eventfd.h>
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

    auto* header_lbl = new QLabel("通知中心");
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
    if (val > target_max) target_max = val * 1.2;
    else target_max = std::max(1.0, target_max * 0.99);
}

void RealTimePlot::step_physics() {
    constexpr double spring_k = 0.15, damper = 0.1;
    double force = spring_k * (target_max - current_max);
    current_velocity = (current_velocity + force) * (1.0 - damper);
    current_max += current_velocity;
    if (current_max < 1.0) current_max = 1.0;
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

void OverviewPage::refresh(const Telemetry& tel, uint64_t last_p[4], uint64_t last_b[4]) {
    double total_pps = 0, total_bps = 0;
    for (int i = 0; i < 4; ++i) {
        uint64_t cp = tel.core_metrics[i].pkts.load(std::memory_order_relaxed);
        uint64_t cb = tel.core_metrics[i].bytes.load(std::memory_order_relaxed);
        uint64_t dp = cp - last_p[i], db = cb - last_b[i];
        total_pps += dp * 25.0;   // 25Hz tick: scale 40ms delta to per-second rate
        total_bps += db * 25.0;
        core_labels[i]->setText(QString("Core %1\n%2 Pkts\n%3 KB")
            .arg(i).arg(cp).arg(cb / 1024));
    }
    pps_plot->add_sample(total_pps);
    bps_plot->add_sample(total_bps);
    pps_plot->step_physics();
    bps_plot->step_physics();
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

    auto* title = new QLabel("网络接口角色分配");
    title->setObjectName("section_title");
    layout->addWidget(title);

    auto* desc = new QLabel("为每个检测到的网络接口分配角色。恰好一个接口须设置为「默认网关」。");
    desc->setStyleSheet("color: #707080; font-size: 12px;");
    desc->setWordWrap(true);
    layout->addWidget(desc);

    // Role table: Interface | Role | Link state
    iface_table = new QTableWidget(0, 3);
    iface_table->setHorizontalHeaderLabels({"接口", "角色", "链路状态"});
    iface_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    iface_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    iface_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    iface_table->verticalHeader()->setVisible(false);
    iface_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    iface_table->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(iface_table);

    scan_interfaces();

    // Advanced options
    auto* adv_group = new QGroupBox("高级选项");
    auto* adv_form = new QFormLayout(adv_group);
    chk_stp = new QCheckBox("开启 STP");
    chk_stp->setChecked(Config::ENABLE_STP.load(std::memory_order_relaxed));
    adv_form->addRow("生成树协议:", chk_stp);
    chk_igmp = new QCheckBox("启用 IGMP 嗅探");
    chk_igmp->setChecked(Config::ENABLE_IGMP_SNOOPING.load(std::memory_order_relaxed));
    adv_form->addRow("IGMP Snooping:", chk_igmp);
    layout->addWidget(adv_group);

    // Button row
    auto* btn_row = new QHBoxLayout();
    btn_refresh = new QPushButton("刷新网口");
    connect(btn_refresh, &QPushButton::clicked, this, &InterfacePage::on_refresh_clicked);
    btn_row->addWidget(btn_refresh);
    btn_row->addStretch();
    auto* btn_reset = new QPushButton("重置");
    connect(btn_reset, &QPushButton::clicked, this, &InterfacePage::on_reset_clicked);
    btn_row->addWidget(btn_reset);
    auto* btn_save = new QPushButton("保存并应用");
    btn_save->setObjectName("btn_primary");
    connect(btn_save, &QPushButton::clicked, this, &InterfacePage::on_save_clicked);
    btn_row->addWidget(btn_save);
    layout->addLayout(btn_row);

    // Register QSocketNotifier for done_fd — wakes Qt event loop when watchdog completes rescan
    int done_fd = Telemetry::instance().sys_info.done_fd;
    if (done_fd >= 0) {
        auto* notifier = new QSocketNotifier(done_fd, QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, &InterfacePage::on_scan_done);
    }
}

void InterfacePage::scan_interfaces() {
    iface_table->setRowCount(0);
    role_combos.clear();

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

        Config::IfaceRole role = Config::IfaceRole::DISABLED;
        if (Config::IFACE_ROLES.count(name)) {
            role = Config::IFACE_ROLES.at(name);
        } else if (name == "eth0") {
            role = Config::IfaceRole::GATEWAY;
        } else if (name == "eth1") {
            role = Config::IfaceRole::LAN;
        }

        int row = iface_table->rowCount();
        iface_table->insertRow(row);

        auto* name_item = new QTableWidgetItem(QString::fromStdString(name));
        name_item->setTextAlignment(Qt::AlignCenter);
        iface_table->setItem(row, 0, name_item);

        auto* combo = new QComboBox();
        combo->addItems({"外网 (WAN)", "内网 (LAN)", "默认网关", "禁用"});
        combo->setCurrentIndex(static_cast<int>(role));
        role_combos[name] = combo;
        iface_table->setCellWidget(row, 1, combo);

        bool is_up = (state == "up");
        auto* state_lbl = new QLabel(is_up ? "● 已连接" : "○ 断开");
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
    for (auto& [name, combo] : role_combos) {
        if (name != iface.toStdString() && combo->currentIndex() == 2) {
            QSignalBlocker blocker(combo);
            combo->setCurrentIndex(0); // demote to WAN
        }
    }
}

void InterfacePage::on_save_clicked() {
    // Validate: exactly one gateway must be assigned
    int gw_count = 0;
    std::string gw_iface;
    for (const auto& [name, combo] : role_combos) {
        if (combo->currentIndex() == 2) { ++gw_count; gw_iface = name; }
    }
    if (gw_count == 0) {
        QMessageBox::warning(this, "配置错误", "请指定一个默认网关接口。");
        return;
    }

    // Persist role map
    Config::IFACE_ROLES.clear();
    for (const auto& [name, combo] : role_combos)
        Config::IFACE_ROLES[name] = static_cast<Config::IfaceRole>(combo->currentIndex());

    // Derive legacy variables consumed by App
    Config::IFACE_GATEWAY = gw_iface;
    Config::IFACE_WAN = gw_iface;
    Config::BRIDGED_INTERFACES.clear();
    for (const auto& [name, combo] : role_combos)
        if (combo->currentIndex() == 1) Config::BRIDGED_INTERFACES.push_back(name);
    Config::IFACE_LAN = Config::BRIDGED_INTERFACES.empty() ? "" : Config::BRIDGED_INTERFACES[0];

    Config::ENABLE_STP.store(chk_stp->isChecked(), std::memory_order_relaxed);
    Config::ENABLE_IGMP_SNOOPING.store(chk_igmp->isChecked(), std::memory_order_relaxed);
    Config::ENABLE_ACCELERATION.store(true, std::memory_order_relaxed);
    Telemetry::instance().bridge_mode.store(true, std::memory_order_relaxed);

    std::println("[GUI] Interface roles saved. Gateway: {}, LAN interfaces: {}",
        Config::IFACE_GATEWAY, Config::BRIDGED_INTERFACES.size());
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
    btn_refresh->setText("扫描中…");
}

void InterfacePage::on_scan_done() {
    // Drain the eventfd counter before reading Telemetry cache
    uint64_t val;
    ::eventfd_read(Telemetry::instance().sys_info.done_fd, &val);
    scan_interfaces();
    btn_refresh->setEnabled(true);
    btn_refresh->setText("刷新网口");
}

// ═════════════════════════════════════════════════════════════
// QosPage: QoS 流量控制页
// ═════════════════════════════════════════════════════════════
QosPage::QosPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel("QoS 与流量控制");
    title->setObjectName("section_title");
    layout->addWidget(title);

    // 加速开关
    chk_acceleration = new QCheckBox("启用游戏流量加速 (启发式优先级调度)");
    chk_acceleration->setChecked(Config::ENABLE_ACCELERATION.load(std::memory_order_relaxed));
    connect(chk_acceleration, &QCheckBox::toggled, this, &QosPage::on_toggle_accel);
    layout->addWidget(chk_acceleration);

    // 带宽限制
    auto* bw_group = new QGroupBox("全局带宽限制");
    auto* bw_form = new QFormLayout(bw_group);
    edit_dl_limit = new QLineEdit("500");
    edit_ul_limit = new QLineEdit("50");
    bw_form->addRow("下行限制 (Mbps):", edit_dl_limit);
    bw_form->addRow("上行限制 (Mbps):", edit_ul_limit);
    layout->addWidget(bw_group);

    // IP 限速规则表
    auto* rules_group = new QGroupBox("IP 限速规则");
    auto* rules_lay = new QVBoxLayout(rules_group);

    rules_table = new QTableWidget(0, 3);
    rules_table->setHorizontalHeaderLabels({"IP 地址", "速率限制 (Mbps)", "操作"});
    rules_table->horizontalHeader()->setStretchLastSection(true);
    rules_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    rules_table->verticalHeader()->setVisible(false);
    rules_lay->addWidget(rules_table);

    // 从当前配置加载
    for (auto& [ip, rate] : Config::IP_LIMIT_MAP) {
        int row = rules_table->rowCount();
        rules_table->insertRow(row);
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip);
        rules_table->setItem(row, 0, new QTableWidgetItem(
            QString("%1.%2.%3.%4").arg(b[0]).arg(b[1]).arg(b[2]).arg(b[3])));
        rules_table->setItem(row, 1, new QTableWidgetItem(QString::number(rate)));
        auto* btn_del = new QPushButton("删除");
        btn_del->setObjectName("btn_danger");
        btn_del->setFixedHeight(32);
        connect(btn_del, &QPushButton::clicked, this, &QosPage::on_remove_rule);
        rules_table->setCellWidget(row, 2, btn_del);
    }

    auto* btn_row = new QHBoxLayout();
    btn_row->addStretch();
    btn_add_rule = new QPushButton("+ 添加规则");
    btn_add_rule->setObjectName("btn_primary");
    connect(btn_add_rule, &QPushButton::clicked, this, &QosPage::on_add_rule);
    btn_row->addWidget(btn_add_rule);
    rules_lay->addLayout(btn_row);
    layout->addWidget(rules_group);
    layout->addStretch();
}

void QosPage::on_toggle_accel() {
    bool on = chk_acceleration->isChecked();
    Config::ENABLE_ACCELERATION.store(on, std::memory_order_relaxed);
    Telemetry::instance().bridge_mode.store(!on, std::memory_order_relaxed);
    std::println("[GUI] 加速模式: {}", on ? "ON" : "OFF");
}

void QosPage::on_add_rule() {
    int row = rules_table->rowCount();
    rules_table->insertRow(row);
    rules_table->setItem(row, 0, new QTableWidgetItem("192.168.1.100"));
    rules_table->setItem(row, 1, new QTableWidgetItem("100"));
    auto* btn_del = new QPushButton("删除");
    btn_del->setObjectName("btn_danger");
    btn_del->setFixedHeight(32);
    connect(btn_del, &QPushButton::clicked, this, &QosPage::on_remove_rule);
    rules_table->setCellWidget(row, 2, btn_del);
}

void QosPage::on_remove_rule() {
    auto* btn = qobject_cast<QPushButton*>(sender());
    if (!btn) return;
    for (int r = 0; r < rules_table->rowCount(); ++r) {
        if (rules_table->cellWidget(r, 2) == btn) {
            rules_table->removeRow(r);
            return;
        }
    }
}

// ═════════════════════════════════════════════════════════════
// ServicePage: 服务开关页
// ═════════════════════════════════════════════════════════════
ServicePage::ServicePage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel("服务管理");
    title->setObjectName("section_title");
    layout->addWidget(title);

    auto* desc = new QLabel("启用或禁用各核心网络服务。更改立即生效，无需重启。");
    desc->setStyleSheet("color: #707080; font-size: 13px; margin-bottom: 8px;");
    layout->addWidget(desc);

    struct ServiceDef {
        const char* name;
        const char* description;
        std::atomic<bool>* state;
    };

    ServiceDef defs[5] = {
        {"NAT (网络地址转换)", "用户态零拷贝高速 SNAT/DNAT 引擎", &Config::global_state.enable_nat},
        {"DHCP (动态主机配置)", "自动为局域网设备分配 IP 地址", &Config::global_state.enable_dhcp},
        {"DNS 缓存", "本地 DNS 缓存加速解析", &Config::global_state.enable_dns_cache},
        {"防火墙", "入站流量过滤规则引擎", &Config::global_state.enable_firewall},
        {"UPnP (即插即用)", "自动端口映射以支持 NAT 穿越", &Config::global_state.enable_upnp},
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

        rows[i].status_label = new QLabel("● 运行中");
        rows[i].status_label->setStyleSheet("color: #00cc66; font-weight: bold;");

        auto* text_col = new QVBoxLayout();
        text_col->addWidget(rows[i].chk);
        text_col->addWidget(desc_lbl);
        row_lay->addLayout(text_col, 1);
        row_lay->addWidget(rows[i].status_label);

        // 连接信号
        auto* state_ptr = defs[i].state;
        auto* status_lbl = rows[i].status_label;
        connect(rows[i].chk, &QCheckBox::toggled, [state_ptr, status_lbl](bool checked) {
            state_ptr->store(checked, std::memory_order_relaxed);
            status_lbl->setText(checked ? "● 运行中" : "○ 已停止");
            status_lbl->setStyleSheet(checked ? "color: #00cc66; font-weight: bold;" : "color: #cc3333; font-weight: bold;");
            std::println("[GUI] 服务状态已更新");
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
        rows[i].status_label->setText(on ? "● 运行中" : "○ 已停止");
        rows[i].status_label->setStyleSheet(on ? "color: #00cc66; font-weight: bold;" : "color: #cc3333; font-weight: bold;");
    }
}

// ═════════════════════════════════════════════════════════════
// SystemPage: 系统管理页
// ═════════════════════════════════════════════════════════════
SystemPage::SystemPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel("系统信息");
    title->setObjectName("section_title");
    layout->addWidget(title);

    auto* info_group = new QGroupBox("硬件与运行时");
    auto* info_form = new QFormLayout(info_group);
    info_form->setSpacing(10);
    lbl_hostname = new QLabel("--");
    lbl_kernel = new QLabel("--");
    lbl_cpu_temp = new QLabel("--");
    lbl_uptime = new QLabel("--");
    lbl_memory = new QLabel("--");
    info_form->addRow("主机名:", lbl_hostname);
    info_form->addRow("内核版本:", lbl_kernel);
    info_form->addRow("CPU 温度:", lbl_cpu_temp);
    info_form->addRow("运行时间:", lbl_uptime);
    info_form->addRow("内存使用:", lbl_memory);
    layout->addWidget(info_group);

    auto* cfg_group = new QGroupBox("配置管理");
    auto* cfg_lay = new QVBoxLayout(cfg_group);
    auto* cfg_form = new QFormLayout();
    edit_config_path = new QLineEdit("config.txt");
    cfg_form->addRow("配置文件路径:", edit_config_path);
    cfg_lay->addLayout(cfg_form);

    auto* btn_row = new QHBoxLayout();
    auto* btn_save = new QPushButton("保存配置到文件");
    btn_save->setObjectName("btn_primary");
    connect(btn_save, &QPushButton::clicked, this, &SystemPage::on_save_config);
    btn_row->addWidget(btn_save);
    auto* btn_restart = new QPushButton("重启引擎");
    btn_restart->setObjectName("btn_danger");
    connect(btn_restart, &QPushButton::clicked, this, &SystemPage::on_restart_engine);
    btn_row->addWidget(btn_restart);
    cfg_lay->addLayout(btn_row);
    layout->addWidget(cfg_group);
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
        lbl_uptime->setText(QString("%1 小时 %2 分钟").arg(h).arg(m));
    }

    uint64_t total = si.mem_total_kb.load(std::memory_order_relaxed);
    uint64_t avail = si.mem_avail_kb.load(std::memory_order_relaxed);
    if (total > 0)
        lbl_memory->setText(QString("已用 %1 MB / 共 %2 MB")
            .arg((total - avail) / 1024).arg(total / 1024));
}

void SystemPage::on_save_config() {
    std::string path = edit_config_path->text().toStdString();
    std::thread([path](){
        Scalpel::System::Optimizer::set_current_thread_affinity(1); // Core 1: Control Plane
        Config::save_config(path);
    }).detach();
}

void SystemPage::on_restart_engine() {
    std::println("[GUI] Engine restart triggered (requires manual execution)");
}

// ═════════════════════════════════════════════════════════════
// PlaceholderPage: 占位页 (Docker / VPN)
// ═════════════════════════════════════════════════════════════
PlaceholderPage::PlaceholderPage(const QString& name, QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel(name);
    title->setObjectName("section_title");
    layout->addWidget(title);
    
    auto* msg = new QLabel(QString("'%1' 功能模块正在构建中...\n\n该模块将包含: \n - 容器生命周期管理\n - 镜像快速部署\n - 虚拟化网络隔绝").arg(name));
    msg->setStyleSheet("color: #707080; font-size: 15px;");
    msg->setAlignment(Qt::AlignCenter);
    layout->addStretch();
    layout->addWidget(msg);
    layout->addStretch();
}

// ═════════════════════════════════════════════════════════════
// Dashboard: 主控面板框架
// ═════════════════════════════════════════════════════════════
Dashboard::Dashboard(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Scalpel Gaming Router");
    setStyleSheet(DARK_STYLESHEET);
    setup_ui();
    // Data timer: 25Hz (40ms), always on — feeds RealTimePlot and router stats labels
    data_timer_id_ = startTimer(40, Qt::CoarseTimer);
}

void Dashboard::setup_ui() {
    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    auto* root_layout = new QVBoxLayout(central);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    // ── 顶部标题栏 ──
    auto* header = new QFrame();
    header->setObjectName("header_frame");
    auto* header_lay = new QHBoxLayout(header);
    header_lay->setContentsMargins(16, 8, 16, 8);
    auto* title = new QLabel("⚡ Scalpel Gaming Router");
    title->setObjectName("header_title");
    header_lay->addWidget(title);
    header_lay->addStretch();
    auto* status_dot = new QLabel("● 运行中");
    status_dot->setStyleSheet("color: #00cc66; font-weight: bold; font-size: 14px;");
    header_lay->addWidget(status_dot);

    auto* btn_notif = new QPushButton("🔔");
    btn_notif->setFixedSize(36, 36);
    btn_notif->setStyleSheet("QPushButton { background: transparent; font-size: 18px; border: none; }");
    connect(btn_notif, &QPushButton::clicked, this, &Dashboard::on_notif_toggle_clicked);
    header_lay->addWidget(btn_notif);

    auto* btn_shutdown = new QPushButton("关闭程序");
    btn_shutdown->setObjectName("btn_danger");
    connect(btn_shutdown, &QPushButton::clicked, this, &Dashboard::on_shutdown_clicked);
    header_lay->addWidget(btn_shutdown);

    root_layout->addWidget(header);

    // ── 中央区域：导航 + 堆栈 ──
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
    page_docker = new PlaceholderPage("🐳 DOCKER 管理");
    page_vpn = new PlaceholderPage("🔐 VPN / IPsec 安全传输");
    page_system = new SystemPage();

    page_stack->addWidget(page_overview);
    page_stack->addWidget(page_interfaces);
    page_stack->addWidget(page_qos);
    page_stack->addWidget(page_services);
    page_stack->addWidget(page_docker);
    page_stack->addWidget(page_vpn);
    page_stack->addWidget(page_system);
    body_layout->addWidget(page_stack, 1);

    root_layout->addLayout(body_layout, 1);

    // ── 底部状态栏 ──
    setup_statusbar();

    // ── 通知面板：浮动在所有内容之上，初始隐藏于屏幕顶部外侧 ──
    notif_panel_ = new NotificationPanel(centralWidget());
    notif_panel_->setFixedWidth(centralWidget()->width());
    notif_panel_->raise();
    notif_panel_->push_notification("路由器已就绪", "数据平面 Core 2/3 挂载完成，转发引擎运行中。");
}

void Dashboard::setup_nav() {
    nav_list = new QListWidget();
    nav_list->setObjectName("nav_list");
    nav_list->setFixedWidth(160);
    nav_list->setIconSize(QSize(0, 0));

    QStringList items = {"📊 总览", "🌐 接口 (LAN/WAN)", "⚡ QoS", "🔧 服务 (NAT/DHCP)", "🐳 DOCKER", "🔐 VPN / IPsec", "💻 系统"};
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
    // 切换到系统页时刷新信息
    if (index == 6) page_system->refresh_info();
}

void Dashboard::on_shutdown_clicked() {
    // QApplication::quit() 使 qapp.exec() 返回，main.cpp 中的关闭流程
    // 负责调用 app.stop() 并等待所有 jthread 安全退出。
    QApplication::quit();
}

void Dashboard::on_notif_toggle_clicked() {
    notif_panel_->set_expanded(!notif_panel_->is_expanded());
    enter_anim_mode();
}

void Dashboard::enter_anim_mode() {
    if (anim_timer_id_ != -1) return;
    // 60Hz animation timer: vsync-synced by Qt Wayland compositor
    anim_timer_id_ = startTimer(16, Qt::PreciseTimer);
}

void Dashboard::exit_anim_mode() {
    if (anim_timer_id_ == -1) return;
    killTimer(anim_timer_id_);
    anim_timer_id_ = -1;
}

void Dashboard::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (notif_panel_)
        notif_panel_->setFixedWidth(centralWidget()->width());
}

void Dashboard::timerEvent(QTimerEvent* event) {
    const int id = event->timerId();

    if (id == anim_timer_id_) {
        // Animation frame: advance spring physics, repaint if still moving
        notif_panel_->advance_spring();
        notif_panel_->update();
        if (notif_panel_->is_settled()) exit_anim_mode();
        return;
    }

    if (id != data_timer_id_) return;

    // Data tick (25Hz / 40ms): feed plots and update router stats labels
    data_tick_++;
    auto& tel = Telemetry::instance();

    // Bandwidth: delta over 40ms tick, scaled to Mbps (×25 = per-second rate)
    uint64_t cur_b2 = tel.core_metrics[2].bytes.load(std::memory_order_relaxed);
    uint64_t cur_b3 = tel.core_metrics[3].bytes.load(std::memory_order_relaxed);
    double dl = (cur_b2 - last_bytes[2]) * 8.0 * 25.0 / 1e6;
    double ul = (cur_b3 - last_bytes[3]) * 8.0 * 25.0 / 1e6;
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

    // Snapshot current counters for next delta
    for (int i = 0; i < 4; ++i) {
        last_pkts[i]  = tel.core_metrics[i].pkts.load(std::memory_order_relaxed);
        last_bytes[i] = tel.core_metrics[i].bytes.load(std::memory_order_relaxed);
    }
}

} // namespace Scalpel::GUI
