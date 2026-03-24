#pragma once
#include <QMainWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <deque>
#include <array>
#include "Telemetry.hpp"

namespace Scalpel::GUI {

    // 准则 #3 & #4: 实时绘图处理必须使用 Shift Buffer 和 40ms 限频，而非即时渲染
    class RealTimePlot : public QWidget {
        Q_OBJECT
    public:
        explicit RealTimePlot(QWidget* parent = nullptr);
        
        // 准则 #3: 仅将样本推入 Shift Buffer，绝对不触发绘图
        void add_sample(double val);

    protected:
        void paintEvent(QPaintEvent* event) override;

    private:
        static constexpr int SHIFT_BUFFER_SIZE = 100;
        std::deque<double> shift_buffer;
        double current_max = 1.0;
    };

    class Dashboard : public QMainWindow {
        Q_OBJECT
    public:
        explicit Dashboard(QWidget* parent = nullptr);

    protected:
        // 准则 #4 & #5: 全局唯一的定时器事件入口，周期为 40ms。仅用于屏幕重绘，绝对不用于采样。
        void timerEvent(QTimerEvent* event) override;

    private:
        // 准则 #1: 强制使用纯 C++ 代码声明布局，禁止 XML
        void setup_ui();
        
        RealTimePlot* pps_plot;
        RealTimePlot* bps_plot;
        QLabel* core_stats_labels[4];
        QPushButton* btn_toggle_acceleration;
        
        int refresh_timer_id;

    private slots:
        // 准则 #2: Qt 原生的 Signal/Slot 回调函数绑定
        void on_toggle_acceleration_clicked();
    };
}
