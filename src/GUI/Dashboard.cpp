#include "GUI/Dashboard.hpp"
#include "GUI/StyleSheet.hpp"
#include "Config.hpp"
#include "SelfTest.hpp"
#include <cstring>
#include "SystemOptimizer.hpp"
#include <QApplication>
#include <QScreen>
#include <QMetaObject>
#include <QButtonGroup>
#include <QFile>
#include <QTextStream>
#include <QMenu>
#include <QBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QTimerEvent>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QEvent>
#include <QPoint>
#include <QFormLayout>
#include <QScrollArea>
#include <QRegularExpressionValidator>
#include <QDoubleValidator>
#include <QLocale>
#include <QDoubleSpinBox>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <thread>
#include <mutex>
#include <print>
#include <algorithm>
#include <array>
#include <string_view>
#include <vector>
#include <optional>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>

namespace HPGTP::GUI {

constexpr double k_qos_bw_min_mbps     = 0.1;
constexpr double k_qos_bw_max_mbps     = 1e6;
constexpr double k_device_bw_min_mbps  = 0.1;
constexpr double k_device_bw_max_mbps  = 10000.0;
constexpr int    k_port_min            = 1;
constexpr int    k_port_max            = 65535;

// ═════════════════════════════════════════════════════════════
// NotificationPanel: iOS-style pull-down overlay
// Spring constants tuned for a snappy-but-not-bouncy feel
// ═════════════════════════════════════════════════════════════
// SwitchToggle: pill track + round thumb (accent #0077ff, muted track #2a2a4a)
SwitchToggle::SwitchToggle(QWidget* parent) : QWidget(parent) {
    setFixedSize(58, 32);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void SwitchToggle::setChecked(bool on) {
    if (checked_ == on) return;
    checked_ = on;
    update();
}

void SwitchToggle::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint())) {
        checked_ = !checked_;
        update();
        emit toggled(checked_);
    }
    QWidget::mouseReleaseEvent(e);
}

void SwitchToggle::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    QRect r = rect().adjusted(1, 1, -2, -2);
    const QColor trackOn(0x00, 0x77, 0xff);
    const QColor trackOff(0x2a, 0x2a, 0x4a);
    const QColor border(0x3a, 0x3a, 0x5a);
    const QColor thumb(0xee, 0xee, 0xf5);
    p.setPen(QPen(border, 1));
    p.setBrush(checked_ ? trackOn : trackOff);
    p.drawRoundedRect(r, r.height() / 2.0, r.height() / 2.0);
    const int diam = r.height() - 6;
    const int x    = checked_ ? (r.right() - diam - 2) : (r.left() + 2);
    const int y    = r.center().y() - diam / 2;
    p.setPen(Qt::NoPen);
    p.setBrush(thumb);
    p.drawEllipse(QRect(x, y, diam, diam));
}

NotificationPanel::NotificationPanel(QWidget* parent) : QFrame(parent) {
    // Height set dynamically by Dashboard (full-screen overlay)
    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet("NotificationPanel { background: transparent; }");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Notification card area (top 1250px, rounded bottom corners) ──
    auto* card_panel = new QFrame();
    card_panel->setObjectName("notif_card_panel");
    card_panel->setStyleSheet(
        "QFrame#notif_card_panel {"
        "  background: transparent;"
        "  border-bottom-left-radius: 24px;"
        "  border-bottom-right-radius: 24px;"
        "}"
    );
    card_panel->setFixedHeight(1250);

    auto* card_lay = new QVBoxLayout(card_panel);
    card_lay->setContentsMargins(20, 14, 20, 16);
    card_lay->setSpacing(8);

    // Drag handle indicator
    auto* handle = new QFrame();
    handle->setFixedSize(44, 5);
    handle->setStyleSheet("background: rgba(255,255,255,70); border-radius: 2px;");
    auto* handle_row = new QHBoxLayout();
    handle_row->addStretch();
    handle_row->addWidget(handle);
    handle_row->addStretch();
    card_lay->addLayout(handle_row);

    auto* hdr_row = new QHBoxLayout();
    hdr_row->setContentsMargins(0, 0, 0, 0);
    auto* header_lbl = new QLabel("Notifications");
    header_lbl->setStyleSheet("color: rgba(255,255,255,200); font-size: 14px; font-weight: bold; padding: 4px 0;");
    auto* btn_clear = new QPushButton("Clear All");
    btn_clear->setStyleSheet(
        "QPushButton { background: transparent; border: none; color: rgba(255,255,255,120);"
        " font-size: 12px; padding: 0; min-height: 0; }"
        "QPushButton:pressed { color: #ffffff; }");
    connect(btn_clear, &QPushButton::clicked, this, &NotificationPanel::clear_all);
    hdr_row->addWidget(header_lbl, 1);
    hdr_row->addWidget(btn_clear);
    card_lay->addLayout(hdr_row);

    // Scrollable notification list
    auto* scroll = new QScrollArea();
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { background: transparent; } QWidget { background: transparent; }");

    auto* list_container = new QWidget();
    list_container->setStyleSheet("background: transparent;");
    notif_list_ = new QVBoxLayout(list_container);
    notif_list_->setContentsMargins(0, 0, 0, 0);
    notif_list_->setSpacing(6);
    notif_list_->addStretch();
    scroll->setWidget(list_container);
    card_lay->addWidget(scroll, 1);

    root->addWidget(card_panel);
    // Transparent remainder — absorbs touch to prevent pass-through to pages below
    root->addStretch(1);

    // Start hidden above screen
    move(0, -height());
}

void NotificationPanel::set_backdrop_alpha(int alpha_0_255) {
    backdrop_alpha_ = std::clamp(alpha_0_255, 0, 255);
    update();
}

void NotificationPanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // Full-screen dark backdrop (simulated blur via opacity)
    p.fillRect(rect(), QColor(12, 12, 22, backdrop_alpha_));
}

void NotificationPanel::mousePressEvent(QMouseEvent* event) {
    swipe_start_y_ = event->pos().y();
    event->accept();
}

void NotificationPanel::mouseReleaseEvent(QMouseEvent* event) {
    int dy = event->pos().y() - swipe_start_y_;
    // Upward swipe (dy < -192px) → collapse with fast-start kick
    if (dy < -192) {
        kick(-12.0);
        set_expanded(false);
    }
    event->accept();
}

// Lightweight clickable QFrame — no Q_OBJECT needed (no new signals)
class ClickableFrame : public QFrame {
public:
    using QFrame::QFrame;
    std::function<void()> on_clicked;
protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && on_clicked) on_clicked();
        else QFrame::mousePressEvent(e);
    }
};

void NotificationPanel::push_notification(const QString& title, const QString& body) {
    auto* card = new QFrame();
    card->setStyleSheet(
        "QFrame { background: rgba(255,255,255,14); border-radius: 14px; }");
    auto* outer = new QVBoxLayout(card);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // ── Clickable header row ──────────────────────────────────────
    auto* hdr = new ClickableFrame(card);
    hdr->setCursor(Qt::PointingHandCursor);
    hdr->setStyleSheet("QFrame { background: transparent; border-radius: 14px; }");
    auto* hdr_lay = new QHBoxLayout(hdr);
    hdr_lay->setContentsMargins(10, 8, 10, 6);
    hdr_lay->setSpacing(6);

    auto* arrow = new QLabel("▶");
    arrow->setStyleSheet("color: rgba(255,255,255,120); font-size: 10px;");
    arrow->setFixedWidth(12);

    auto* t = new QLabel(title);
    t->setStyleSheet("color: #ffffff; font-size: 13px; font-weight: bold;");

    auto* ts = new QLabel(QTime::currentTime().toString("HH:mm"));
    ts->setStyleSheet("color: rgba(255,255,255,70); font-size: 11px;");

    auto* btn_x = new QPushButton("×");
    btn_x->setFixedSize(22, 22);
    btn_x->setStyleSheet(
        "QPushButton { background: transparent; border: none; color: rgba(255,255,255,120);"
        " font-size: 16px; font-weight: bold; padding: 0; min-height: 0; }"
        "QPushButton:pressed { color: #ffffff; }");

    hdr_lay->addWidget(arrow);
    hdr_lay->addWidget(t, 1);
    hdr_lay->addWidget(ts);
    hdr_lay->addWidget(btn_x);
    outer->addWidget(hdr);

    // ── Detail area (hidden by default) ──────────────────────────
    auto* detail = new QWidget(card);
    detail->hide();
    auto* det_lay = new QVBoxLayout(detail);
    det_lay->setContentsMargins(32, 2, 10, 8);
    det_lay->setSpacing(0);
    auto* b = new QLabel(body);
    b->setStyleSheet("color: rgba(255,255,255,160); font-size: 12px;");
    b->setWordWrap(true);
    det_lay->addWidget(b);
    outer->addWidget(detail);

    // Toggle detail on header click
    hdr->on_clicked = [detail, arrow]() {
        bool show = !detail->isVisible();
        detail->setVisible(show);
        arrow->setText(show ? "▼" : "▶");
    };

    connect(btn_x, &QPushButton::clicked, card, &QFrame::deleteLater);

    notif_list_->insertWidget(notif_list_->count() - 1, card);

    if (!expanded_) ++unread_count_;
}

void NotificationPanel::clear_all() {
    // Remove every item except the trailing stretch (last item)
    while (notif_list_->count() > 1) {
        auto* item = notif_list_->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    unread_count_ = 0;
}

void NotificationPanel::set_expanded(bool expanded) {
    expanded_ = expanded;
    if (expanded_) unread_count_ = 0;
}

void NotificationPanel::advance_spring() {
    // k=0.20 + damp=0.55: ease-out feel (fast start → rapid deceleration, <1/4 original overshoot)
    constexpr double k    = 0.20;
    constexpr double damp = 0.55;
    double target = expanded_ ? 0.0 : -(double)height();
    double force  = k * (target - pos_y_);
    vel_y_ = (vel_y_ + force) * (1.0 - damp);
    pos_y_ += vel_y_;
    move(0, (int)pos_y_);
}

void NotificationPanel::kick(double initial_vel) {
    vel_y_ = initial_vel;
}

bool NotificationPanel::is_settled() const {
    return std::abs(vel_y_) < 0.5 && std::abs(pos_y_ - (expanded_ ? 0.0 : -(double)height())) < 1.0;
}

// ═════════════════════════════════════════════════════════════
// RealTimePlot (retain Phase 3 physics engine)
// ═════════════════════════════════════════════════════════════
RealTimePlot::RealTimePlot(QWidget* parent) : QWidget(parent) {
    setMinimumSize(200, 300);
    shift_buffer.fill(0.0);
}

void RealTimePlot::add_sample(double val) {
    shift_buffer[shift_head] = val;
    shift_head = (shift_head + 1) % SHIFT_BUFFER_SIZE;
}

void RealTimePlot::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QLinearGradient bg(0, 0, 0, height());
    bg.setColorAt(0, QColor(30, 30, 50));
    bg.setColorAt(1, QColor(20, 20, 35));
    p.fillRect(rect(), bg);

    // Layout: left margin reserved for Y-axis labels
    const int ml = 50, mt = 6, mb = 6;
    const int pw = width() - ml;
    const int ph = height() - mt - mb;

    double y_max = fixed_max_ > 0.0 ? fixed_max_ : (current_max + 1.0);
    bool use_k   = (y_max >= 10000.0);

    // Y-axis grid + labels at 0, 0.2, 0.4, 0.6, 0.8, 1.0
    QFont lf = p.font();
    lf.setPixelSize(11);
    p.setFont(lf);
    for (int i = 0; i <= 5; ++i) {
        double ratio = i / 5.0;
        int iy = mt + ph - static_cast<int>(ratio * ph);
        p.setPen(QPen(QColor(255, 255, 255, 18), 1, Qt::DashLine));
        p.drawLine(ml, iy, width(), iy);
        QString lbl = (i == 0) ? "0"
                    : use_k    ? QString("%1K").arg(static_cast<int>(y_max * ratio / 1000.0))
                               : QString::number(static_cast<int>(y_max * ratio));
        p.setPen(QColor(130, 130, 150));
        p.drawText(QRect(0, iy - 8, ml - 4, 16), Qt::AlignRight | Qt::AlignVCenter, lbl);
    }

    // Plot line (oldest→newest left→right)
    QPainterPath path;
    double x_step = static_cast<double>(pw) / (SHIFT_BUFFER_SIZE - 1);
    for (int i = 0; i < SHIFT_BUFFER_SIZE; ++i) {
        double sample = shift_buffer[(shift_head + i) % SHIFT_BUFFER_SIZE];
        double norm = std::min(sample / y_max, 1.0);
        double x = ml + i * x_step;
        double y = mt + ph - norm * ph;
        if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
    }
    p.setPen(QPen(QColor(0, 150, 255, 40), 8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(path);
    p.setPen(QPen(QColor(200, 240, 255), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(path);
}

// ═════════════════════════════════════════════════════════════
// OverviewPage: overview page
// ═════════════════════════════════════════════════════════════
OverviewPage::OverviewPage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);

    auto* title = new QLabel("System Overview");
    title->setObjectName("section_title");
    layout->addWidget(title);

    // Mode label
    auto* info_row = new QHBoxLayout();
    lbl_mode = new QLabel("Mode: Acceleration");
    lbl_mode->setStyleSheet("color: #00cc66; font-weight: bold; font-size: 15px;");
    info_row->addWidget(lbl_mode);
    info_row->addStretch();
    layout->addLayout(info_row);

    // Dual chart row
    auto* plot_row = new QHBoxLayout();
    auto* pps_group = new QGroupBox("Packet rate (PPS)");
    auto* pps_lay = new QVBoxLayout(pps_group);
    pps_plot = new RealTimePlot();
    pps_plot->set_fixed_max(100000.0);
    pps_lay->addWidget(pps_plot);
    plot_row->addWidget(pps_group);

    auto* bps_group = new QGroupBox("Bandwidth (Mbps)");
    auto* bps_lay = new QVBoxLayout(bps_group);
    bps_plot = new RealTimePlot();
    bps_plot->set_fixed_max(1000.0);
    bps_lay->addWidget(bps_plot);
    plot_row->addWidget(bps_group);
    layout->addLayout(plot_row);

    // 4-core CPU load row
    auto* cores_row = new QHBoxLayout();
    for (int i = 0; i < 4; ++i) {
        core_labels[i] = new QLabel(QString("Core %1\n0%").arg(i));
        core_labels[i]->setStyleSheet("background-color: #22223a; border: 1px solid #2a2a4a; border-radius: 6px; padding: 10px; font-size: 15px;");
        core_labels[i]->setAlignment(Qt::AlignCenter);
        cores_row->addWidget(core_labels[i]);
    }
    layout->addLayout(cores_row);

    // ── System info (merged from SystemPage) ──
    auto* sys_title = new QLabel("System Info");
    sys_title->setObjectName("section_title");
    layout->addWidget(sys_title);

    auto* info_group = new QGroupBox("Hardware & Runtime");
    auto* info_form = new QFormLayout(info_group);
    info_form->setSpacing(8);
    info_form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    lbl_hostname = new QLabel("--");
    lbl_kernel   = new QLabel("--");
    lbl_cpu_temp = new QLabel("--");
    lbl_uptime   = new QLabel("--");
    lbl_memory   = new QLabel("--");
    for (QLabel* l : {lbl_hostname, lbl_kernel, lbl_uptime, lbl_memory})
        l->setWordWrap(true);
    info_form->addRow("Hostname:", lbl_hostname);
    info_form->addRow("Kernel:",   lbl_kernel);
    info_form->addRow("CPU Temp:", lbl_cpu_temp);
    info_form->addRow("Uptime:",   lbl_uptime);
    info_form->addRow("Memory:",   lbl_memory);
    layout->addWidget(info_group);


    layout->addStretch();
}

void OverviewPage::refresh(const Telemetry& tel, const std::array<uint64_t, 4>& last_p, const std::array<uint64_t, 4>& last_b) {
    double total_pps = 0, total_bps = 0;
    for (int i = 0; i < 4; ++i) {
        uint64_t cp = tel.core_metrics[i].pkts.load(std::memory_order_relaxed);
        uint64_t cb = tel.core_metrics[i].bytes.load(std::memory_order_relaxed);
        uint64_t dp = cp - last_p[i], db = cb - last_b[i];
        total_pps += dp * 30.0;
        total_bps += db * 30.0 * 8.0 / 1e6;  // bytes/tick → Mbps
        int load_pct = tel.core_metrics[i].cpu_load_pct.load(std::memory_order_relaxed);
        // Colour: green <50%, orange 50-80%, red >80%
        const char* colour = load_pct < 50 ? "#00cc66" : (load_pct < 80 ? "#ffaa00" : "#ff4444");
        core_labels[i]->setText(QString("Core %1\n%2%").arg(i).arg(load_pct));
        core_labels[i]->setStyleSheet(
            QString("background-color: #22223a; border: 1px solid #2a2a4a; border-radius: 6px;"
                    " padding: 10px; font-size: 15px; color: %1;").arg(colour));
    }

    pps_plot->add_sample(total_pps);
    bps_plot->add_sample(total_bps);
    pps_plot->update();
    bps_plot->update();

    bool bridge = tel.bridge_mode.load(std::memory_order_relaxed);
    lbl_mode->setText(bridge ? "Mode: Bridge" : "Mode: Acceleration");
    lbl_mode->setStyleSheet(bridge
        ? "color: #ffaa00; font-weight: bold; font-size: 15px;"
        : "color: #00cc66; font-weight: bold; font-size: 15px;");
}

void OverviewPage::sync_bridge_mode_from_config() {
    bool accel = Config::ENABLE_ACCELERATION.load(std::memory_order_relaxed);
    Telemetry::instance().bridge_mode.store(!accel, std::memory_order_relaxed);
    lbl_mode->setText(accel ? "Mode: Acceleration" : "Mode: Bridge");
    lbl_mode->setStyleSheet(accel
        ? "color: #00cc66; font-weight: bold; font-size: 15px;"
        : "color: #ffaa00; font-weight: bold; font-size: 15px;");
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

    // Scrollable card list
    auto* cards_container = new QWidget();
    iface_cards_layout_ = new QVBoxLayout(cards_container);
    iface_cards_layout_->setSpacing(10);
    iface_cards_layout_->setContentsMargins(0, 4, 0, 4);
    iface_cards_layout_->addStretch();
    layout->addWidget(cards_container);

    scan_interfaces();

    // Advanced options
    auto* adv_group = new QGroupBox("Advanced Options");
    auto* adv_form = new QFormLayout(adv_group);
    sw_stp = new SwitchToggle();
    sw_stp->setChecked(Config::ENABLE_STP.load(std::memory_order_relaxed));
    adv_form->addRow("Spanning Tree Protocol:", sw_stp);
    sw_igmp = new SwitchToggle();
    sw_igmp->setChecked(Config::ENABLE_IGMP_SNOOPING.load(std::memory_order_relaxed));
    adv_form->addRow("IGMP Snooping:", sw_igmp);
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
    int done_fd = Telemetry::instance().sys_info.done_notifier_fd();
    if (done_fd >= 0) {
        scan_done_notifier_ = new QSocketNotifier(done_fd, QSocketNotifier::Read, this);
        connect(scan_done_notifier_, &QSocketNotifier::activated, this, &InterfacePage::on_scan_done);
    }
}

void InterfacePage::scan_interfaces() {
    // Clear existing cards (keep trailing stretch)
    role_entries_count_ = 0;
    while (iface_cards_layout_->count() > 1) {
        auto* item = iface_cards_layout_->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    auto& si = Telemetry::instance().sys_info;
    uint8_t cnt = si.iface_count.load(std::memory_order_acquire);

    using IfacePair = std::pair<std::string, std::string>;
    std::array<IfacePair, Telemetry::SystemInfo::MAX_IFACES> buf;
    uint8_t valid = 0;
    if (cnt > 0) {
        for (uint8_t i = 0; i < cnt; ++i)
            buf[valid++] = { si.ifaces[i].name.data(), si.ifaces[i].operstate.data() };
    } else {
        buf[0] = {"eth0","unknown"}; buf[1] = {"eth1","unknown"};
        buf[2] = {"eth2","unknown"}; buf[3] = {"eth3","unknown"};
        valid = 4;
    }
    std::sort(buf.begin(), buf.begin() + valid,
        [](const IfacePair& a, const IfacePair& b){ return a.first < b.first; });

    for (uint8_t i = 0; i < valid; ++i) {
        const auto& [name, state] = buf[i];

        Config::IfaceRole role = Config::find_role(name);
        if (role == Config::IfaceRole::DISABLED) {
            if (name == "eth0") role = Config::IfaceRole::GATEWAY;
            else if (name == "eth1") role = Config::IfaceRole::LAN;
        }

        // ── Card ──
        auto* card = new QFrame();
        card->setStyleSheet("QFrame { background:#1e1e3a; border:1px solid #2a2a4a; border-radius:10px; }");
        auto* cl = new QVBoxLayout(card);
        cl->setSpacing(10);
        cl->setContentsMargins(16, 12, 16, 12);

        // Header: name + link state
        auto* hdr = new QHBoxLayout();
        auto* lbl_name = new QLabel(QString::fromStdString(name));
        lbl_name->setStyleSheet("font-size:17px; font-weight:bold; color:#ffffff;");
        bool is_up = (state == "up");
        auto* lbl_state = new QLabel(is_up ? "● Connected" : "○ Down");
        lbl_state->setStyleSheet(is_up ? "color:#00cc66; font-weight:bold;" : "color:#888888;");
        hdr->addWidget(lbl_name);
        hdr->addStretch();
        hdr->addWidget(lbl_state);
        cl->addLayout(hdr);

        // Role buttons (4-way segmented)
        auto* btn_group = new QButtonGroup(card);
        btn_group->setExclusive(true);
        auto* role_row = new QHBoxLayout();
        role_row->setSpacing(6);
        const char* role_labels[] = {"WAN", "LAN", "Gateway", "Disabled"};
        for (int r = 0; r < 4; ++r) {
            auto* btn = new QPushButton(role_labels[r]);
            btn->setObjectName("role_btn");
            btn->setCheckable(true);
            btn->setChecked(static_cast<int>(role) == r);
            btn_group->addButton(btn, r);
            role_row->addWidget(btn);
        }
        cl->addLayout(role_row);

        // Store entry
        if (role_entries_count_ < role_entries_.size()) {
            auto nn = std::string_view{name}.copy(
                role_entries_[role_entries_count_].name.data(), 15);
            role_entries_[role_entries_count_].name[nn] = '\0';
            role_entries_[role_entries_count_].group = btn_group;
            ++role_entries_count_;
        }

        connect(btn_group, &QButtonGroup::idClicked, this, [this, name](int id) {
            on_role_changed(QString::fromStdString(name), id);
        });

        iface_cards_layout_->insertWidget(iface_cards_layout_->count() - 1, card);
    }
}

void InterfacePage::on_role_changed(const QString& iface, int index) {
    if (index != 2) return;
    std::string iface_str = iface.toStdString();
    for (size_t i = 0; i < role_entries_count_; ++i) {
        if (iface_str != role_entries_[i].name.data() && role_entries_[i].group->checkedId() == 2)
            role_entries_[i].group->button(0)->setChecked(true); // demote to WAN
    }
}

void InterfacePage::refresh_from_backend() {
    scan_interfaces();
    {
        QSignalBlocker b1(*sw_stp);
        QSignalBlocker b2(*sw_igmp);
        sw_stp->setChecked(Config::ENABLE_STP.load(std::memory_order_relaxed));
        sw_igmp->setChecked(Config::ENABLE_IGMP_SNOOPING.load(std::memory_order_relaxed));
    }
}

void InterfacePage::on_save_clicked() {
    int gw_count = 0;
    std::string gw_iface;
    for (size_t i = 0; i < role_entries_count_; ++i) {
        if (role_entries_[i].group->checkedId() == 2) { ++gw_count; gw_iface = role_entries_[i].name.data(); }
    }
    if (gw_count == 0) {
        QMessageBox::warning(this, "Config Error", "No Default Gateway assigned.");
        return;
    }

    Config::clear_roles();
    for (size_t i = 0; i < role_entries_count_; ++i)
        Config::set_role(role_entries_[i].name.data(),
            static_cast<Config::IfaceRole>(role_entries_[i].group->checkedId()));

    Config::IFACE_GATEWAY = gw_iface;
    Config::IFACE_WAN     = gw_iface;
    Config::clear_bridged();
    for (size_t i = 0; i < role_entries_count_; ++i)
        if (role_entries_[i].group->checkedId() == 1) Config::add_bridged(role_entries_[i].name.data());
    Config::IFACE_LAN = Config::BRIDGED_IFACES_COUNT == 0 ? "" : std::string(Config::BRIDGED_INTERFACES[0].name.data());

    Config::ENABLE_STP.store(sw_stp->isChecked(), std::memory_order_relaxed);
    Config::ENABLE_IGMP_SNOOPING.store(sw_igmp->isChecked(), std::memory_order_relaxed);
    Config::ENABLE_ACCELERATION.store(true, std::memory_order_relaxed);
    Telemetry::instance().bridge_mode.store(false, std::memory_order_relaxed);

    std::println("[GUI] Interface roles saved. Gateway: {}, LAN interfaces: {}",
        Config::IFACE_GATEWAY, Config::BRIDGED_IFACES_COUNT);
}

void InterfacePage::on_reset_clicked() {
    scan_interfaces(); // Rebuilds table from Config::IFACE_ROLES
    sw_stp->setChecked(Config::ENABLE_STP.load(std::memory_order_relaxed));
    sw_igmp->setChecked(Config::ENABLE_IGMP_SNOOPING.load(std::memory_order_relaxed));
}

void InterfacePage::on_refresh_clicked() {
    Telemetry::instance().sys_info.request_rescan();
    btn_refresh->setEnabled(false);
    btn_refresh->setText("Scanning…");
}

void InterfacePage::on_scan_done() {
    // Drain the eventfd counter before reading Telemetry cache
    Telemetry::instance().sys_info.consume_done();
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
    {
        auto* accel_row = new QHBoxLayout();
        auto* lbl_accel = new QLabel("Enable Gaming Traffic Acceleration (Heuristic Priority Scheduling)");
        lbl_accel->setWordWrap(true);
        lbl_accel->setStyleSheet("font-size: 15px; color: #e0e0e0;");
        sw_acceleration = new SwitchToggle();
        sw_acceleration->setChecked(Config::ENABLE_ACCELERATION.load(std::memory_order_relaxed));
        connect(sw_acceleration, &SwitchToggle::toggled, this, &QosPage::on_toggle_accel);
        accel_row->addWidget(lbl_accel, 1);
        accel_row->addWidget(sw_acceleration, 0, Qt::AlignVCenter);
        layout->addLayout(accel_row);
    }

    // Bandwidth limit
    auto* bw_group = new QGroupBox("Global Bandwidth Limits");
    auto* bw_form = new QFormLayout(bw_group);
    edit_dl_limit = new QLineEdit("500");
    edit_ul_limit = new QLineEdit("50");
    edit_dl_limit->setReadOnly(true);
    edit_ul_limit->setReadOnly(true);
    {
        auto attach_bw_validator = [](QLineEdit* le) {
            auto* v = new QDoubleValidator(k_qos_bw_min_mbps, k_qos_bw_max_mbps, 12, le);
            v->setNotation(QDoubleValidator::StandardNotation);
            v->setLocale(QLocale::c());
            le->setValidator(v);
        };
        attach_bw_validator(edit_dl_limit);
        attach_bw_validator(edit_ul_limit);
    }
    edit_dl_limit->installEventFilter(this);
    edit_ul_limit->installEventFilter(this);
    bw_form->addRow("Download Limit (Mbps):", edit_dl_limit);
    bw_form->addRow("Upload Limit (Mbps):", edit_ul_limit);
    {
        auto* apply_row = new QHBoxLayout();
        apply_row->addStretch();
        auto* btn_apply_bw = new QPushButton("Apply");
        btn_apply_bw->setObjectName("btn_primary");
        btn_apply_bw->setFixedHeight(40);
        connect(btn_apply_bw, &QPushButton::clicked, this, &QosPage::on_apply_global_bw);
        apply_row->addWidget(btn_apply_bw);
        bw_form->addRow(apply_row);
    }
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

    // Game port whitelist (backed by Config — edit via dialog, watchdog applies)
    auto* wl_group = new QGroupBox("Game Port Whitelist");
    auto* wl_lay = new QVBoxLayout(wl_group);

    whitelist_table = new QTableWidget(0, 3);
    whitelist_table->setHorizontalHeaderLabels({"Port / Range", "Protocol", "Description"});
    whitelist_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    whitelist_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    whitelist_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    whitelist_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    whitelist_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    whitelist_table->verticalHeader()->setVisible(false);
    const int wl_row_h = qRound(whitelist_table->fontMetrics().height() * 1.2);
    whitelist_table->verticalHeader()->setDefaultSectionSize(wl_row_h);
    whitelist_table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    wl_lay->addWidget(whitelist_table);

    refresh_whitelist_from_config();

    auto* wl_btn_row = new QHBoxLayout();
    auto* btn_edit_wl = new QPushButton("Edit Whitelist…");
    btn_edit_wl->setObjectName("btn_primary");
    wl_btn_row->addWidget(btn_edit_wl);
    wl_btn_row->addStretch();
    wl_lay->addLayout(wl_btn_row);
    layout->addWidget(wl_group);
    layout->addStretch();

    connect(btn_edit_wl, &QPushButton::clicked, this, &QosPage::on_edit_whitelist);
}

void QosPage::refresh_whitelist_from_config() {
    const int wl_row_h = qRound(whitelist_table->fontMetrics().height() * 1.2);
    whitelist_table->verticalHeader()->setDefaultSectionSize(wl_row_h);
    whitelist_table->setRowCount(0);

    auto make_item_qs = [](const QString& text) {
        auto* it = new QTableWidgetItem(text);
        it->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        return it;
    };
    std::array<Config::PortRange, Config::MAX_GAME_PORT_RANGES> wl_buf{};
    const size_t wl_n = Config::copy_active_game_ports_for_display(wl_buf.data(), wl_buf.size());
    for (size_t i = 0; i < wl_n; ++i) {
        const auto& pr = wl_buf[i];
        const QString port_str = (pr.start == pr.end)
            ? QString::number(pr.start)
            : QStringLiteral("%1-%2").arg(pr.start).arg(pr.end);
        const int r = whitelist_table->rowCount();
        whitelist_table->insertRow(r);
        whitelist_table->setItem(r, 0, make_item_qs(port_str));
        whitelist_table->setItem(r, 1, make_item_qs(QStringLiteral("TCP/UDP")));
        whitelist_table->setItem(r, 2, make_item_qs(QString::fromUtf8(pr.desc)));
    }
    const int header_h = whitelist_table->horizontalHeader()->height();
    const int rows_h   = whitelist_table->rowCount() * wl_row_h;
    whitelist_table->setMinimumHeight(header_h + rows_h + 4);
}

bool QosPage::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        if (watched == edit_dl_limit) {
            bool ok = false;
            double cur = edit_dl_limit->text().trimmed().toDouble(&ok);
            if (!ok) cur = 500.0;
            cur = std::clamp(cur, k_qos_bw_min_mbps, k_qos_bw_max_mbps);
            if (auto v = NumPadDialog::get_double(this, QStringLiteral("Download Limit (Mbps)"),
                                                  cur, k_qos_bw_min_mbps, k_qos_bw_max_mbps))
                edit_dl_limit->setText(QString::number(*v, 'g', 12));
            return true;
        }
        if (watched == edit_ul_limit) {
            bool ok = false;
            double cur = edit_ul_limit->text().trimmed().toDouble(&ok);
            if (!ok) cur = 50.0;
            cur = std::clamp(cur, k_qos_bw_min_mbps, k_qos_bw_max_mbps);
            if (auto v = NumPadDialog::get_double(this, QStringLiteral("Upload Limit (Mbps)"),
                                                  cur, k_qos_bw_min_mbps, k_qos_bw_max_mbps))
                edit_ul_limit->setText(QString::number(*v, 'g', 12));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QosPage::on_apply_global_bw() {
    if (!edit_dl_limit->hasAcceptableInput() || !edit_ul_limit->hasAcceptableInput()) {
        Dashboard::post_notification(
            QStringLiteral("Invalid limits"),
            QStringLiteral("Use the keypad: each value must be greater than 0 and at most %1 Mbps.")
                .arg(k_qos_bw_max_mbps, 0, 'g', 6));
        return;
    }
    bool ok_dl = false, ok_ul = false;
    double dl = edit_dl_limit->text().trimmed().toDouble(&ok_dl);
    double ul = edit_ul_limit->text().trimmed().toDouble(&ok_ul);
    if (!ok_dl || !ok_ul || dl <= 0.0 || ul <= 0.0
        || dl < k_qos_bw_min_mbps || ul < k_qos_bw_min_mbps
        || dl > k_qos_bw_max_mbps || ul > k_qos_bw_max_mbps) {
        Dashboard::post_notification(
            QStringLiteral("Invalid limits"),
            QStringLiteral("Enter download and upload between %1 and %2 Mbps.")
                .arg(k_qos_bw_min_mbps, 0, 'g', 6)
                .arg(k_qos_bw_max_mbps, 0, 'g', 6));
        return;
    }
    auto& tel = Telemetry::instance();
    tel.qos_global_dl_mbps_pending.store(dl, std::memory_order_relaxed);
    tel.qos_global_ul_mbps_pending.store(ul, std::memory_order_relaxed);
    tel.qos_global_bw_dirty.store(true, std::memory_order_release);
    Dashboard::post_notification(
        QStringLiteral("QoS"),
        QStringLiteral("Parameters have been applied successfully."));
}

void QosPage::on_toggle_accel() {
    bool on = sw_acceleration->isChecked();
    Config::ENABLE_ACCELERATION.store(on, std::memory_order_relaxed);
    Telemetry::instance().bridge_mode.store(!on, std::memory_order_relaxed);
    std::println("[GUI] Acceleration mode: {}", on ? "ON" : "OFF");
}

// ═════════════════════════════════════════════════════════════
// NumPadDialog
// ═════════════════════════════════════════════════════════════
namespace {
QString numpad_format_initial_double(double v) {
    return QString::number(v, 'g', 12);
}
} // namespace

NumPadDialog::NumPadDialog(const QString& title, QString initial_text,
                           double min_val, double max_val,
                           bool allow_negative, bool allow_decimal,
                           QWidget* parent)
    : QDialog(parent)
    , min_(min_val)
    , max_(max_val)
    , allow_negative_(allow_negative)
    , allow_decimal_(allow_decimal)
{
    btn_minus_ = nullptr;
    btn_dot_   = nullptr;
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setStyleSheet(HPGTP::GUI::DARK_STYLESHEET);
    setFixedWidth(540);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(30, 30, 30, 30);
    lay->setSpacing(15);

    auto* lbl_title = new QLabel(title);
    lbl_title->setAlignment(Qt::AlignCenter);
    lbl_title->setWordWrap(true);
    lbl_title->setStyleSheet("font-size:24px; font-weight:bold; color:#ffffff; padding-bottom:6px;");
    lay->addWidget(lbl_title);

    display_ = new QLabel();
    display_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    display_->setFixedHeight(87);
    lay->addWidget(display_);

    auto* grid = new QGridLayout();
    grid->setSpacing(12);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(2, 1);

    auto make_btn = [&](const QString& t, int row, int col, int rs = 1, int cs = 1) -> QPushButton* {
        auto* b = new QPushButton(t);
        b->setFixedHeight(84);
        b->setStyleSheet("font-size:33px; font-weight:bold;");
        grid->addWidget(b, row, col, rs, cs);
        return b;
    };

    auto* b7 = make_btn("7", 0, 0);
    auto* b8 = make_btn("8", 0, 1);
    auto* b9 = make_btn("9", 0, 2);
    auto* b4 = make_btn("4", 1, 0);
    auto* b5 = make_btn("5", 1, 1);
    auto* b6 = make_btn("6", 1, 2);
    auto* b1 = make_btn("1", 2, 0);
    auto* b2 = make_btn("2", 2, 1);
    auto* b3 = make_btn("3", 2, 2);

    QPushButton* b0 = nullptr;
    QPushButton* del = nullptr;

    if (allow_negative_ && !allow_decimal_) {
        b0         = make_btn("0", 3, 0);
        btn_minus_ = make_btn(QStringLiteral("\u2212"), 3, 1);
        del        = make_btn(QStringLiteral("\u232B"), 3, 2);
    } else if (!allow_negative_ && allow_decimal_) {
        b0       = make_btn("0", 3, 0);
        btn_dot_ = make_btn(QStringLiteral("."), 3, 1);
        del      = make_btn(QStringLiteral("\u232B"), 3, 2);
    } else {
        b0 = make_btn("0", 3, 0);
        auto* row4_pad = new QWidget();
        row4_pad->setFixedHeight(84);
        row4_pad->setAttribute(Qt::WA_TransparentForMouseEvents);
        grid->addWidget(row4_pad, 3, 1);
        del = make_btn(QStringLiteral("\u232B"), 3, 2);
    }
    del->setObjectName("btn_danger");

    lay->addLayout(grid);

    auto* btn_row = new QHBoxLayout();
    btn_row->setSpacing(15);
    auto* cancel = new QPushButton("Cancel");
    btn_ok_ = new QPushButton("OK");
    btn_ok_->setObjectName("btn_primary");
    {
        QFont bf;
        bf.setBold(true);
        bf.setPixelSize(23);
        cancel->setFont(bf);
        btn_ok_->setFont(bf);
        cancel->setFixedHeight(84);
        btn_ok_->setFixedHeight(84);
    }
    btn_row->addWidget(cancel, 1);
    btn_row->addWidget(btn_ok_, 1);
    lay->addLayout(btn_row);

    text_ = std::move(initial_text);

    for (auto [btn, ch] : std::initializer_list<std::pair<QPushButton*, char>>{
             {b0, '0'}, {b1, '1'}, {b2, '2'}, {b3, '3'}, {b4, '4'},
             {b5, '5'}, {b6, '6'}, {b7, '7'}, {b8, '8'}, {b9, '9'}}) {
        if (btn)
            connect(btn, &QPushButton::clicked, this, [this, ch]() { push_digit(ch); });
    }
    if (btn_minus_)
        connect(btn_minus_, &QPushButton::clicked, this, &NumPadDialog::on_minus);
    if (btn_dot_)
        connect(btn_dot_, &QPushButton::clicked, this, &NumPadDialog::on_dot);

    connect(del, &QPushButton::clicked, this, &NumPadDialog::do_backspace);
    connect(btn_ok_, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    update_display();
}

void NumPadDialog::push_digit(char d) {
    if (text_.length() >= 20) return;
    text_ += QChar(d);
    update_display();
}

void NumPadDialog::on_minus() {
    if (!allow_negative_) return;
    if (!text_.isEmpty()) return;
    text_ = '-';
    update_display();
}

void NumPadDialog::on_dot() {
    if (!allow_decimal_) return;
    if (text_.contains('.')) return;
    if (text_.isEmpty() || text_ == '-')
        text_ += QStringLiteral("0.");
    else
        text_ += '.';
    update_display();
}

void NumPadDialog::do_backspace() {
    if (!text_.isEmpty()) text_.chop(1);
    update_display();
}

void NumPadDialog::update_display() {
    display_->setText(text_);

    bool ok_btn = false;
    bool red    = false;

    if (text_.isEmpty()) {
    } else if (text_ == u'-' || text_ == u'.' || text_ == QStringLiteral("-.")) {
    } else {
        bool ok_parse = false;
        double val = text_.toDouble(&ok_parse);
        if (!allow_decimal_ && text_.contains('.'))
            ok_parse = false;
        if (!allow_negative_ && (val < 0 || text_.startsWith('-')))
            ok_parse = false;

        if (ok_parse && !allow_decimal_) {
            if (std::trunc(val) != val) ok_parse = false;
        }
        if (ok_parse) {
            if (val >= min_ && val <= max_)
                ok_btn = true;
            else
                red = true;
        } else {
            red = true;
        }
    }

    display_->setStyleSheet(
        QString("background-color:#22223a; border:3px solid %1; border-radius:12px;"
                "font-size:39px; font-weight:bold; color:%2; padding:0 18px;")
            .arg(red ? "#cc3333" : "#3a3a5a")
            .arg(red ? "#ff6666" : "#ffffff"));
    btn_ok_->setEnabled(ok_btn);
}

std::optional<int> NumPadDialog::get_int(QWidget* parent, const QString& title,
                                          int initial, int min, int max)
{
    initial = std::clamp(initial, min, max);
    NumPadDialog dlg(title, QString::number(initial), static_cast<double>(min),
                     static_cast<double>(max), false, false, parent);
    if (dlg.exec() != QDialog::Accepted || dlg.text_.isEmpty()) return std::nullopt;
    bool ok = false;
    double dv = dlg.text_.toDouble(&ok);
    if (!ok) return std::nullopt;
    int v = static_cast<int>(std::llround(dv));
    if (std::fabs(dv - static_cast<double>(v)) > 1e-9) return std::nullopt;
    if (v < min || v > max) return std::nullopt;
    return v;
}

std::optional<double> NumPadDialog::get_double(QWidget* parent, const QString& title,
                                                 double initial, double min, double max)
{
    initial = std::clamp(initial, min, max);
    NumPadDialog dlg(title, numpad_format_initial_double(initial), min, max, false, true, parent);
    if (dlg.exec() != QDialog::Accepted || dlg.text_.isEmpty()) return std::nullopt;
    bool ok = false;
    double v = dlg.text_.toDouble(&ok);
    if (!ok || v <= 0.0 || v < min || v > max) return std::nullopt;
    return v;
}

// PortWhitelistDialog: full-edit modal for game port whitelist
// ═════════════════════════════════════════════════════════════
PortWhitelistDialog::PortWhitelistDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Edit Game Port Whitelist");
    setMinimumSize(720, 900);
    setStyleSheet(HPGTP::GUI::DARK_STYLESHEET);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(12);

    auto* desc = new QLabel(
        "Ports listed here receive highest scheduling priority.\n"
        "Use the keypad: port 1–65535, or range start–end (start ≤ end).");
    desc->setStyleSheet("color: #808090; font-size: 13px;");
    desc->setWordWrap(true);
    lay->addWidget(desc);

    table_ = new QTableWidget(0, 3);
    table_->setHorizontalHeaderLabels({"Port / Range", "Protocol", "Description"});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(qRound(table_->fontMetrics().height() * 1.2));
    lay->addWidget(table_, 1);

    auto* btn_row = new QHBoxLayout();
    btn_row->setSpacing(12);
    auto* btn_add    = new QPushButton("+ Add Port");
    auto* btn_remove = new QPushButton("- Remove");
    auto* btn_close  = new QPushButton("Apply & Close");
    btn_add->setObjectName("btn_primary");
    btn_close->setObjectName("btn_primary");
    btn_row->addWidget(btn_add);
    btn_row->addWidget(btn_remove);
    btn_row->addStretch();
    btn_row->addWidget(btn_close);
    lay->addLayout(btn_row);

    connect(btn_add,    &QPushButton::clicked, this, &PortWhitelistDialog::on_add_port);
    connect(btn_remove, &QPushButton::clicked, this, &PortWhitelistDialog::on_remove_port);
    connect(btn_close,  &QPushButton::clicked, this, &QDialog::accept);
    connect(table_, &QTableWidget::cellDoubleClicked, this, &PortWhitelistDialog::on_cell_edit);
}

void PortWhitelistDialog::on_add_port() {
    QDialog dlg(this);
    dlg.setWindowTitle("Select Protocol");
    dlg.setFixedSize(560, 300);
    dlg.setStyleSheet(HPGTP::GUI::DARK_STYLESHEET);

    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(24, 20, 24, 20);
    lay->setSpacing(16);

    auto* lbl = new QLabel("Select protocol for the new port rule:");
    lbl->setStyleSheet("color: #c0c0d0; font-size: 15px;");
    lay->addWidget(lbl);

    auto* btn_row = new QHBoxLayout();
    btn_row->setSpacing(12);

    QString chosen;
    auto make_btn = [&](const QString& proto) {
        auto* btn = new QPushButton(proto);
        btn->setObjectName("btn_primary");
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btn->setFixedHeight(64);
        connect(btn, &QPushButton::clicked, &dlg, [&dlg, &chosen, proto]() {
            chosen = proto;
            dlg.accept();
        });
        btn_row->addWidget(btn);
    };
    make_btn("UDP");
    make_btn("TCP");
    make_btn("TCP & UDP");
    lay->addLayout(btn_row);

    auto* btn_cancel = new QPushButton("Cancel");
    btn_cancel->setFixedHeight(52);
    connect(btn_cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    lay->addWidget(btn_cancel);

    if (dlg.exec() != QDialog::Accepted) return;

    auto make_left = [](const QString& text) {
        auto* it = new QTableWidgetItem(text);
        it->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        return it;
    };

    // NumPad for port number(s)
    auto start = NumPadDialog::get_int(this, "Port Number", k_port_min, k_port_min, k_port_max);
    if (!start) return;
    auto end = NumPadDialog::get_int(this, "Range End\n(same value = single port)", *start, *start, k_port_max);
    QString port_str = (!end || *end <= *start)
                       ? QString::number(*start)
                       : QString("%1-%2").arg(*start).arg(*end);

    int row = table_->rowCount();
    table_->insertRow(row);
    table_->setItem(row, 0, make_left(port_str));
    table_->setItem(row, 1, make_left(chosen));
    table_->setItem(row, 2, make_left(""));
    table_->editItem(table_->item(row, 2));   // let user type description
}

void PortWhitelistDialog::on_cell_edit(int row, int col) {
    auto make_left = [](const QString& text) {
        auto* it = new QTableWidgetItem(text);
        it->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        return it;
    };
    if (col == 0) {
        // Port column — NumPad
        auto* item = table_->item(row, col);
        QString cur = item ? item->text() : "0";
        int cur_start = std::clamp(cur.section('-', 0, 0).toInt(), k_port_min, k_port_max);
        auto start = NumPadDialog::get_int(this, "Port Number", cur_start, k_port_min, k_port_max);
        if (!start) return;
        int cur_end = cur.contains('-')
            ? std::clamp(cur.section('-', 1, 1).toInt(), *start, k_port_max)
            : *start;
        auto end = NumPadDialog::get_int(this, "Range End\n(same value = single port)", cur_end, *start, k_port_max);
        QString port_str = (!end || *end <= *start)
                           ? QString::number(*start)
                           : QString("%1-%2").arg(*start).arg(*end);
        table_->setItem(row, col, make_left(port_str));
    } else if (col == 2) {
        // Description column — allow standard inline text edit
        table_->editItem(table_->item(row, col));
    }
}

void PortWhitelistDialog::on_remove_port() {
    int row = table_->currentRow();
    if (row >= 0) table_->removeRow(row);
}

namespace {
bool parse_whitelist_port_cell(const QString& text, Config::PortRange& out) {
    const QString t = text.trimmed();
    if (t.isEmpty()) return false;
    if (t.contains(QLatin1Char('-'))) {
        const auto parts = t.split(QLatin1Char('-'));
        if (parts.size() != 2) return false;
        bool ok1 = false, ok2 = false;
        const int a = parts[0].trimmed().toInt(&ok1);
        const int b = parts[1].trimmed().toInt(&ok2);
        if (!ok1 || !ok2 || a < k_port_min || b < k_port_min || a > k_port_max || b > k_port_max || a > b)
            return false;
        out.start = static_cast<uint16_t>(a);
        out.end   = static_cast<uint16_t>(b);
        return true;
    }
    bool ok = false;
    const int v = t.toInt(&ok);
    if (!ok || v < k_port_min || v > k_port_max) return false;
    out.start = out.end = static_cast<uint16_t>(v);
    return true;
}
} // namespace

void QosPage::on_edit_whitelist() {
    PortWhitelistDialog dlg(this);
    auto make_aligned = [](const QString& text) {
        auto* it = new QTableWidgetItem(text);
        it->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        return it;
    };
    // Pre-populate dialog from the page display table
    for (int r = 0; r < whitelist_table->rowCount(); ++r) {
        dlg.table()->insertRow(dlg.table()->rowCount());
        for (int c = 0; c < 3; ++c) {
            auto* src = whitelist_table->item(r, c);
            dlg.table()->setItem(dlg.table()->rowCount() - 1, c,
                make_aligned(src ? src->text() : ""));
        }
    }
    if (dlg.exec() != QDialog::Accepted) return;

    std::vector<Config::PortRange> ranges;
    ranges.reserve(static_cast<size_t>(dlg.table()->rowCount()));
    for (int r = 0; r < dlg.table()->rowCount(); ++r) {
        auto* cell = dlg.table()->item(r, 0);
        if (!cell) continue;
        Config::PortRange pr{};
        if (!parse_whitelist_port_cell(cell->text(), pr)) continue;
        auto* desc_cell = dlg.table()->item(r, 2);
        if (desc_cell) {
            QByteArray utf8 = desc_cell->text().toUtf8();
            std::strncpy(pr.desc, utf8.constData(), sizeof(pr.desc) - 1);
            pr.desc[sizeof(pr.desc) - 1] = '\0';
        }
        ranges.push_back(pr);
    }
    if (ranges.empty()) {
        Dashboard::post_notification(
            QStringLiteral("Whitelist"),
            QStringLiteral("Add at least one valid port or range (1–65535)."));
        return;
    }
    Config::request_game_ports_apply(ranges);

    // Sync back to page display table
    whitelist_table->setRowCount(0);
    for (int r = 0; r < dlg.table()->rowCount(); ++r) {
        int nr = whitelist_table->rowCount();
        whitelist_table->insertRow(nr);
        for (int c = 0; c < 3; ++c) {
            auto* src = dlg.table()->item(r, c);
            whitelist_table->setItem(nr, c, make_aligned(src ? src->text() : ""));
        }
    }
    // Resize display table to show all rows without internal scrolling
    int wl_row_h = qRound(whitelist_table->fontMetrics().height() * 1.2);
    int rows_h   = whitelist_table->rowCount() * wl_row_h;
    int header_h = whitelist_table->horizontalHeader()->height();
    whitelist_table->verticalHeader()->setDefaultSectionSize(wl_row_h);
    whitelist_table->setMinimumHeight(header_h + rows_h + 4);

    Dashboard::post_notification(
        QStringLiteral("QoS"),
        QStringLiteral("Game port whitelist will apply on the next control tick."));

    QTimer::singleShot(1200, this, [this]() { refresh_whitelist_from_config(); });
}

void QosPage::on_throttle_changed(int value_pct) {
    Telemetry::instance().qos_throttle_pct.store(value_pct, std::memory_order_relaxed);
    lbl_throttle->setText(QString("%1%").arg(value_pct));
}


namespace {
constexpr int k_ip_octet_min = 0;
constexpr int k_ip_octet_max = 255;

bool parse_ipv4_quads(const QString& s, int out[4]) {
    const auto parts = s.trimmed().split(QLatin1Char('.'));
    if (parts.size() != 4) return false;
    for (int i = 0; i < 4; ++i) {
        bool ok = false;
        const int v = parts[i].toInt(&ok);
        if (!ok || v < k_ip_octet_min || v > k_ip_octet_max) return false;
        out[i] = v;
    }
    return true;
}

QString format_ipv4_quads(const int q[4]) {
    return QStringLiteral("%1.%2.%3.%4").arg(q[0]).arg(q[1]).arg(q[2]).arg(q[3]);
}

std::optional<QString> numpad_edit_ipv4(QWidget* parent, const QString& title, const QString& current) {
    int q[4] = {192, 168, 1, 1};
    if (!current.trimmed().isEmpty()) {
        int parsed[4];
        if (parse_ipv4_quads(current, parsed)) {
            for (int i = 0; i < 4; ++i) q[i] = parsed[i];
        }
    }
    for (int i = 0; i < 4; ++i) {
        const QString oct_title = QStringLiteral("%1 — octet %2/4").arg(title).arg(i + 1);
        auto v = NumPadDialog::get_int(parent, oct_title, q[i], k_ip_octet_min, k_ip_octet_max);
        if (!v) return std::nullopt;
        q[i] = *v;
    }
    return format_ipv4_quads(q);
}
} // namespace

// ═════════════════════════════════════════════════════════════
// DhcpConfigDialog
// ═════════════════════════════════════════════════════════════
DhcpConfigDialog::DhcpConfigDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("DHCP Pool Configuration");
    setMinimumSize(700, 520);
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);

    auto* ip_validator = new QRegularExpressionValidator(
        QRegularExpression(
            R"(^((25[0-5]|2[0-4]\d|1\d{2}|[1-9]\d|\d)\.){3}(25[0-5]|2[0-4]\d|1\d{2}|[1-9]\d|\d)$)"),
        this);

    edit_pool_start = new QLineEdit(QString::fromStdString(Config::DHCP_POOL_START));
    edit_pool_start->setValidator(ip_validator);
    edit_pool_start->setReadOnly(true);
    edit_pool_start->setPlaceholderText("Tap to enter (keypad)");
    edit_pool_start->installEventFilter(this);
    form->addRow("Pool Start:", edit_pool_start);

    edit_pool_end = new QLineEdit(QString::fromStdString(Config::DHCP_POOL_END));
    edit_pool_end->setValidator(ip_validator);
    edit_pool_end->setReadOnly(true);
    edit_pool_end->setPlaceholderText("Tap to enter (keypad)");
    edit_pool_end->installEventFilter(this);
    form->addRow("Pool End:", edit_pool_end);

    auto* lease_row = new QHBoxLayout();
    spin_days = new QSpinBox(); spin_days->setRange(0, 365); spin_days->setSuffix(" d");
    spin_days->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin_days->setValue(static_cast<int>(Config::DHCP_LEASE_DURATION.count() / 86400));
    spin_days->setFocusPolicy(Qt::ClickFocus);
    if (auto* le = spin_days->findChild<QLineEdit*>()) {
        le->setReadOnly(true);
        le->setObjectName(QStringLiteral("dhcp_lease_days"));
        le->installEventFilter(this);
    }
    spin_hours = new QSpinBox(); spin_hours->setRange(0, 24); spin_hours->setSuffix(" h");
    spin_hours->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin_hours->setValue(static_cast<int>((Config::DHCP_LEASE_DURATION.count() % 86400) / 3600));
    spin_hours->setFocusPolicy(Qt::ClickFocus);
    if (auto* le = spin_hours->findChild<QLineEdit*>()) {
        le->setReadOnly(true);
        le->setObjectName(QStringLiteral("dhcp_lease_hours"));
        le->installEventFilter(this);
    }
    spin_minutes = new QSpinBox(); spin_minutes->setRange(0, 60); spin_minutes->setSuffix(" min");
    spin_minutes->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin_minutes->setValue(static_cast<int>((Config::DHCP_LEASE_DURATION.count() % 3600) / 60));
    spin_minutes->setFocusPolicy(Qt::ClickFocus);
    if (auto* le = spin_minutes->findChild<QLineEdit*>()) {
        le->setReadOnly(true);
        le->setObjectName(QStringLiteral("dhcp_lease_minutes"));
        le->installEventFilter(this);
    }
    lease_row->addWidget(spin_days);
    lease_row->addWidget(spin_hours);
    lease_row->addWidget(spin_minutes);
    form->addRow("Lease Duration (tap each):", lease_row);
    layout->addLayout(form);

    auto* btn_row = new QHBoxLayout();
    auto* btn_apply = new QPushButton("Apply");
    btn_apply->setObjectName("btn_primary");
    btn_row->addStretch();
    btn_row->addWidget(btn_apply);
    layout->addLayout(btn_row);

    connect(btn_apply, &QPushButton::clicked, this, &DhcpConfigDialog::on_apply);
}

bool DhcpConfigDialog::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        if (watched == edit_pool_start) {
            if (auto ip = numpad_edit_ipv4(this, QStringLiteral("DHCP pool start"), edit_pool_start->text()))
                edit_pool_start->setText(*ip);
            return true;
        }
        if (watched == edit_pool_end) {
            if (auto ip = numpad_edit_ipv4(this, QStringLiteral("DHCP pool end"), edit_pool_end->text()))
                edit_pool_end->setText(*ip);
            return true;
        }
        if (auto* le = qobject_cast<QLineEdit*>(watched)) {
            const QString on = le->objectName();
            if (on == QStringLiteral("dhcp_lease_days")) {
                if (auto v = NumPadDialog::get_int(this, QStringLiteral("Lease days"), spin_days->value(), 0, 365))
                    spin_days->setValue(*v);
                return true;
            }
            if (on == QStringLiteral("dhcp_lease_hours")) {
                if (auto v = NumPadDialog::get_int(this, QStringLiteral("Lease hours"), spin_hours->value(), 0, 24))
                    spin_hours->setValue(*v);
                return true;
            }
            if (on == QStringLiteral("dhcp_lease_minutes")) {
                if (auto v = NumPadDialog::get_int(this, QStringLiteral("Lease minutes"), spin_minutes->value(), 0, 60))
                    spin_minutes->setValue(*v);
                return true;
            }
        }
    }
    return false;
}

void DhcpConfigDialog::on_apply() {
    if (!edit_pool_start->hasAcceptableInput() || !edit_pool_end->hasAcceptableInput()) {
        edit_pool_start->setStyleSheet("border: 1px solid #cc3333;");
        edit_pool_end->setStyleSheet("border: 1px solid #cc3333;");
        return;
    }
    edit_pool_start->setStyleSheet("");
    edit_pool_end->setStyleSheet("");

    Net::IPv4Net start_ip = Config::parse_ip_str(edit_pool_start->text().toStdString());
    Net::IPv4Net end_ip   = Config::parse_ip_str(edit_pool_end->text().toStdString());
    if (ntohl(start_ip.raw()) >= ntohl(end_ip.raw())) {
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
    Config::DHCP_LEASE_DURATION = std::chrono::seconds{secs};
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
    setMinimumSize(720, 900);
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
    edit_dns_primary->setReadOnly(true);
    edit_dns_primary->setPlaceholderText("Tap to enter (keypad)");
    edit_dns_primary->installEventFilter(this);
    form->addRow("Primary DNS:", edit_dns_primary);

    edit_dns_secondary = new QLineEdit(QString::fromStdString(Config::DNS_UPSTREAM_SECONDARY));
    edit_dns_secondary->setValidator(dns_ip_validator2);
    edit_dns_secondary->setReadOnly(true);
    edit_dns_secondary->setPlaceholderText("Optional — tap to set");
    edit_dns_secondary->installEventFilter(this);
    auto* sec_dns_row = new QHBoxLayout();
    sec_dns_row->addWidget(edit_dns_secondary, 1);
    auto* btn_clear_dns2 = new QPushButton("Clear");
    btn_clear_dns2->setToolTip("Remove secondary upstream");
    connect(btn_clear_dns2, &QPushButton::clicked, this, [this]() { edit_dns_secondary->clear(); });
    sec_dns_row->addWidget(btn_clear_dns2);
    auto* sec_dns_wrap = new QWidget();
    sec_dns_wrap->setLayout(sec_dns_row);
    form->addRow("Secondary DNS:", sec_dns_wrap);

    auto* redirect_row = new QHBoxLayout();
    auto* lbl_redirect = new QLabel("Force all DNS queries to upstream server");
    lbl_redirect->setWordWrap(true);
    lbl_redirect->setStyleSheet("color: #e0e0e0;");
    sw_dns_redirect = new SwitchToggle();
    sw_dns_redirect->setChecked(Config::DNS_REDIRECT_ENABLED.load(std::memory_order_relaxed));
    sw_dns_redirect->setToolTip("When enabled, all LAN DNS queries (UDP 53) are forwarded to the configured upstream server");
    redirect_row->addWidget(lbl_redirect, 1);
    redirect_row->addWidget(sw_dns_redirect, 0, Qt::AlignVCenter);
    auto* redirect_wrap = new QWidget();
    redirect_wrap->setLayout(redirect_row);
    form->addRow("", redirect_wrap);

    auto* static_dns_label = new QLabel(
        "Static DNS — double-click hostname to type, IP for keypad (IPv4).");
    static_dns_label->setStyleSheet("color: #808090; font-size: 12px;");
    static_dns_label->setWordWrap(true);
    form->addRow(static_dns_label);

    static_dns_table = new QTableWidget(0, 2);
    static_dns_table->setHorizontalHeaderLabels({"Hostname", "IP Address"});
    static_dns_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    static_dns_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    static_dns_table->verticalHeader()->setVisible(false);
    static_dns_table->verticalHeader()->setDefaultSectionSize(52);
    static_dns_table->setFixedHeight(220);
    static_dns_table->setStyleSheet("QTableWidget { background: #1a1a2e; }");
    static_dns_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(static_dns_table, &QTableWidget::cellDoubleClicked, this,
        [this](int row, int col) {
            if (col == 0) {
                if (auto* it = static_dns_table->item(row, 0))
                    static_dns_table->editItem(it);
            } else if (col == 1) {
                auto* it = static_dns_table->item(row, 1);
                const QString cur = it ? it->text() : QString();
                if (auto ip = numpad_edit_ipv4(this, QStringLiteral("Static record IP"), cur)) {
                    if (!it)
                        static_dns_table->setItem(row, 1, new QTableWidgetItem(*ip));
                    else
                        it->setText(*ip);
                }
            }
        });

    for (size_t i = 0; i < Config::STATIC_DNS_COUNT; ++i) {
        int row = static_dns_table->rowCount();
        static_dns_table->insertRow(row);
        static_dns_table->setItem(row, 0, new QTableWidgetItem(
            QString::fromLatin1(Config::STATIC_DNS_TABLE[i].hostname.data())));
        Net::IPv4Net ip = Config::STATIC_DNS_TABLE[i].ip;
        const uint32_t r = ip.raw();
        static_dns_table->setItem(row, 1, new QTableWidgetItem(
            QString("%1.%2.%3.%4")
                .arg((r >> 24) & 0xFF).arg((r >> 16) & 0xFF)
                .arg((r >> 8) & 0xFF).arg(r & 0xFF)));
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

bool DnsConfigDialog::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        if (watched == edit_dns_primary) {
            if (auto ip = numpad_edit_ipv4(this, QStringLiteral("Primary DNS"), edit_dns_primary->text()))
                edit_dns_primary->setText(*ip);
            return true;
        }
        if (watched == edit_dns_secondary) {
            if (auto ip = numpad_edit_ipv4(this, QStringLiteral("Secondary DNS"), edit_dns_secondary->text()))
                edit_dns_secondary->setText(*ip);
            return true;
        }
    }
    return false;
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
    Config::DNS_REDIRECT_ENABLED.store(sw_dns_redirect->isChecked(), std::memory_order_relaxed);

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
        sw_dns_redirect->isChecked(), Config::STATIC_DNS_COUNT);
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

        auto* name_lbl = new QLabel(defs[i].name);
        name_lbl->setStyleSheet("font-size: 15px; font-weight: bold; color: #e0e0e0;");

        rows[i].sw = new SwitchToggle();
        rows[i].sw->setChecked(defs[i].state->load(std::memory_order_relaxed));

        auto* desc_lbl = new QLabel(defs[i].description);
        desc_lbl->setStyleSheet("color: #808090; font-size: 12px;");

        rows[i].status_label = new QLabel("● Running");
        rows[i].status_label->setStyleSheet("color: #00cc66; font-weight: bold;");

        auto* text_col = new QVBoxLayout();
        text_col->addWidget(name_lbl);
        text_col->addWidget(desc_lbl);
        row_lay->addLayout(text_col, 1);
        row_lay->addWidget(rows[i].sw, 0, Qt::AlignVCenter);

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
        connect(rows[i].sw, &SwitchToggle::toggled, [state_ptr, status_lbl, btn_cfg](bool checked) {
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
        {
            QSignalBlocker b(rows[i].sw);
            rows[i].sw->setChecked(on);
        }
        rows[i].status_label->setText(on ? "● Running" : "○ Stopped");
        rows[i].status_label->setStyleSheet(on ? "color: #00cc66; font-weight: bold;" : "color: #cc3333; font-weight: bold;");
        if (rows[i].btn_settings)
            rows[i].btn_settings->setEnabled(on);
    }
}

// ═════════════════════════════════════════════════════════════
// OverviewPage: system info refresh + config/speedtest slots (merged from SystemPage)
// ═════════════════════════════════════════════════════════════
void OverviewPage::refresh_info() {
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
    if (secs > 0)
        lbl_uptime->setText(QString("%1h %2m").arg(secs / 3600).arg((secs % 3600) / 60));
    uint64_t total = si.mem_total_kb.load(std::memory_order_relaxed);
    uint64_t avail = si.mem_avail_kb.load(std::memory_order_relaxed);
    if (total > 0)
        lbl_memory->setText(QString("%1 MB used / %2 MB total")
            .arg((total - avail) / 1024).arg(total / 1024));
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
        Net::IPv4Net ip     = tel.device_table[i].ip;
        const char* mac_str = tel.device_table[i].mac.data();

        // Look up existing policy
        bool blocked = false, rate_limited = false;
        double dl = 100.0, ul = 10.0;
        {
            std::lock_guard<std::mutex> lk(Config::device_policy_mutex);
            for (size_t j = 0; j < Config::DEVICE_POLICY_COUNT; ++j) {
                if (Config::DEVICE_POLICY_TABLE[j].ip == ip) {
                    blocked      = Config::DEVICE_POLICY_TABLE[j].blocked;
                    rate_limited = Config::DEVICE_POLICY_TABLE[j].rate_limited;
                    dl           = Config::DEVICE_POLICY_TABLE[j].dl.value;
                    ul           = Config::DEVICE_POLICY_TABLE[j].ul.value;
                    break;
                }
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
        struct in_addr addr{}; addr.s_addr = ip.raw();  // POSIX boundary
        auto* lbl_info = new QLabel(
            QString("<b>%1</b>  <span style='color:#707080;font-size:12px;'>%2</span>")
                .arg(inet_ntoa(addr)).arg(mac_str));
        lbl_info->setTextFormat(Qt::RichText);
        cl->addWidget(lbl_info);

        // Row 1: allow / rate-limit switches
        auto* chk_row = new QHBoxLayout();
        auto* lbl_allow = new QLabel("Allow Access");
        lbl_allow->setStyleSheet("color: #e0e0e0;");
        auto* sw_allow = new SwitchToggle();
        sw_allow->setChecked(!blocked);
        auto* lbl_rate = new QLabel("Rate Limit");
        lbl_rate->setStyleSheet("color: #e0e0e0;");
        auto* sw_rate = new SwitchToggle();
        sw_rate->setChecked(rate_limited);
        chk_row->addWidget(lbl_allow);
        chk_row->addWidget(sw_allow);
        chk_row->addSpacing(24);
        chk_row->addWidget(lbl_rate);
        chk_row->addWidget(sw_rate);
        chk_row->addStretch();
        cl->addLayout(chk_row);

        // Row 2: custom ± speed controls (48×48px buttons, tap-friendly)
        // Centre label is a flat QPushButton — tap to enter value via NumPad
        auto make_spin_row = [](const QString& arrow, double init_val) {
            auto* row   = new QHBoxLayout();
            auto* btn_m = new QPushButton("−");
            auto* lbl   = new QPushButton(QString("%1 %2 Mbps").arg(arrow).arg(init_val, 0, 'f', 1));
            auto* btn_p = new QPushButton("+");
            btn_m->setObjectName("spin_btn");
            btn_p->setObjectName("spin_btn");
            lbl->setFlat(true);
            lbl->setStyleSheet("font-size:14px; color:#c0c0e0; min-width:110px;"
                               "text-decoration:underline;");
            row->addWidget(btn_m);
            row->addWidget(lbl, 1);
            row->addWidget(btn_p);
            return std::tuple{row, lbl, btn_m, btn_p};
        };

        auto [dl_row, lbl_dl, btn_dl_m, btn_dl_p] = make_spin_row("↓", dl);
        auto [ul_row, lbl_ul, btn_ul_m, btn_ul_p] = make_spin_row("↑", ul);
        cl->addLayout(dl_row);
        cl->addLayout(ul_row);

        cards_layout->insertWidget(cards_layout->count() - 1, card);

        // Push row — capture index so lambdas can safely reference rows_[idx]
        DeviceRow r;
        r.ip        = ip;
        r.sw_allow = sw_allow;
        r.sw_rate  = sw_rate;
        r.lbl_dl    = lbl_dl;
        r.lbl_ul    = lbl_ul;
        r.val_dl    = dl;
        r.val_ul    = ul;
        size_t idx = rows_.size();
        rows_.push_back(r);

        connect(btn_dl_m, &QPushButton::clicked, this, [this, idx]() {
            rows_[idx].val_dl = std::max(k_device_bw_min_mbps, rows_[idx].val_dl - 1.0);
            rows_[idx].lbl_dl->setText(QString("↓ %1 Mbps").arg(rows_[idx].val_dl, 0, 'f', 1));
        });
        connect(btn_dl_p, &QPushButton::clicked, this, [this, idx]() {
            rows_[idx].val_dl = std::min(k_device_bw_max_mbps, rows_[idx].val_dl + 1.0);
            rows_[idx].lbl_dl->setText(QString("↓ %1 Mbps").arg(rows_[idx].val_dl, 0, 'f', 1));
        });
        connect(btn_ul_m, &QPushButton::clicked, this, [this, idx]() {
            rows_[idx].val_ul = std::max(k_device_bw_min_mbps, rows_[idx].val_ul - 1.0);
            rows_[idx].lbl_ul->setText(QString("↑ %1 Mbps").arg(rows_[idx].val_ul, 0, 'f', 1));
        });
        connect(btn_ul_p, &QPushButton::clicked, this, [this, idx]() {
            rows_[idx].val_ul = std::min(k_device_bw_max_mbps, rows_[idx].val_ul + 1.0);
            rows_[idx].lbl_ul->setText(QString("↑ %1 Mbps").arg(rows_[idx].val_ul, 0, 'f', 1));
        });
        // Tap label → NumPad direct entry
        connect(lbl_dl, &QPushButton::clicked, this, [this, idx]() {
            auto v = NumPadDialog::get_double(this, "Download Limit (Mbps)",
                                              rows_[idx].val_dl, k_device_bw_min_mbps, k_device_bw_max_mbps);
            if (!v) return;
            rows_[idx].val_dl = *v;
            rows_[idx].lbl_dl->setText(QString("↓ %1 Mbps").arg(*v, 0, 'f', 1));
        });
        connect(lbl_ul, &QPushButton::clicked, this, [this, idx]() {
            auto v = NumPadDialog::get_double(this, "Upload Limit (Mbps)",
                                              rows_[idx].val_ul, k_device_bw_min_mbps, k_device_bw_max_mbps);
            if (!v) return;
            rows_[idx].val_ul = *v;
            rows_[idx].lbl_ul->setText(QString("↑ %1 Mbps").arg(*v, 0, 'f', 1));
        });
    }
}

void DevicePage::on_apply_all() {
    for (auto& r : rows_) {
        Config::DevicePolicy p{};
        p.ip           = r.ip;
        p.blocked      = !r.sw_allow->isChecked();
        p.rate_limited = r.sw_rate->isChecked();
        p.dl           = Traffic::Mbps{r.val_dl};
        p.ul           = Traffic::Mbps{r.val_ul};
        Config::upsert_device_policy(p);
    }
    Config::DEVICE_POLICY_DIRTY.store(true, std::memory_order_release);
    std::println("[GUI] Device policies applied for {} device(s)", rows_.size());
}

// ═════════════════════════════════════════════════════════════
// Dashboard: main control panel frame
// ═════════════════════════════════════════════════════════════
Dashboard* Dashboard::instance_ = nullptr;

Dashboard::Dashboard(QWidget* parent) : QMainWindow(parent) {
    instance_ = this;
    setWindowTitle("High-Performance Gaming Traffic Prioritizer");
    setStyleSheet(DARK_STYLESHEET);
    setup_ui();
    ui_timer_id_   = startTimer(16, Qt::PreciseTimer); // 60Hz: spring, badge, selftest overlay
    plot_timer_id_ = startTimer(40, Qt::PreciseTimer); // 25Hz: plots, header rates, page data
    qApp->installEventFilter(this); // global: 15% pull zone + mid-animation interrupt
}

Dashboard::~Dashboard() {
    instance_ = nullptr;
}

void Dashboard::post_notification(const QString& title, const QString& body) {
    if (!instance_) return;
    QMetaObject::invokeMethod(instance_, [title, body]() {
        if (instance_ && instance_->notif_panel_)
            instance_->notif_panel_->push_notification(title, body);
    }, Qt::QueuedConnection);
}

void Dashboard::on_selftest_done(const HPGTP::SelfTest::Report& r) {
    if (!instance_) return;
    QMetaObject::invokeMethod(instance_, [r]() {
        if (!instance_) return;
        if (instance_->testing_overlay_)
            instance_->testing_overlay_->hide();

        size_t failures = r.count - r.passed;
        if (failures == 0) {
            instance_->notif_panel_->push_notification(
                "Self-Test Passed",
                QString("All %1 checks passed. Data plane ready.").arg(r.count));
        } else {
            instance_->notif_panel_->push_notification(
                QString("Self-Test: %1 failure(s)").arg(failures),
                QString("%1/%2 checks passed.").arg(r.passed).arg(r.count));
            for (size_t i = 0; i < r.count; ++i) {
                if (!r.cases[i].pass)
                    instance_->notif_panel_->push_notification(
                        QString("[FAIL] %1").arg(r.cases[i].name.data()),
                        r.cases[i].detail.data());
            }
        }
        instance_->notif_panel_->push_notification(
            "Engine Ready", "Data plane Cores 2/3 attached. Forwarding engine running.");
    }, Qt::QueuedConnection);
}

void Dashboard::setup_ui() {
    QWidget* central = new QWidget(this);
    setCentralWidget(central);
    statusBar()->hide();

    auto* root_layout = new QVBoxLayout(central);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    // ── Header bar ──
    header_ = new QFrame();
    auto* header = header_;
    header->setObjectName("header_frame");
    header->installEventFilter(this);  // detect swipe-down to expand notification panel
    auto* header_lay = new QHBoxLayout(header);
    header_lay->setContentsMargins(12, 0, 12, 0);
    header_lay->setSpacing(8);

    auto* btn_shutdown_hdr = new QPushButton("Shutdown");
    btn_shutdown_hdr->setObjectName("btn_header_icon");
    btn_shutdown_hdr->setStyleSheet(
        "QPushButton#btn_header_icon { color: #cc4444; }"
        "QPushButton#btn_header_icon:pressed { background-color: rgba(204,68,68,40); }");
    connect(btn_shutdown_hdr, &QPushButton::clicked, this, &Dashboard::on_shutdown_clicked);
    header_lay->addWidget(btn_shutdown_hdr);

    auto* title = new QLabel("HPGTP");
    title->setObjectName("header_title");
    header_lay->addWidget(title);

    header_lay->addStretch();

    hdr_info_ = new QLabel("↓ --  ↑ --  🌡 --");
    hdr_info_->setStyleSheet("color: #a0b8d0; font-size: 13px;");
    header_lay->addWidget(hdr_info_);

    hdr_badge_ = new QLabel();
    hdr_badge_->setAlignment(Qt::AlignCenter);
    hdr_badge_->setFixedSize(24, 24);
    hdr_badge_->setStyleSheet(
        "background-color: #cc2222; border-radius: 12px;"
        "color: #ffffff; font-size: 11px; font-weight: bold;");
    hdr_badge_->hide();
    header_lay->addWidget(hdr_badge_);

    root_layout->addWidget(header);

    // ── Page stack (full width) ──
    page_stack = new QStackedWidget();
    page_overview   = new OverviewPage();
    page_interfaces = new InterfacePage();
    page_qos        = new QosPage();
    page_services   = new ServicePage();
    page_devices    = new DevicePage();

    auto wrap = [](QWidget* page) -> QScrollArea* {
        auto* sa = new QScrollArea();
        sa->setWidget(page);
        sa->setWidgetResizable(true);
        sa->setFrameShape(QFrame::NoFrame);
        sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        return sa;
    };
    page_stack->addWidget(wrap(page_overview));    // 0
    page_stack->addWidget(wrap(page_interfaces));  // 1
    page_stack->addWidget(wrap(page_qos));         // 2
    page_stack->addWidget(wrap(page_services));    // 3
    page_stack->addWidget(wrap(page_devices));     // 4
    root_layout->addWidget(page_stack, 1);

    // ── Bottom tab bar ──
    setup_tabbar(root_layout);

    // ── Notification panel: floats above all content ──
    notif_panel_ = new NotificationPanel(centralWidget());
    notif_panel_->setFixedSize(centralWidget()->width(), centralWidget()->height());
    notif_panel_->raise();

    // Async startup log read — regular file I/O is offloaded off the GUI thread.
    // Each line is queued back via invokeMethod; if Dashboard is destroyed before
    // delivery, Qt silently drops the queued event (QObject receiver invalidation).
    startup_log_reader_ = std::jthread([](std::stop_token st) {
        QFile log("/tmp/hpgtp_startup.log");
        if (!log.open(QIODevice::ReadOnly | QIODevice::Text)) return;
        QTextStream ts(&log);
        while (!ts.atEnd()) {
            if (st.stop_requested()) return;
            QString line = ts.readLine().trimmed();
            if (line.isEmpty()) continue;
            int sep = line.indexOf('|');
            QString title = (sep >= 0) ? line.left(sep)   : line;
            QString body  = (sep >= 0) ? line.mid(sep + 1): QString();
            QMetaObject::invokeMethod(instance_, [title, body]() {
                if (instance_ && instance_->notif_panel_)
                    instance_->notif_panel_->push_notification(title, body);
            }, Qt::QueuedConnection);
        }
    });

    // Self-test results posted asynchronously via on_selftest_done() after worker completes.

    // Self-test overlay — covers content area until on_selftest_done() is called
    auto* cw = centralWidget();
    testing_overlay_ = new QWidget(cw);
    testing_overlay_->setStyleSheet("background-color: rgba(10,10,20,210);");
    auto* ov_lay = new QVBoxLayout(testing_overlay_);
    ov_lay->setAlignment(Qt::AlignCenter);
    ov_lay->setSpacing(20);

    auto* ov_label = new QLabel("System Self-Test Running...");
    ov_label->setStyleSheet("color: #c0c8e0; font-size: 18px; background: transparent;");
    ov_label->setAlignment(Qt::AlignCenter);

    testing_progress_ = new QProgressBar();
    testing_progress_->setRange(0, SELFTEST_TICKS);
    testing_progress_->setValue(0);
    testing_progress_->setFixedWidth(320);
    testing_progress_->setFixedHeight(10);
    testing_progress_->setTextVisible(false);
    testing_progress_->setStyleSheet(
        "QProgressBar { background: #1a1a2e; border: 1px solid #2a2a4a; border-radius: 5px; }"
        "QProgressBar::chunk { background: #0077ff; border-radius: 4px; }");

    ov_lay->addWidget(ov_label);
    ov_lay->addWidget(testing_progress_, 0, Qt::AlignHCenter);
    testing_overlay_->setGeometry(cw->rect());
    testing_overlay_->raise();
    testing_overlay_->show();
}

// Tab button with independently-sized icon and text label.
// Intentionally has no Q_OBJECT — uses only inherited QAbstractButton signals.
class NavTabButton final : public QAbstractButton {
public:
    NavTabButton(const QString& icon_ch, const QString& label_text, QWidget* parent = nullptr)
        : QAbstractButton(parent), icon_(icon_ch), text_(label_text)
    {
        setCheckable(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(96);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Background
        p.fillRect(rect(), QColor("#12122a"));

        // Pressed tint
        if (isDown())
            p.fillRect(rect(), QColor(0, 119, 255, 25));

        const bool chk = isChecked();
        const QColor ink = chk ? QColor("#0077ff") : QColor("#606080");

        // Active top border
        if (chk)
            p.fillRect(0, 0, width(), 3, QColor("#0077ff"));

        p.setPen(ink);

        // Icon — 30px
        QFont fi = font();
        fi.setPixelSize(30);
        p.setFont(fi);
        p.drawText(QRect(0, 4, width(), 44), Qt::AlignCenter, icon_);

        // Label — 20px
        QFont ft = font();
        ft.setPixelSize(20);
        if (chk) ft.setBold(true);
        p.setFont(ft);
        p.drawText(QRect(0, 52, width(), 38), Qt::AlignCenter, text_);
    }
private:
    QString icon_;
    QString text_;
};

void Dashboard::setup_tabbar(QBoxLayout* root_layout) {
    auto* bar = new QFrame();
    bar->setObjectName("tab_bar_frame");
    bar->setFixedHeight(96);
    auto* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    struct TabDef { const char* icon; const char* label; int page; };
    static constexpr TabDef TABS[] = {
        {"⌂",  "Overview",   0},
        {"⚡",  "QoS",        2},
        {"⚙",  "Services",   3},
        {"▭",  "Devices",    4},
        {"🌐", "Interfaces", 1},
    };

    auto* grp = new QButtonGroup(bar);
    grp->setExclusive(true);

    for (int i = 0; i < 5; ++i) {
        auto* btn = new NavTabButton(TABS[i].icon, TABS[i].label, bar);
        int page_idx = TABS[i].page;
        connect(btn, &QAbstractButton::clicked, this, [this, page_idx]() {
            on_tab_clicked(page_idx);
        });
        grp->addButton(btn);
        lay->addWidget(btn, 1);
        tab_btns_[i] = btn;
    }
    tab_btns_[0]->setChecked(true);

    root_layout->addWidget(bar);
}

void Dashboard::on_tab_clicked(int page_index) {
    page_stack->setCurrentIndex(page_index);
    if (page_index == 2) page_qos->refresh_whitelist_from_config();
    if (page_index == 4) page_devices->refresh();
}


void Dashboard::on_shutdown_clicked() {
    QDialog dlg(this);
    dlg.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    dlg.setStyleSheet(DARK_STYLESHEET);
    dlg.setFixedWidth(420);

    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(24, 24, 24, 24);
    lay->setSpacing(12);

    auto* lbl = new QLabel("Exit application?");
    lbl->setAlignment(Qt::AlignCenter);
    lbl->setStyleSheet("font-size: 17px; font-weight: bold; color: #ffffff; padding: 8px 0 16px 0;");
    lay->addWidget(lbl);

    auto* btn_save   = new QPushButton("Yes — Save Settings");
    auto* btn_nosave = new QPushButton("Yes — Don't Save");
    auto* btn_cancel = new QPushButton("Cancel");
    btn_save->setObjectName("btn_primary");
    btn_nosave->setObjectName("btn_danger");
    lay->addWidget(btn_save);
    lay->addWidget(btn_nosave);
    lay->addWidget(btn_cancel);

    // Save settings then exit — skip redundant save in main()
    connect(btn_save, &QPushButton::clicked, &dlg, [&dlg]() {
        if (auto sr = Config::save_config("config/config.txt"); !sr)
            std::println(stderr, "[GUI] {}", sr.error());
        Config::SAVE_ON_EXIT.store(false, std::memory_order_relaxed);
        dlg.accept();
        QApplication::quit();
    });
    // Exit without saving
    connect(btn_nosave, &QPushButton::clicked, &dlg, [&dlg]() {
        Config::SAVE_ON_EXIT.store(false, std::memory_order_relaxed);
        dlg.accept();
        QApplication::quit();
    });
    connect(btn_cancel, &QPushButton::clicked, &dlg, &QDialog::reject);

    dlg.exec();
}


bool Dashboard::eventFilter(QObject* obj, QEvent* event) {
    // ── Legacy header swipe (kept as fallback) ──────────────────────────────
    if (obj == header_ && notif_panel_ && !notif_panel_->is_expanded()) {
        if (event->type() == QEvent::MouseButtonPress) {
            hdr_swipe_y0_ = static_cast<QMouseEvent*>(event)->pos().y();
        } else if (event->type() == QEvent::MouseMove && hdr_swipe_y0_ >= 0) {
            int dy = static_cast<QMouseEvent*>(event)->pos().y() - hdr_swipe_y0_;
            if (dy > 28) {
                hdr_swipe_y0_ = -1;
                notif_panel_->kick(12.0);
                notif_panel_->set_expanded(true);
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            hdr_swipe_y0_ = -1;
        }
    }

    // ── Global: 15% pull zone (open) + mid-animation interrupt (close) ──────
    // Installed via qApp->installEventFilter — fires for every widget's events.
    if (notif_panel_ && !(testing_overlay_ && testing_overlay_->isVisible())) {
        const auto etype = event->type();

        if (etype == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            bool panel_active = notif_panel_->is_expanded() || !notif_panel_->is_settled();
            const QPoint gp = me->globalPosition().toPoint();
            bool in_pull_zone = gp.y() < height() * 15 / 100;
            if (panel_active || in_pull_zone) {
                notif_pull_y0_ = gp.y();
                notif_pull_x0_ = gp.x();
            }

        } else if (etype == QEvent::MouseMove && notif_pull_y0_ >= 0) {
            auto* me = static_cast<QMouseEvent*>(event);
            const QPoint gp = me->globalPosition().toPoint();
            int dy = gp.y() - notif_pull_y0_;
            int dx = std::abs(gp.x() - notif_pull_x0_);
            // Horizontal intent wins — abandon notification tracking
            if (dx > 30) {
                notif_pull_y0_ = notif_pull_x0_ = -1;
            // Pull-down open: only when panel not yet expanding, downward and not diagonal
            } else if (!notif_panel_->is_expanded() && notif_panel_->is_settled()
                       && dy > 28 && dx < 20) {
                notif_pull_y0_ = notif_pull_x0_ = -1;
                notif_panel_->kick(12.0);
                notif_panel_->set_expanded(true);
            }

        } else if (etype == QEvent::MouseButtonRelease && notif_pull_y0_ >= 0) {
            auto* me = static_cast<QMouseEvent*>(event);
            int dy = me->globalPosition().toPoint().y() - notif_pull_y0_;
            // Interrupt / collapse: upward swipe when panel is opening or open.
            // Skip if touch is on the panel itself (NotificationPanel::mouseReleaseEvent
            // already handles that path — avoid double kick).
            bool panel_active = notif_panel_->is_expanded() || !notif_panel_->is_settled();
            auto* w = qobject_cast<QWidget*>(obj);
            bool on_panel = w && (w == notif_panel_ || notif_panel_->isAncestorOf(w));
            if (panel_active && !on_panel && dy < -192) {
                notif_panel_->kick(-12.0);
                notif_panel_->set_expanded(false);
            }
            notif_pull_y0_ = notif_pull_x0_ = -1;
        }
    }

    return QMainWindow::eventFilter(obj, event);
}

void Dashboard::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (testing_overlay_ && testing_overlay_->isVisible())
        testing_overlay_->setGeometry(centralWidget()->rect());
    if (notif_panel_) {
        auto* cw = centralWidget();
        notif_panel_->setFixedSize(cw->width(), cw->height());
        // If collapsed, snap to just off-screen top so position stays correct
        if (!notif_panel_->is_expanded())
            notif_panel_->move(0, -cw->height());
    }
}

void Dashboard::timerEvent(QTimerEvent* event) {
    const int tid = event->timerId();

    // ── 60Hz UI tick: spring animation, selftest overlay, notification badge ──
    if (tid == ui_timer_id_) {
        ui_tick_++;

        // Advance self-test progress bar; block all other UI work until done
        if (testing_overlay_ && testing_overlay_->isVisible()) {
            if (selftest_tick_ < SELFTEST_TICKS)
                testing_progress_->setValue(++selftest_tick_);
            return;
        }

        // Spring physics for notification panel (no-op when settled)
        if (!notif_panel_->is_settled()) {
            notif_panel_->advance_spring();
            notif_panel_->update();
        }

        // Notification badge
        int unread = notif_panel_->unread_count();
        if (unread > 0) {
            hdr_badge_->setText(unread > 99 ? "99+" : QString::number(unread));
            hdr_badge_->show();
        } else {
            hdr_badge_->hide();
        }
        return;
    }

    // ── 25Hz data tick: header rates, page plots, counter snapshot ──
    if (tid != plot_timer_id_) return;
    if (testing_overlay_ && testing_overlay_->isVisible()) return;

    plot_tick_++;

    // Measure actual elapsed time so rate is correct even if timer fires late
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - plot_last_tick_).count();
    plot_last_tick_ = now;
    // Clamp to [20ms, 200ms]: guards divide-by-zero on first tick and huge
    // spikes if the process was suspended (e.g. SIGSTOP during debug)
    elapsed = std::clamp(elapsed, 0.020, 0.200);

    auto& tel = Telemetry::instance();

    // Bandwidth: byte delta / actual elapsed seconds → Mbps
    uint64_t cur_b2 = tel.core_metrics[2].bytes.load(std::memory_order_relaxed);
    uint64_t cur_b3 = tel.core_metrics[3].bytes.load(std::memory_order_relaxed);
    double dl = (cur_b2 - last_bytes[2]) * 8.0 / elapsed / 1e6;
    double ul = (cur_b3 - last_bytes[3]) * 8.0 / elapsed / 1e6;

    // CPU temperature
    double t = tel.cpu_temp_celsius.load(std::memory_order_relaxed);

    // Update header info label: ↓ DL  ↑ UL  🌡 temp
    hdr_info_->setText(QString("↓%1  ↑%2  🌡%3°C")
        .arg(dl, 0, 'f', 1)
        .arg(ul, 0, 'f', 1)
        .arg(t > 0 ? t : 0.0, 0, 'f', 0));

    // Refresh overview plots if visible; update system info every 25 ticks (1s)
    if (page_stack->currentIndex() == 0)
        page_overview->refresh(tel, last_pkts, last_bytes);
    if (plot_tick_ % 25 == 0)
        page_overview->refresh_info();

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

} // namespace HPGTP::GUI
