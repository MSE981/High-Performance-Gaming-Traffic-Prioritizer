#include "GUI/Dashboard.hpp"
#include "GUI/StyleSheet.hpp"
#include "SystemOptimizer_util.hpp"
#include "App.hpp"
#include "Config.hpp"
#include "SelfTest.hpp"
#include <cstring>
#include "SystemOptimizer_util.hpp"
#include <QApplication>
#include <QPointer>
#include <QScreen>
#include <QMetaObject>
#include <QButtonGroup>
#include <QMenu>
#include <QBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
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
#include <utility>
#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <vector>
#include <optional>
#include <cmath>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <arpa/inet.h>

namespace HPGTP::GUI {

constexpr double k_qos_bw_min_mbps     = 0.1;
constexpr double k_qos_bw_max_mbps     = 1e6;
constexpr double k_device_bw_min_mbps  = 0.1;
constexpr double k_device_bw_max_mbps  = 10000.0;

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

// ═════════════════════════════════════════════════════════════
// RealTimePlot ? shift-buffer sampling, redrawn on the plot timer.
// ═════════════════════════════════════════════════════════════
RealTimePlot::RealTimePlot(QWidget* parent) : QWidget(parent) {
    setMinimumSize(200, 300);
    shift_buffer.fill(0.0);
}

void RealTimePlot::add_sample(double val) {
    shift_buffer[shift_head] = val;
    shift_head = (shift_head + 1) % SHIFT_BUFFER_SIZE;
}

void RealTimePlot::setLineColor(const QColor& color) {
    line_color_ = color;
}

void RealTimePlot::paintEvent(QPaintEvent*) {
    QPainter p(this);
    // No antialiasing: one sample per display pixel, cheaper on software rendering.
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setRenderHint(QPainter::TextAntialiasing, false);

    // Dark background, one polyline, dashed grid with Y-axis labels.
    p.fillRect(rect(), QColor(18, 18, 32));
    const int ml = 50, mt = 6, mb = 6;
    const int pw = width() - ml;
    const int ph = height() - mt - mb;
    const double y_max = fixed_max_ > 0.0 ? fixed_max_ : (current_max + 1.0);
    const bool use_k = (y_max >= 10000.0);

    // Y-axis grid + labels at 0, 0.2, 0.4, 0.6, 0.8, 1.0
    QFont lf = p.font();
    lf.setPixelSize(11);
    p.setFont(lf);
    for (int i = 0; i <= 5; ++i) {
        const double ratio = i / 5.0;
        const int iy = mt + ph - static_cast<int>(ratio * ph);
        p.setPen(QPen(QColor(255, 255, 255, 18), 1, Qt::DashLine));
        p.drawLine(ml, iy, width(), iy);
        const QString lbl =
            (i == 0) ? "0"
            : use_k ? QString("%1K").arg(static_cast<int>(y_max * ratio / 1000.0))
                    : QString::number(static_cast<int>(y_max * ratio));
        p.setPen(QColor(130, 130, 150));
        p.drawText(QRect(0, iy - 8, ml - 4, 16), Qt::AlignRight | Qt::AlignVCenter, lbl);
    }

    if (pw <= 0 || ph <= 0) return;

    // One sample per display pixel column, resampled from the circular window.
    const int cols = pw;
    QPolygonF pts;
    pts.reserve(cols);
    for (int col = 0; col < cols; ++col) {
        const double t = (cols > 1) ? static_cast<double>(col) / (cols - 1) : 1.0;
        const int idx = static_cast<int>(
            std::llround(t * (SHIFT_BUFFER_SIZE - 1))) % SHIFT_BUFFER_SIZE;
        const double sample = shift_buffer[(shift_head + idx) % SHIFT_BUFFER_SIZE];
        const double norm = sample < 0.0 ? 0.0 : (sample > y_max ? 1.0 : sample / y_max);
        pts.push_back(QPointF(static_cast<double>(ml) + col,
                              mt + ph - norm * ph));
    }
    p.setPen(QPen(line_color_, 1, Qt::SolidLine, Qt::SquareCap, Qt::BevelJoin));
    p.drawPolyline(pts);
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

    // Two charts: High (unthrottled) and Normal (throttled) traffic.
    auto* plot_row = new QHBoxLayout();
    auto* high_group = new QGroupBox("High priority (Mb)");
    auto* high_lay = new QVBoxLayout(high_group);
    high_plot = new RealTimePlot();
    high_plot->set_fixed_max(9.0);
    high_plot->setLineColor(QColor(0, 200, 255));
    high_lay->addWidget(high_plot);
    plot_row->addWidget(high_group);

    auto* normal_group = new QGroupBox("Normal (Mb)");
    auto* normal_lay = new QVBoxLayout(normal_group);
    normal_plot = new RealTimePlot();
    normal_plot->set_fixed_max(900.0);
    normal_plot->setLineColor(QColor(80, 230, 120));
    normal_lay->addWidget(normal_plot);
    plot_row->addWidget(normal_group);
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

    // System info (merged from SystemPage)
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

void OverviewPage::refresh(const Telemetry& tel, double high_mbps, double normal_mbps) {
    for (int i = 0; i < 4; ++i) {
        int load_pct = tel.core_metrics[i].cpu_load_pct.load(std::memory_order_relaxed);
        // Colour: green <50%, orange 50-80%, red >80%
        const char* colour = load_pct < 50 ? "#00cc66" : (load_pct < 80 ? "#ffaa00" : "#ff4444");
        core_labels[i]->setText(QString("Core %1\n%2%").arg(i).arg(load_pct));
        core_labels[i]->setStyleSheet(
            QString("background-color: #22223a; border: 1px solid #2a2a4a; border-radius: 6px;"
                    " padding: 10px; font-size: 15px; color: %1;").arg(colour));
    }

    high_plot->add_sample(high_mbps);
    normal_plot->add_sample(normal_mbps);
    high_plot->update();
    normal_plot->update();

    bool bridge = tel.effective_bridge_mode.load(std::memory_order_acquire);
    lbl_mode->setText(bridge ? "Mode: Bridge" : "Mode: Acceleration");
    lbl_mode->setStyleSheet(bridge
        ? "color: #ffaa00; font-weight: bold; font-size: 15px;"
        : "color: #00cc66; font-weight: bold; font-size: 15px;");
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
        sw_acceleration->setChecked(
            Telemetry::instance().effective_acceleration.load(std::memory_order_acquire));
        connect(sw_acceleration, &SwitchToggle::toggled, this, &QosPage::on_toggle_accel);
        accel_row->addWidget(lbl_accel, 1);
        accel_row->addWidget(sw_acceleration, 0, Qt::AlignVCenter);
        layout->addLayout(accel_row);
    }

    // Bandwidth limit
    auto* bw_group = new QGroupBox("Global Bandwidth Limits");
    auto* bw_form = new QFormLayout(bw_group);
    {
        double idl = Telemetry::instance().qos_global_dl_mbps_pending.load(std::memory_order_relaxed);
        double iul = Telemetry::instance().qos_global_ul_mbps_pending.load(std::memory_order_relaxed);
        edit_dl_limit = new QLineEdit(QString::number(idl, 'g', 12));
        edit_ul_limit = new QLineEdit(QString::number(iul, 'g', 12));
    }
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
    bw_form->addRow("Download Limit (Mb):", edit_dl_limit);
    bw_form->addRow("Upload Limit (Mb):", edit_ul_limit);
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

}

void QosPage::refresh_from_backend() {
    {
        QSignalBlocker b(*sw_acceleration);
        sw_acceleration->setChecked(
            Telemetry::instance().effective_acceleration.load(std::memory_order_acquire));
    }
    double dl = Telemetry::instance().qos_global_dl_mbps_pending.load(std::memory_order_relaxed);
    double ul = Telemetry::instance().qos_global_ul_mbps_pending.load(std::memory_order_relaxed);
    edit_dl_limit->setText(QString::number(dl, 'g', 12));
    edit_ul_limit->setText(QString::number(ul, 'g', 12));
}

bool QosPage::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        if (watched == edit_dl_limit) {
            bool ok = false;
            double cur = edit_dl_limit->text().trimmed().toDouble(&ok);
            if (!ok) cur = 500.0;
            cur = std::clamp(cur, k_qos_bw_min_mbps, k_qos_bw_max_mbps);
            if (auto v = NumPadDialog::get_double(this, QStringLiteral("Download Limit (Mb)"),
                                                  cur, k_qos_bw_min_mbps, k_qos_bw_max_mbps))
                edit_dl_limit->setText(QString::number(*v, 'g', 12));
            return true;
        }
        if (watched == edit_ul_limit) {
            bool ok = false;
            double cur = edit_ul_limit->text().trimmed().toDouble(&ok);
            if (!ok) cur = 50.0;
            cur = std::clamp(cur, k_qos_bw_min_mbps, k_qos_bw_max_mbps);
            if (auto v = NumPadDialog::get_double(this, QStringLiteral("Upload Limit (Mb)"),
                                                  cur, k_qos_bw_min_mbps, k_qos_bw_max_mbps))
                edit_ul_limit->setText(QString::number(*v, 'g', 12));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QosPage::on_apply_global_bw() {
    if (!edit_dl_limit->hasAcceptableInput() || !edit_ul_limit->hasAcceptableInput()) {
        return;
    }
    bool ok_dl = false, ok_ul = false;
    double dl = edit_dl_limit->text().trimmed().toDouble(&ok_dl);
    double ul = edit_ul_limit->text().trimmed().toDouble(&ok_ul);
    if (!ok_dl || !ok_ul || dl <= 0.0 || ul <= 0.0
        || dl < k_qos_bw_min_mbps || ul < k_qos_bw_min_mbps
        || dl > k_qos_bw_max_mbps || ul > k_qos_bw_max_mbps) {
        return;
    }
    auto& tel = Telemetry::instance();
    tel.qos_global_dl_mbps_pending.store(dl, std::memory_order_relaxed);
    tel.qos_global_ul_mbps_pending.store(ul, std::memory_order_relaxed);
    tel.qos_global_bw_dirty.store(true, std::memory_order_release);
}

void QosPage::on_toggle_accel() {
    bool on = sw_acceleration->isChecked();
    auto& tel = Telemetry::instance();
    tel.acceleration_pending.store(on, std::memory_order_relaxed);
    tel.mode_config_dirty.store(true, std::memory_order_release);
    std::println("[GUI] Pending acceleration mode: {}", on ? "ON" : "OFF");
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
        const QString oct_title = QStringLiteral("%1, octet %2/4").arg(title).arg(i + 1);
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
DhcpConfigDialog::DhcpConfigDialog(std::function<void()> apply_callback, QWidget* parent)
    : QDialog(parent), apply_callback_(std::move(apply_callback)) {
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

    auto start_e = Config::parse_ip_str(edit_pool_start->text().toStdString());
    auto end_e   = Config::parse_ip_str(edit_pool_end->text().toStdString());
    if (!start_e || !end_e) {
        edit_pool_start->setStyleSheet("border: 1px solid #cc3333;");
        edit_pool_end->setStyleSheet("border: 1px solid #cc3333;");
        return;
    }
    Utils::Net::IPv4Net start_ip = *start_e;
    Utils::Net::IPv4Net end_ip   = *end_e;
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
    if (apply_callback_) apply_callback_();
    std::println("[GUI] DHCP pool updated: {} to {}, lease {}s",
        Config::DHCP_POOL_START, Config::DHCP_POOL_END, secs);
    accept();
}

// ═════════════════════════════════════════════════════════════
// ServicePage: service toggle page
// ═════════════════════════════════════════════════════════════
ServicePage::ServicePage(std::function<void()> dhcp_apply_callback, QWidget* parent)
    : QWidget(parent), dhcp_apply_callback_(std::move(dhcp_apply_callback)) {
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel("Services");
    title->setObjectName("section_title");
    layout->addWidget(title);

    auto* desc = new QLabel("DHCP is always active and assigns IP addresses to LAN clients.");
    desc->setStyleSheet("color: #707080; font-size: 13px; margin-bottom: 8px;");
    layout->addWidget(desc);

    auto* row_frame = new QFrame();
    row_frame->setStyleSheet("QFrame { background-color: #22223a; border: 1px solid #2a2a4a; border-radius: 6px; padding: 8px; margin: 2px 0px; }");
    auto* row_lay = new QHBoxLayout(row_frame);

    auto* name_lbl = new QLabel("DHCP (Dynamic Host Config)");
    name_lbl->setStyleSheet("font-size: 15px; font-weight: bold; color: #e0e0e0;");

    auto* desc_lbl = new QLabel("Automatically assigns IP addresses to LAN clients");
    desc_lbl->setStyleSheet("color: #808090; font-size: 12px;");

    rows[0].status_label = new QLabel("● Running");
    rows[0].status_label->setStyleSheet("color: #00cc66; font-weight: bold;");

    auto* text_col = new QVBoxLayout();
    text_col->addWidget(name_lbl);
    text_col->addWidget(desc_lbl);
    row_lay->addLayout(text_col, 1);

    auto* btn = new QPushButton("Set DHCP");
    rows[0].btn_settings = btn;
    row_lay->addWidget(btn);
    connect(btn, &QPushButton::clicked, this, [this]() {
        DhcpConfigDialog dlg(dhcp_apply_callback_, this);
        dlg.exec();
    });

    row_lay->addWidget(rows[0].status_label);
    layout->addWidget(row_frame);

    layout->addStretch();
}

void ServicePage::refresh_status() {
    // DHCP is always on; report a running state only.
    rows[0].status_label->setText("● Running");
    rows[0].status_label->setStyleSheet("color: #00cc66; font-weight: bold;");
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



// DevicePage: read-only ARP device list
// ============================================================================
DevicePage::DevicePage(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel("Devices");
    title->setObjectName("section_title");
    layout->addWidget(title);

    auto* desc = new QLabel("LAN devices currently in the ARP table (read-only).");
    desc->setStyleSheet("color: #707080; font-size: 13px; margin-bottom: 8px;");
    layout->addWidget(desc);

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
}

void DevicePage::refresh() {
    auto& tel = Telemetry::instance();
    const uint8_t cnt = tel.device_count.load(std::memory_order_acquire);
    const uint64_t drev = tel.device_table_revision.load(std::memory_order_acquire);
    if (cnt == last_device_count && drev == last_device_table_revision_) return;
    last_device_count = cnt;
    last_device_table_revision_ = drev;

    while (cards_layout->count() > 1) {
        QLayoutItem* item = cards_layout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    for (uint8_t i = 0; i < cnt; ++i) {
        const Utils::Net::IPv4Net ip = tel.device_table[i].ip;
        const char* mac_str = tel.device_table[i].mac.data();
        auto* card = new QFrame();
        card->setStyleSheet(
            "QFrame { background:#1e1e3a; border:1px solid #2a2a4a; border-radius:6px; }"
        );
        auto* cl = new QVBoxLayout(card);
        cl->setSpacing(6);
        cl->setContentsMargins(12, 8, 12, 8);
        struct in_addr addr{}; addr.s_addr = ip.raw();
        auto* lbl_info = new QLabel(
            QString("<b>%1</b>  <span style='color:#707080;font-size:12px;'>%2</span>")
                .arg(inet_ntoa(addr)).arg(mac_str));
        lbl_info->setTextFormat(Qt::RichText);
        cl->addWidget(lbl_info);
        cards_layout->insertWidget(cards_layout->count() - 1, card);
    }
}
// ═════════════════════════════════════════════════════════════
// Dashboard: main control panel frame
// ═════════════════════════════════════════════════════════════

Dashboard::Dashboard(ShutdownCallback shutdown_callback,
                     DhcpApplyCallback dhcp_apply_callback, QWidget* parent)
    : QMainWindow(parent),
      shutdown_callback_(std::move(shutdown_callback)),
      dhcp_apply_callback_(std::move(dhcp_apply_callback)) {
    setWindowTitle("High-Performance Gaming Traffic Prioritizer");
    setStyleSheet(DARK_STYLESHEET);
    setup_ui();
    run_page_enter_refresh(0);
    plot_timer_id_ = startTimer(16, Qt::PreciseTimer); // ~60Hz: plots, header rates, page data
}

Dashboard::~Dashboard() {
}




void Dashboard::setup_ui() {
    QWidget* central = new QWidget(this);
    setCentralWidget(central);
    statusBar()->hide();

    auto* root_layout = new QVBoxLayout(central);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    // Header bar
    header_ = new QFrame();
    auto* header = header_;
    header->setObjectName("header_frame");
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


    root_layout->addWidget(header);

    // Page stack (full width)
    page_stack = new QStackedWidget();
    page_overview   = new OverviewPage();
    page_qos        = new QosPage();
    page_services   = new ServicePage(dhcp_apply_callback_);
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
    page_stack->addWidget(wrap(page_qos));         // 1
    page_stack->addWidget(wrap(page_services));    // 2
    page_stack->addWidget(wrap(page_devices));     // 3
    root_layout->addWidget(page_stack, 1);

    // Bottom tab bar
    setup_tabbar(root_layout);



}

// Tab button with independently-sized icon and text label.
// Intentionally has no Q_OBJECT - uses only inherited QAbstractButton signals.
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

        // Icon - 30px
        QFont fi = font();
        fi.setPixelSize(30);
        p.setFont(fi);
        p.drawText(QRect(0, 4, width(), 44), Qt::AlignCenter, icon_);

        // Label - 20px
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
        {"⚡",  "QoS",        1},
        {"⚙",  "Services",   2},
        {"▭",  "Devices",    3},
    };

    auto* grp = new QButtonGroup(bar);
    grp->setExclusive(true);

    for (int i = 0; i < 4; ++i) {
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

void Dashboard::run_page_enter_refresh(int page_index) {
    switch (page_index) {
    case 0: break;
    case 1: page_qos->refresh_from_backend(); break;
    case 2: page_services->refresh_status(); break;
    case 3: page_devices->refresh(); break;
    default: break;
    }
}

void Dashboard::on_tab_clicked(int page_index) {
    page_stack->setCurrentIndex(page_index);
    run_page_enter_refresh(page_index);
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

    auto* btn_save   = new QPushButton("Yes, Save Settings");
    auto* btn_nosave = new QPushButton("Yes, Don't Save");
    auto* btn_cancel = new QPushButton("Cancel");
    btn_save->setObjectName("btn_primary");
    btn_nosave->setObjectName("btn_danger");
    lay->addWidget(btn_save);
    lay->addWidget(btn_nosave);
    lay->addWidget(btn_cancel);

    // Save on a worker thread; quit only after save + SAVE_ON_EXIT clear (same order
    // as join-based path) so main()'s tail save_config does not race. GUI thread
    // never blocks on disk I/O or join.
    connect(btn_save, &QPushButton::clicked, &dlg,
        [&dlg, btn_save, btn_nosave, btn_cancel, shutdown_cb = shutdown_callback_]() {
            btn_save->setEnabled(false);
            btn_nosave->setEnabled(false);
            btn_cancel->setEnabled(false);
            QPointer<QDialog> dlg_guard(&dlg);
            auto*             qapp = QApplication::instance();
            std::thread([dlg_guard, qapp, shutdown_cb]() {
                HPGTP::Utils::System::set_current_thread_affinity_control(); // control: cores 0-1
                if (auto sr = Config::save_config("config/config.txt"); !sr)
                    std::println(stderr, "[GUI] {}", sr.error());
                Config::SAVE_ON_EXIT.store(false, std::memory_order_relaxed);
                if (!qapp) return;
                QMetaObject::invokeMethod(qapp, [dlg_guard, shutdown_cb]() {
                    if (dlg_guard) dlg_guard->accept();
                    shutdown_cb();
                }, Qt::QueuedConnection);
            }).detach();
        });
    // Exit without saving
    connect(btn_nosave, &QPushButton::clicked, &dlg,
        [&dlg, shutdown_cb = shutdown_callback_]() {
        Config::SAVE_ON_EXIT.store(false, std::memory_order_relaxed);
        dlg.accept();
        shutdown_cb();
    });
    connect(btn_cancel, &QPushButton::clicked, &dlg, &QDialog::reject);

    dlg.exec();
}


void Dashboard::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
}

void Dashboard::timerEvent(QTimerEvent* event) {
    const int tid = event->timerId();


    // 60Hz data tick: header rates, page plots, counter snapshot
    if (tid != plot_timer_id_) return;

    plot_tick_++;

    // Measure actual elapsed time so rate is correct even if timer fires late
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - plot_last_tick_).count();
    plot_last_tick_ = now;
    // Clamp to [20ms, 200ms]: guards divide-by-zero on first tick and huge
    // spikes if the process was suspended (e.g. SIGSTOP during debug)
    elapsed = std::clamp(elapsed, 0.020, 0.200);

    auto& tel = Telemetry::instance();

    // Header rates (total DL/UL) and class-split Mb for the overview chart
    uint64_t cur_b2 = tel.core_metrics[2].bytes.load(std::memory_order_relaxed);
    uint64_t cur_b3 = tel.core_metrics[3].bytes.load(std::memory_order_relaxed);
    double dl = (cur_b2 - last_bytes[2]) * 8.0 / elapsed / 1e6;
    double ul = (cur_b3 - last_bytes[3]) * 8.0 / elapsed / 1e6;

    // High / Normal split across both data-plane cores (priority buckets 0 and 1)
    const uint64_t cur_high =
        tel.core_metrics[2].prio_bytes[0].load(std::memory_order_relaxed)
      + tel.core_metrics[3].prio_bytes[0].load(std::memory_order_relaxed);
    const uint64_t cur_norm =
        tel.core_metrics[2].prio_bytes[1].load(std::memory_order_relaxed)
      + tel.core_metrics[3].prio_bytes[1].load(std::memory_order_relaxed);
    const double high_mbps = (cur_high - last_high_bytes) * 8.0 / elapsed / 1e6;
    const double normal_mbps = (cur_norm - last_normal_bytes) * 8.0 / elapsed / 1e6;
    last_high_bytes = cur_high;
    last_normal_bytes = cur_norm;

    double t = tel.cpu_temp_celsius.load(std::memory_order_relaxed);
    hdr_info_->setText(QString("↓%1Mb  ↑%2Mb  🌡%3°C")
        .arg(dl, 0, 'f', 1)
        .arg(ul, 0, 'f', 1)
        .arg(t > 0 ? t : 0.0, 0, 'f', 0));

    // Refresh overview plots if visible; update system info every 25 ticks (1s)
    if (page_stack->currentIndex() == 0)
        page_overview->refresh(tel, high_mbps, normal_mbps);
    if (plot_tick_ % 25 == 0)
        page_overview->refresh_info();

    // Refresh device list if visible (checks device_count change internally, cheap if no change)
    if (page_stack->currentIndex() == 3)
        page_devices->refresh();

    // Snapshot current counters for next delta
    for (int i = 0; i < 4; ++i)
        last_bytes[i] = tel.core_metrics[i].bytes.load(std::memory_order_relaxed);
}

} // namespace HPGTP::GUI
