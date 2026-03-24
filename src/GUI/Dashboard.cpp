#include "GUI/Dashboard.hpp"
#include "Config.hpp"
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <thread>
#include <print>

namespace Scalpel::GUI {

    RealTimePlot::RealTimePlot(QWidget* parent) : QWidget(parent) {
        setMinimumSize(400, 200);
        shift_buffer.resize(SHIFT_BUFFER_SIZE, 0.0);
    }

    void RealTimePlot::add_sample(double val) {
        // [准则 #3] 数据与渲染分离：只操作内存缓冲区结构，无阻塞，无重绘
        shift_buffer.pop_front();
        shift_buffer.push_back(val);
        if (val > target_max) {
            target_max = val * 1.2;
        } else {
            // 缓慢衰退 target_max，保持动感
            target_max = std::max(1.0, target_max * 0.99);
        }
    }

    void RealTimePlot::step_physics() {
        // RK4 极简版 (Spring-Damper 阻尼弹簧)
        constexpr double spring_k = 0.15;
        constexpr double damper = 0.1;
        
        double force = spring_k * (target_max - current_max);
        current_velocity = (current_velocity + force) * (1.0 - damper);
        current_max += current_velocity;
        
        if (current_max < 1.0) current_max = 1.0;
    }

    void RealTimePlot::paintEvent(QPaintEvent*) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        // 简版的 Liquid Glass 渐变背景
        QLinearGradient bgGrad(0, 0, 0, height());
        bgGrad.setColorAt(0, QColor(40, 40, 45));
        bgGrad.setColorAt(1, QColor(25, 25, 30));
        painter.fillRect(rect(), bgGrad);

        if (shift_buffer.empty()) return;

        QPainterPath path;
        double x_step = (double)width() / (SHIFT_BUFFER_SIZE - 1);
        double h = height();

        for (int i = 0; i < SHIFT_BUFFER_SIZE; ++i) {
            double y = h - (shift_buffer[i] / (current_max + 1.0) * h * 0.8) - 10;
            if (i == 0) path.moveTo(0, y);
            else path.lineTo(i * x_step, y);
        }

        QPen pen(QColor(0, 150, 255), 2);
        painter.setPen(pen);
        painter.drawPath(path);
    }

    Dashboard::Dashboard(QWidget* parent) : QMainWindow(parent) {
        setWindowTitle("Scalpel Gaming Router Control [Strict Guidelines Edition]");
        setup_ui();
        
        // [准则 #4 & #5] 全局唯一下降刷新管线，25Hz (40ms) 控制屏幕渲染包袱，无数据轮询。
        refresh_timer_id = startTimer(40);
    }

    void Dashboard::setup_ui() {
        QWidget* central = new QWidget(this);
        setCentralWidget(central);
        
        // [准则 #1] 零 XML：完全依赖 QLayout 嵌套生成界面
        QVBoxLayout* main_layout = new QVBoxLayout(central);
        
        QHBoxLayout* plots_layout = new QHBoxLayout();
        pps_plot = new RealTimePlot();
        bps_plot = new RealTimePlot();
        plots_layout->addWidget(pps_plot);
        plots_layout->addWidget(bps_plot);
        
        main_layout->addLayout(plots_layout);
        
        QHBoxLayout* stats_layout = new QHBoxLayout();
        for (int i = 0; i < 4; ++i) {
            core_stats_labels[i] = new QLabel(QString("Core %1: Idle").arg(i));
            core_stats_labels[i]->setStyleSheet("color: white; font-weight: bold; background: rgba(0,0,0,100); padding: 5px;");
            stats_layout->addWidget(core_stats_labels[i]);
        }
        
        btn_toggle_acceleration = new QPushButton("Toggle Acceleration");
        // [准则 #2] 信号与槽的 C++ 类型安全绑定 
        connect(btn_toggle_acceleration, &QPushButton::clicked, this, &Dashboard::on_toggle_acceleration_clicked);
        stats_layout->addWidget(btn_toggle_acceleration);
        
        main_layout->addLayout(stats_layout);
        central->setStyleSheet("background-color: #1a1a1a;");
    }

    void Dashboard::timerEvent(QTimerEvent* event) {
        if (event->timerId() == refresh_timer_id) {
            auto& tel = Telemetry::instance();
            double total_pps = 0;
            double total_bps = 0;
            
            for (int i = 0; i < 4; ++i) {
                uint64_t current_pkts = tel.core_metrics[i].pkts.load(std::memory_order_relaxed);
                uint64_t current_bytes = tel.core_metrics[i].bytes.load(std::memory_order_relaxed);
                
                // 计算 40ms 窗口内的微积分差值 (Delta)
                uint64_t delta_pkts = current_pkts - last_pkts[i];
                uint64_t delta_bytes = current_bytes - last_bytes[i];
                
                // 刷新游标
                last_pkts[i] = current_pkts;
                last_bytes[i] = current_bytes;
                
                // 乘 25 (因为 40ms * 25 = 1000ms) 零开销推导精准每秒速率
                total_pps += (delta_pkts * 25.0);
                total_bps += (delta_bytes * 25.0);
                
                core_stats_labels[i]->setText(QString("C%1: %2 Pkts").arg(i).arg(current_pkts));
            }
            
            pps_plot->add_sample(total_pps);
            bps_plot->add_sample(total_bps);
            
            // 步进物理引擎
            pps_plot->step_physics();
            bps_plot->step_physics();
            
            // 批量应用重塑并更新 UI
            pps_plot->update();
            bps_plot->update();
        }
    }

    void Dashboard::on_toggle_acceleration_clicked() {
        // [准则 #3] 回调非阻塞执行：不可以直接修改重负载网络参数，推入独立事件或使用无锁标记投递给 Core 1
        std::jthread bg_task([](){
            Scalpel::Config::ENABLE_ACCELERATION = !Scalpel::Config::ENABLE_ACCELERATION;
            std::println("[GUI Action] Toggled Acceleration State to {}", Scalpel::Config::ENABLE_ACCELERATION);
        });
        bg_task.detach();
    }
}
