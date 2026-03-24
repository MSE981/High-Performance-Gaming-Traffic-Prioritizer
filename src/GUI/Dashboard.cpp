#include "GUI/Dashboard.hpp"
#include "GUI/StyleSheet.hpp"
#include "Config.hpp"
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QTimerEvent>
#include <QFormLayout>
#include <QScrollArea>
#include <QMessageBox>
#include <thread>
#include <print>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace Scalpel::GUI {

// ═════════════════════════════════════════════════════════════
// RealTimePlot (保留 Phase 3 物理引擎)
// ═════════════════════════════════════════════════════════════
RealTimePlot::RealTimePlot(QWidget* parent) : QWidget(parent) {
    setMinimumSize(200, 120);
    shift_buffer.resize(SHIFT_BUFFER_SIZE, 0.0);
}

void RealTimePlot::add_sample(double val) {
    shift_buffer.pop_front();
    shift_buffer.push_back(val);
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
        double y = h - (shift_buffer[i] / (current_max + 1.0) * h * 0.8) - 10;
        if (i == 0) path.moveTo(0, y); else path.lineTo(i * x_step, y);
    }
    painter.setPen(QPen(QColor(0, 150, 255, 40), 8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);
    painter.setPen(QPen(QColor(200, 240, 255), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);
}

// ═════════════════════════════════════════════════════════════
// OverviewPage: 总览页
// ═════════════════════════════════════════════════════════════
OverviewPage::OverviewPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);

    auto* title = new QLabel("系统总览");
    title->setObjectName("section_title");
    layout->addWidget(title);

    // 模式与性能标签
    auto* info_row = new QHBoxLayout();
    lbl_mode = new QLabel("模式: 加速");
    lbl_cpu_capacity = new QLabel("CPU 处理能力: --");
    lbl_mode->setStyleSheet("color: #00cc66; font-weight: bold; font-size: 15px;");
    lbl_cpu_capacity->setStyleSheet("color: #ffaa00; font-weight: bold; font-size: 15px;");
    info_row->addWidget(lbl_mode);
    info_row->addStretch();
    info_row->addWidget(lbl_cpu_capacity);
    layout->addLayout(info_row);

    // 双图表行
    auto* plot_row = new QHBoxLayout();
    auto* pps_group = new QGroupBox("包速率 (PPS)");
    auto* pps_lay = new QVBoxLayout(pps_group);
    pps_plot = new RealTimePlot();
    pps_lay->addWidget(pps_plot);
    plot_row->addWidget(pps_group);

    auto* bps_group = new QGroupBox("带宽 (Bytes/s)");
    auto* bps_lay = new QVBoxLayout(bps_group);
    bps_plot = new RealTimePlot();
    bps_lay->addWidget(bps_plot);
    plot_row->addWidget(bps_group);
    layout->addLayout(plot_row);

    // 四核心状态行
    auto* cores_row = new QHBoxLayout();
    for (int i = 0; i < 4; ++i) {
        core_labels[i] = new QLabel(QString("Core %1: 空闲").arg(i));
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
        total_pps += dp * 25.0;
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

    lbl_mode->setText(tel.bridge_mode.load() ? "模式: 桥接透传" : "模式: 加速转发");
    double cap = tel.internal_limit_mbps.load();
    if (cap > 0) lbl_cpu_capacity->setText(QString("CPU 处理能力: %1 Mbps").arg(cap, 0, 'f', 1));
}

// ═════════════════════════════════════════════════════════════
// InterfacePage: 接口配置页
// ═════════════════════════════════════════════════════════════
InterfacePage::InterfacePage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel("接口配置");
    title->setObjectName("section_title");
    layout->addWidget(title);

    tab_widget = new QTabWidget();

    // === WAN 标签页 ===
    auto* wan_page = new QWidget();
    auto* wan_form = new QFormLayout(wan_page);
    wan_form->setSpacing(12);
    wan_iface_edit = new QLineEdit(QString::fromStdString(Config::IFACE_WAN));
    wan_form->addRow("WAN 接口名称:", wan_iface_edit);
    auto* wan_desc = new QLabel("上游广域网接口 (连接互联网)");
    wan_desc->setStyleSheet("color: #707080; font-size: 12px;");
    wan_form->addRow("", wan_desc);
    tab_widget->addTab(wan_page, "WAN (eth0)");

    // === LAN 标签页 ===
    auto* lan_page = new QWidget();
    auto* lan_form = new QFormLayout(lan_page);
    lan_form->setSpacing(12);
    lan_iface_edit = new QLineEdit(QString::fromStdString(Config::IFACE_LAN));
    lan_form->addRow("LAN 接口名称:", lan_iface_edit);
    chk_bridge = new QCheckBox("启用桥接模式 (透传所有流量)");
    chk_bridge->setChecked(!Config::ENABLE_ACCELERATION.load());
    lan_form->addRow("桥接接口:", chk_bridge);
    auto* lan_desc = new QLabel("局域网接口 (连接内网设备)");
    lan_desc->setStyleSheet("color: #707080; font-size: 12px;");
    lan_form->addRow("", lan_desc);
    tab_widget->addTab(lan_page, "LAN (eth1)");

    layout->addWidget(tab_widget);

    // 按钮行
    auto* btn_row = new QHBoxLayout();
    btn_row->addStretch();
    auto* btn_reset = new QPushButton("重置");
    connect(btn_reset, &QPushButton::clicked, this, &InterfacePage::on_reset_clicked);
    btn_row->addWidget(btn_reset);
    auto* btn_save = new QPushButton("保存并应用");
    btn_save->setObjectName("btn_primary");
    connect(btn_save, &QPushButton::clicked, this, &InterfacePage::on_save_clicked);
    btn_row->addWidget(btn_save);
    layout->addLayout(btn_row);
    layout->addStretch();
}

void InterfacePage::on_save_clicked() {
    std::jthread([](){
        // 异步下发，不阻塞 GUI
        // 实际的接口切换需要底层 socket 重建，此处仅保存配置
        std::println("[GUI] 接口配置已保存。");
    }).detach();
    Config::IFACE_WAN = wan_iface_edit->text().toStdString();
    Config::IFACE_LAN = lan_iface_edit->text().toStdString();
    Config::ENABLE_ACCELERATION.store(!chk_bridge->isChecked());
    Telemetry::instance().bridge_mode.store(chk_bridge->isChecked(), std::memory_order_relaxed);
}

void InterfacePage::on_reset_clicked() {
    wan_iface_edit->setText(QString::fromStdString(Config::IFACE_WAN));
    lan_iface_edit->setText(QString::fromStdString(Config::IFACE_LAN));
    chk_bridge->setChecked(!Config::ENABLE_ACCELERATION.load());
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
    chk_acceleration->setChecked(Config::ENABLE_ACCELERATION.load());
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
    std::jthread([on](){
        Config::ENABLE_ACCELERATION.store(on);
        Telemetry::instance().bridge_mode.store(!on, std::memory_order_relaxed);
        std::println("[GUI] 加速模式: {}", on ? "ON" : "OFF");
    }).detach();
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
    // 读取 /proc 信息 (Linux only)
    auto read_file = [](const char* path) -> std::string {
        std::ifstream f(path);
        if (!f.is_open()) return "--";
        std::string s; std::getline(f, s); return s;
    };

    lbl_hostname->setText(QString::fromStdString(read_file("/etc/hostname")));
    
    std::string kver = read_file("/proc/version");
    if (kver.size() > 60) kver = kver.substr(0, 60) + "...";
    lbl_kernel->setText(QString::fromStdString(kver));

    std::string temp_str = read_file("/sys/class/thermal/thermal_zone0/temp");
    if (temp_str != "--") {
        double t = std::stod(temp_str) / 1000.0;
        lbl_cpu_temp->setText(QString("%1 °C").arg(t, 0, 'f', 1));
        if (t > 70) lbl_cpu_temp->setStyleSheet("color: #ff4444; font-weight: bold;");
        else lbl_cpu_temp->setStyleSheet("color: #00cc66;");
    }

    std::string up = read_file("/proc/uptime");
    if (up != "--") {
        double secs = std::stod(up);
        int h = (int)(secs / 3600), m = (int)((secs - h*3600) / 60);
        lbl_uptime->setText(QString("%1 小时 %2 分钟").arg(h).arg(m));
    }

    std::ifstream memf("/proc/meminfo");
    if (memf.is_open()) {
        std::string line; uint64_t total = 0, avail = 0;
        while (std::getline(memf, line)) {
            if (line.starts_with("MemTotal:")) sscanf(line.c_str(), "MemTotal: %lu", &total);
            if (line.starts_with("MemAvailable:")) sscanf(line.c_str(), "MemAvailable: %lu", &avail);
        }
        if (total > 0) lbl_memory->setText(QString("已用 %1 MB / 共 %2 MB")
            .arg((total - avail) / 1024).arg(total / 1024));
    }
}

void SystemPage::on_save_config() {
    std::string path = edit_config_path->text().toStdString();
    std::jthread([path](){
        std::ofstream f(path);
        if (!f.is_open()) { std::println(stderr, "[GUI] 无法写入配置文件: {}", path); return; }
        f << "IFACE_WAN=" << Config::IFACE_WAN << "\n";
        f << "IFACE_LAN=" << Config::IFACE_LAN << "\n";
        f << "ENABLE_ACCELERATION=" << (Config::ENABLE_ACCELERATION.load() ? "true" : "false") << "\n";
        f << "enable_gui=true\n";
        f << "LARGE_PACKET_THRESHOLD=" << Config::LARGE_PACKET_THRESHOLD << "\n";
        f << "PUNISH_TRIGGER_COUNT=" << Config::PUNISH_TRIGGER_COUNT << "\n";
        f << "CLEANUP_INTERVAL=" << Config::CLEANUP_INTERVAL << "\n";
        for (auto& [ip, rate] : Config::IP_LIMIT_MAP) {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip);
            f << "IP_LIMIT=" << (int)b[0] << "." << (int)b[1] << "." << (int)b[2] << "." << (int)b[3] << ":" << rate << "\n";
        }
        std::println("[GUI] 配置已保存至: {}", path);
    }).detach();
}

void SystemPage::on_restart_engine() {
    std::println("[GUI] 重启引擎操作已触发 (需手动执行)");
}

// ═════════════════════════════════════════════════════════════
// Dashboard: 主控面板框架
// ═════════════════════════════════════════════════════════════
Dashboard::Dashboard(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Scalpel Gaming Router");
    setStyleSheet(DARK_STYLESHEET);
    setup_ui();
    refresh_timer_id = startTimer(40); // 25Hz
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
    page_system = new SystemPage();

    page_stack->addWidget(page_overview);
    page_stack->addWidget(page_interfaces);
    page_stack->addWidget(page_qos);
    page_stack->addWidget(page_services);
    page_stack->addWidget(page_system);
    body_layout->addWidget(page_stack, 1);

    root_layout->addLayout(body_layout, 1);

    // ── 底部状态栏 ──
    setup_statusbar();
}

void Dashboard::setup_nav() {
    nav_list = new QListWidget();
    nav_list->setObjectName("nav_list");
    nav_list->setFixedWidth(160);
    nav_list->setIconSize(QSize(0, 0));

    QStringList items = {"📊 总览", "🌐 接口", "⚡ QoS", "🔧 服务", "💻 系统"};
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
    if (index == 4) page_system->refresh_info();
}

void Dashboard::timerEvent(QTimerEvent* event) {
    if (event->timerId() != refresh_timer_id) return;
    tick_counter++;

    auto& tel = Telemetry::instance();

    // 每 40ms 刷新总览页 (如果可见)
    if (page_stack->currentIndex() == 0) {
        page_overview->refresh(tel, last_pkts, last_bytes);
    }

    // 保存当前统计快照
    for (int i = 0; i < 4; ++i) {
        last_pkts[i] = tel.core_metrics[i].pkts.load(std::memory_order_relaxed);
        last_bytes[i] = tel.core_metrics[i].bytes.load(std::memory_order_relaxed);
    }

    // 每 1 秒 (25 ticks) 刷新状态栏
    if (tick_counter % 25 == 0) {
        // 带宽统计
        double dl = 0, ul = 0;
        dl = tel.core_metrics[2].bytes.load(std::memory_order_relaxed) * 8.0 / 1e6;
        ul = tel.core_metrics[3].bytes.load(std::memory_order_relaxed) * 8.0 / 1e6;
        status_dl->setText(QString("↓ %1 Mbps").arg(dl, 0, 'f', 2));
        status_ul->setText(QString("↑ %1 Mbps").arg(ul, 0, 'f', 2));

        // CPU 温度
        std::ifstream tf("/sys/class/thermal/thermal_zone0/temp");
        if (tf.is_open()) {
            double t; tf >> t; t /= 1000.0;
            status_cpu->setText(QString("CPU: %1°C").arg(t, 0, 'f', 0));
        }

        // 服务页同步
        if (page_stack->currentIndex() == 3) page_services->refresh_status();
    }
}

} // namespace Scalpel::GUI
