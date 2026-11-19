#include <QtWidgets>
#include <DSlider>
#include <libudev.h>
#include <pluginsiteminterface_v2.h>

namespace {
const QString Item = QStringLiteral("dock-hardware-tuning-plugin");
const QString Helper = QStringLiteral("/usr/libexec/dock-hardware-tuning-plugin-helper");

struct Description {
    QString title;
    QMap<QString, QString> labels;
    QMap<QString, QString> help;
};

Description describe(const QString &id)
{
    if (id == QStringLiteral("governor")) {
        return {QStringLiteral("CPU 调频"), {}, {
            {"performance", "请求策略允许的最高频率。适合对响应速度敏感的任务，也可能增加功耗。"},
            {"ondemand", "负载上升时快速升频，空闲时降频。"},
            {"schedutil", "根据内核调度器的负载估计动态调整频率。"},
            {"conservative", "逐级调整频率，升降频更平缓。"},
            {"powersave", "采用驱动的省电策略。具体行为取决于 CPU 调频驱动。"},
            {"userspace", "允许其他用户空间程序指定频率。此面板只选择策略。"}
        }};
    }
    if (id == QStringLiteral("boost")) {
        return {"CPU 睿频", {{"1", "允许"}, {"0", "禁用"}}, {
            {"1", "温度和功耗条件允许时，CPU 可超过额定基础频率。"},
            {"0", "禁用 CPU 睿频，有助于降低功耗和温度。"}
        }};
    }
    if (id == QStringLiteral("aspm")) {
        return {"PCIe ASPM", {}, {
            {"default", "使用平台默认策略。本机出现过高性能档死机，建议保留此选项。"},
            {"performance", "减少 PCIe 链路低功耗状态带来的唤醒延迟。若切换后系统失去响应，重启可恢复系统配置。"},
            {"powersave", "允许支持的链路进入节能状态，可能增加设备唤醒延迟。"},
            {"powersupersave", "启用更积极的链路省电策略，支持范围由设备和驱动决定。"}
        }};
    }
    if (id.startsWith(QStringLiteral("scheduler:"))) {
        return {QStringLiteral("磁盘 I/O 调度 · %1").arg(id.section(':', 1)), {}, {
            {"none", "将请求直接交给设备队列，减少主机侧调度开销。"},
            {"mq-deadline", "按期限和顺序安排读写，有助于减少请求长时间等待。"},
            {"bfq", "在进程间分配 I/O 带宽，适合关注交互响应的桌面负载。"},
            {"kyber", "用延迟目标控制请求队列，适合支持多队列的设备。"}
        }};
    }
    const QString pci = id.section(':', 1);
    if (id.startsWith(QStringLiteral("gpu:"))) {
        return {QStringLiteral("AMD GPU 性能 · %1").arg(pci),
                {{"auto", "自动"}, {"low", "低"}, {"high", "高"}}, {
            {"auto", "由驱动按负载调整 GPU 性能档位。"},
            {"low", "请求较低的性能状态，降低功耗，可能影响帧率。"},
            {"high", "请求较高的性能状态，可能增加功耗和温度。"}
        }};
    }
    return {QStringLiteral("GPU 风扇 · %1").arg(pci),
            {{"auto", "自动"}, {"70", "固定 70%"}, {"85", "固定 85%"}, {"100", "固定 100%"}}, {
        {"auto", "由显卡根据温度自动调节转速。"},
        {"70", "固定 PWM 占空比 70%。观察温度，测试完可恢复自动。"},
        {"85", "固定 PWM 占空比 85%。噪音可能上升，转速以实际 RPM 为准。"},
        {"100", "固定 PWM 占空比 100%。适合短时散热测试。"}
    }};
}

QString displayValue(const Description &info, const QString &value)
{
    return info.labels.value(value, value);
}

QString formatMetric(QJsonValue value, double divisor, int decimals, const QString &suffix)
{
    if (!value.isDouble())
        return QStringLiteral("暂无");
    return QString::number(value.toDouble() / divisor, 'f', decimals) + suffix;
}

QString usedTotal(double used, double total)
{
    if (total <= 0 || used < 0 || used > total)
        return QStringLiteral("暂无");
    constexpr double GiB = 1073741824.0;
    return QStringLiteral("%1 / %2 GiB（%3%）")
        .arg(used / GiB, 0, 'f', 1).arg(total / GiB, 0, 'f', 1).arg(100 * used / total, 0, 'f', 0);
}

class Panel : public QWidget {
public:
    Panel()
        : m_process(new QProcess(this)), m_timer(new QTimer(this))
    {
        const auto *screen = QGuiApplication::screenAt(QCursor::pos());
        const auto available = screen ? screen->availableGeometry().size() : QSize(960, 720);
        setFixedWidth(qBound(300, available.width() - 32, 420));
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(14, 11, 14, 11);
        outer->setSpacing(8);
        auto *heading = new QHBoxLayout;
        auto *title = new QLabel(QStringLiteral("硬件面板"));
        auto font = title->font();
        font.setBold(true);
        font.setPointSizeF(font.pointSizeF() + 2);
        title->setFont(font);
        heading->addWidget(title);
        heading->addStretch();
        m_refresh = new QPushButton(QStringLiteral("刷新"));
        heading->addWidget(m_refresh);
        outer->addLayout(heading);
        auto *summary = new QFrame;
        summary->setObjectName(QStringLiteral("hardware-summary"));
        summary->setStyleSheet(QStringLiteral(
            "QFrame#hardware-summary { background: palette(alternate-base); "
            "border: 1px solid palette(midlight); border-radius: 10px; }"));
        auto *summaryLayout = new QVBoxLayout(summary);
        summaryLayout->setContentsMargins(12, 10, 12, 10);
        summaryLayout->setSpacing(7);
        auto *summaryTitle = label("硬件信息");
        auto summaryFont = summaryTitle->font();
        summaryFont.setBold(true);
        summaryTitle->setFont(summaryFont);
        summaryLayout->addWidget(summaryTitle);
        m_metricsScroll = scrollArea();
        auto *metricsContent = new QWidget;
        m_metrics = new QGridLayout(metricsContent);
        m_metrics->setContentsMargins(0, 0, 6, 0);
        m_metrics->setVerticalSpacing(4);
        m_metrics->setHorizontalSpacing(10);
        m_metrics->setColumnStretch(1, 1);
        m_metricsScroll->setWidget(metricsContent);
        metricsContent->setAutoFillBackground(false);
        summaryLayout->addWidget(m_metricsScroll);
        outer->addWidget(summary);
        showMetrics({{"状态", "等待采样…"}});

        auto *panes = new QHBoxLayout;
        panes->setSpacing(9);
        panes->addWidget(makePane("硬件调控", m_rows, m_hardwareScroll), 1);
        auto *divider = new QFrame;
        divider->setFrameShape(QFrame::VLine);
        divider->setForegroundRole(QPalette::Mid);
        panes->addWidget(divider);
        panes->addWidget(makePane("显示器", m_monitorRows, m_monitorScroll), 1);
        m_monitorRows->addWidget(hint("等待首次检测…"));
        outer->addLayout(panes);
        m_status = hint("设置即刻生效。重启或系统重新应用电源模式时，相关设置可能恢复。");
        outer->addWidget(m_status);
        m_monitorRetry.setSingleShot(true);
        m_monitorRetry.setInterval(1600);
        connect(&m_monitorRetry, &QTimer::timeout, this, [this] {
            if (isVisible())
                run(Operation::MonitorList);
        });
        connect(m_refresh, &QPushButton::clicked, this, [this] {
            run(Operation::List);
            refreshMonitors();
        });
        auto *hotplug = new QTimer(this);
        hotplug->setInterval(850);
        hotplug->setSingleShot(true);
        connect(hotplug, &QTimer::timeout, this, [this] {
            if (m_monitorVisited)
                refreshMonitors();
        });
        watchDisplays(hotplug);
        connect(m_timer, &QTimer::timeout, this, [this] {
            run(Operation::Telemetry);
            run(Operation::MonitorInputs);
        });
        connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus status) {
            finish(status == QProcess::NormalExit ? code : -1);
        });
        connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                finish(-1);
        });
    }

    ~Panel() override
    {
        m_timer->stop();
        m_process->disconnect(this);
        if (m_hotplugNotifier)
            m_hotplugNotifier->setEnabled(false);
        if (m_udevMonitor)
            udev_monitor_unref(m_udevMonitor);
        if (m_udev)
            udev_unref(m_udev);
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        m_screen = QGuiApplication::screenAt(QCursor::pos());
        schedulePanelResize();
        m_totalTicks = -1;
        m_diskTime = -1;
        m_diskCounters.clear();
        m_timer->start(10000);
        run(m_rows->count() == 0 ? Operation::List : Operation::Telemetry);
        if (!m_monitorsLoaded || m_monitorScanFailed)
            refreshMonitors();
        else
            run(Operation::MonitorInputs);
    }

    void hideEvent(QHideEvent *event) override
    {
        m_timer->stop();
        QWidget::hideEvent(event);
    }

private:
    enum class Operation { None, List, MonitorList, MonitorInputs, Telemetry, Apply };

    static QLabel *label(const QString &text)
    {
        auto *result = new QLabel(text);
        result->setTextFormat(Qt::PlainText);
        result->setWordWrap(true);
        return result;
    }

    static QLabel *hint(const QString &text)
    {
        auto *result = label(text);
        auto font = result->font();
        if (font.pointSizeF() > 9)
            font.setPointSizeF(qMax(9.0, font.pointSizeF() - 1.5));
        result->setFont(font);
        auto palette = result->palette();
        const auto foreground = palette.color(QPalette::WindowText);
        const auto background = palette.color(QPalette::Window);
        const auto mix = [](int fg, int bg) { return qRound(0.73 * fg + 0.27 * bg); };
        palette.setColor(QPalette::WindowText, QColor(mix(foreground.red(), background.red()),
                                                       mix(foreground.green(), background.green()),
                                                       mix(foreground.blue(), background.blue())));
        result->setPalette(palette);
        return result;
    }

    static QScrollArea *scrollArea()
    {
        auto *scroll = new QScrollArea;
        scroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setAutoFillBackground(false);
        scroll->viewport()->setAutoFillBackground(false);
        scroll->setFixedHeight(40);
        return scroll;
    }

    static QWidget *makePane(const QString &title, QVBoxLayout *&rows, QScrollArea *&scroll)
    {
        auto *pane = new QWidget;
        auto *layout = new QVBoxLayout(pane);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        auto *heading = label(title);
        auto font = heading->font();
        font.setBold(true);
        heading->setFont(font);
        layout->addWidget(heading);
        scroll = scrollArea();
        auto *content = new QWidget;
        rows = new QVBoxLayout(content);
        rows->setContentsMargins(1, 0, 7, 1);
        rows->setSpacing(5);
        rows->setAlignment(Qt::AlignTop);
        scroll->setWidget(content);
        content->setAutoFillBackground(false);
        layout->addWidget(scroll);
        return pane;
    }

    void showPageStatus()
    {
        m_status->setText(QStringLiteral("设置即时生效。输入源定时读回，插拔时重新检测显示器。"));
    }

    void schedulePanelResize()
    {
        if (m_resizePending)
            return;
        m_resizePending = true;
        QTimer::singleShot(0, this, [this] {
            m_resizePending = false;
            if (!isVisible())
                return;
            auto *popup = window();
            const auto *screen = m_screen ? m_screen.data() : QGuiApplication::primaryScreen();
            if (!screen)
                return;

            const QRect area = screen->availableGeometry().adjusted(10, 10, -10, -10);
            const int maxHeight = qMax(1, qMin(560, area.height() * 4 / 5));
            const int panelWidth = qMax(1, qMin(420, area.width()));
            setFixedWidth(panelWidth);
            auto *outer = layout();
            outer->invalidate();
            const int measured = outer->hasHeightForWidth()
                ? outer->totalHeightForWidth(panelWidth) : outer->sizeHint().height();
            const int chrome = measured - m_metricsScroll->minimumHeight() - m_hardwareScroll->minimumHeight();
            const int budget = qMax(0, maxHeight - chrome);
            const int metricsWidth = qMax(1, panelWidth - 64);
            const int metricsHeight = m_metrics->hasHeightForWidth()
                ? m_metrics->heightForWidth(metricsWidth) : m_metrics->sizeHint().height();
            const int summaryHeight = qBound(0, metricsHeight, budget * 2 / 5);
            const int rowsHeight = qMax(m_rows->sizeHint().height(), m_monitorRows->sizeHint().height());
            m_metricsScroll->setFixedHeight(summaryHeight);
            const int controlsHeight = qBound(0, rowsHeight, budget - summaryHeight);
            m_hardwareScroll->setFixedHeight(controlsHeight);
            m_monitorScroll->setFixedHeight(controlsHeight);
            const QSize target(panelWidth, qBound(1, chrome + summaryHeight + controlsHeight, maxHeight));

            if (popup == this) {
                resize(target);
                return;
            }
            const QSize previous = popup->size();
            QPoint position = popup->pos();
            const auto dockSide = qApp->property(PROP_POSITION).value<Dock::Position>();
            if (dockSide == Dock::Bottom)
                position.ry() += previous.height() - target.height();
            else if (dockSide == Dock::Left || dockSide == Dock::Right)
                position.ry() += (previous.height() - target.height()) / 2;
            if (dockSide == Dock::Right)
                position.rx() += previous.width() - target.width();
            position.setX(qBound(area.left(), position.x(), area.left() + area.width() - target.width()));
            position.setY(qBound(area.top(), position.y(), area.top() + area.height() - target.height()));

            resize(target);
            if (previous != target)
                popup->setFixedSize(target);
            if (popup->pos() != position)
                popup->move(position);
        });
    }

    void watchDisplays(QTimer *debounce)
    {
        m_udev = udev_new();
        if (m_udev)
            m_udevMonitor = udev_monitor_new_from_netlink(m_udev, "udev");
        if (!m_udevMonitor
                || udev_monitor_filter_add_match_subsystem_devtype(m_udevMonitor, "drm", nullptr) < 0
                || udev_monitor_enable_receiving(m_udevMonitor) < 0) {
            connect(qApp, &QGuiApplication::screenAdded, debounce, [debounce](QScreen *) { debounce->start(); });
            connect(qApp, &QGuiApplication::screenRemoved, debounce, [debounce](QScreen *) { debounce->start(); });
            return;
        }
        m_hotplugNotifier = new QSocketNotifier(udev_monitor_get_fd(m_udevMonitor), QSocketNotifier::Read, this);
        connect(m_hotplugNotifier, &QSocketNotifier::activated, this, [this, debounce]() {
            while (auto *event = udev_monitor_receive_device(m_udevMonitor)) {
                const QByteArray action = udev_device_get_action(event);
                const QByteArray hotplug = udev_device_get_property_value(event, "HOTPLUG");
                if (action == "add" || action == "remove" || hotplug == "1")
                    debounce->start();
                udev_device_unref(event);
            }
        });
    }

    void refreshMonitors()
    {
        m_monitorRetry.stop();
        m_monitorRetries = 2;
        run(Operation::MonitorList);
    }

    QStringList monitorInputIds() const
    {
        QStringList ids;
        for (int i = 0; i < m_monitorRows->count(); ++i) {
            auto *row = m_monitorRows->itemAt(i)->widget();
            if (row && row->objectName().startsWith("monitor:input:"))
                ids.append(row->objectName());
        }
        return ids;
    }

    void run(Operation operation, const QString &id = {}, const QString &value = {})
    {
        if (m_operation != Operation::None) {
            if (operation == Operation::Apply) {
                m_pendingValues.insert(id, value);
            } else if (operation == Operation::MonitorList) {
                m_monitorRefreshPending = true;
            } else if (operation == Operation::MonitorInputs) {
                m_inputRefreshPending = true;
            } else if (operation == Operation::List) {
                m_refreshPending = true;
            }
            return;
        }
        if (operation == Operation::MonitorInputs) {
            const auto ids = monitorInputIds();
            if (!ids.isEmpty()) {
                m_operation = operation;
                m_process->start(Helper, QStringList{QStringLiteral("monitor-inputs")} + ids);
            }
            return;
        }
        m_operation = operation;
        if (operation == Operation::Apply) {
            m_activeId = id;
            m_activeValue = value;
            const bool monitorWrite = id.startsWith("monitor:");
            m_status->setText(monitorWrite ? "正在向显示器发送设置…" : "正在应用硬件设置…");
            m_process->start(Helper, {monitorWrite ? "monitor-set" : "request", id, value});
            return;
        }
        QString program = Helper;
        if (qApp->applicationName() == QStringLiteral("dock-hardware-tuning-plugin-preview"))
            program = qEnvironmentVariable("DDE_HARDWARE_TUNING_PREVIEW_HELPER", Helper);
        if (operation == Operation::MonitorList) {
            m_monitorVisited = true;
            m_status->setText(m_monitorsLoaded ? "正在刷新显示器，保留上次读数…" : "正在检测显示器…");
        }
        m_process->start(program, {operation == Operation::Telemetry ? "telemetry"
                                     : operation == Operation::MonitorList ? "monitor-list" : "list"});
    }

    void runPending()
    {
        if (!m_pendingValues.isEmpty()) {
            auto it = m_pendingValues.begin();
            const auto id = it.key();
            const auto value = it.value();
            m_pendingValues.erase(it);
            run(Operation::Apply, id, value);
        } else if (m_monitorRefreshPending) {
            m_monitorRefreshPending = false;
            run(Operation::MonitorList);
        } else if (m_refreshPending) {
            m_refreshPending = false;
            run(Operation::List);
        } else if (m_inputRefreshPending) {
            m_inputRefreshPending = false;
            if (isVisible())
                run(Operation::MonitorInputs);
        }
    }

    void finish(int code)
    {
        const auto operation = m_operation;
        m_operation = Operation::None;
        const auto output = m_process->readAllStandardOutput();
        const auto error = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
        const auto failure = error.isEmpty() ? m_process->errorString() : error;
        const auto json = QJsonDocument::fromJson(output);
        if (operation == Operation::Apply) {
            const bool monitor = m_activeId.startsWith("monitor:");
            const bool input = m_activeId.startsWith("monitor:input:");
            if (code == 0) {
                const auto actual = monitor ? m_activeValue : QString::fromUtf8(output).trimmed();
                const bool newerValueQueued = m_pendingValues.contains(m_activeId);
                if (newerValueQueued && !input) {
                    if (auto *row = findRow(m_activeId))
                        row->setProperty("current", actual);
                } else if (!input) {
                    updateControl(m_activeId, actual);
                } else if (!newerValueQueued) {
                    updateControl(m_activeId, {});
                }
                if (input) {
                    QTimer::singleShot(1200, this, [this] {
                        if (isVisible())
                            run(Operation::MonitorInputs);
                    });
                }
                m_status->setText(input ? "切换指令已发送，等待读回。若失去画面，可用显示器按键切回。"
                    : matches(m_activeId, m_activeValue, actual) ? "设置已确认。"
                    : "读回值与请求不同，已显示实际值。");
            } else {
                if (!m_pendingValues.contains(m_activeId)) {
                    if (auto *row = findRow(m_activeId)) {
                        auto *slider = row->findChild<Dtk::Widget::DSlider *>();
                        if (!slider || !slider->slider()->isSliderDown())
                            updateControl(m_activeId, row->property("current").toString());
                    }
                }
                m_status->setText(QStringLiteral("设置失败：%1").arg(failure));
            }
        } else if (operation == Operation::List || operation == Operation::MonitorList) {
            if (code == 0 && json.isArray()) {
                const bool monitors = operation == Operation::MonitorList;
                renderRows(json.array(), monitors);
                if (monitors) {
                    m_monitorsLoaded = true;
                    m_monitorScanFailed = false;
                    m_monitorRetry.stop();
                    m_inputRefreshPending = true;
                }
                showPageStatus();
            } else if (operation == Operation::MonitorList) {
                m_monitorScanFailed = true;
                for (const auto &id : monitorInputIds()) {
                    if (!m_pendingValues.contains(id))
                        updateControl(id, {});
                }
                m_inputRefreshPending = true;
                if (isVisible() && m_monitorRetries > 0) {
                    --m_monitorRetries;
                    m_monitorRetry.start();
                }
                m_status->setText(QStringLiteral("显示器扫描暂未完成，等待读回输入源。"));
                qWarning().noquote() << "Hardware plugin display scan:" << failure;
            } else {
                m_status->setText(QStringLiteral("硬件列表刷新失败，保留上次结果。%1").arg(failure));
            }
        } else if (operation == Operation::MonitorInputs) {
            const auto readings = code == 0 ? json.object() : QJsonObject{};
            bool allConfirmed = true;
            for (const auto &id : monitorInputIds()) {
                const auto actual = readings.value(id).toString();
                allConfirmed = allConfirmed && !actual.isEmpty();
                if (m_pendingValues.contains(id))
                    continue;
                if (auto *row = findRow(id)) {
                    if (row->property("current").toString() != actual)
                        updateControl(id, actual);
                }
            }
            if (!allConfirmed)
                m_status->setText(QStringLiteral("部分显示器输入源暂未确认，稍后会自动重读。"));
            else if (m_monitorScanFailed)
                m_status->setText(QStringLiteral("输入源已读回。其他显示器设置沿用上次列表。"));
            else if (m_status->text().startsWith(QStringLiteral("切换指令已发送"))
                     || m_status->text().startsWith(QStringLiteral("部分显示器输入源")))
                showPageStatus();
        } else if (operation == Operation::Telemetry) {
            if (code == 0 && json.isObject())
                renderTelemetry(json.object());
            else
                showMetrics({{"状态", "传感器读数暂不可用"}});
        }
        schedulePanelResize();
        runPending();
        if (m_operation == Operation::None && isVisible()
                && (operation == Operation::List || operation == Operation::MonitorList))
            run(Operation::Telemetry);
    }

    static Description monitorDescription(const QString &feature, const QString &title,
                                          int maximum, const QJsonArray &options, const QString &current)
    {
        Description info{title, {}, {}};
        if (feature == "input") {
            info.labels = {{"1", "VGA 1"}, {"2", "VGA 2"}, {"3", "DVI 1"}, {"4", "DVI 2"},
                           {"15", "DisplayPort 1"}, {"16", "DisplayPort 2"},
                           {"17", "HDMI 1"}, {"18", "HDMI 2"}};
            const auto fallback = [](const QString &value) {
                return QStringLiteral("VCP 0x%1").arg(value.toInt(), 2, 16, QLatin1Char('0'));
            };
            if (!current.isEmpty() && !info.labels.contains(current))
                info.labels.insert(current, fallback(current));
            for (const auto &option : options) {
                const auto value = option.toString();
                if (!info.labels.contains(value))
                    info.labels.insert(value, fallback(value));
                info.help.insert(value, "切换显示器的硬件输入。当前电脑可能失去该屏幕画面，必要时使用显示器物理按键切回。");
            }
        } else {
            for (const auto &option : options) {
                const auto value = option.toString();
                if (maximum > 0)
                    info.labels.insert(value, QString::number(qRound(100.0 * value.toInt() / maximum)) + "%");
                info.help.insert(value, feature == "brightness"
                    ? "调节显示器自身的亮度，通过 DDC/CI 写入。"
                    : "调节显示器自身的对比度，影响亮部和暗部的区分。");
            }
        }
        return info;
    }

    void renderRows(const QJsonArray &controls, bool monitors)
    {
        auto *rows = monitors ? m_monitorRows : m_rows;
        while (auto *item = rows->takeAt(0)) {
            item->widget()->hide();
            item->widget()->deleteLater();
            delete item;
        }
        if (controls.isEmpty())
            rows->addWidget(label(monitors
                                       ? "暂无可用的显示器设置。" : "当前驱动未提供可调整的项目。"));
        QString previousMonitor;
        for (const auto &entry : controls) {
            const auto control = entry.toObject();
            const auto id = control.value("id").toString();
            const auto current = control.value("current").toString();
            const auto title = control.value("title").toString();
            if (id.isEmpty()) {
                rows->addWidget(label(title + "\n" + control.value("detail").toString()));
                continue;
            }
            const auto maximum = control.value("maximum").toInt();
            const auto info = id.startsWith("monitor:")
                ? monitorDescription(id.section(':', 1, 1), title, maximum,
                                     control.value("options").toArray(), current)
                : describe(id);
            if (monitors) {
                const auto monitorName = title.section(QStringLiteral(" · "), 0, 0);
                if (monitorName != previousMonitor) {
                    auto *heading = label(monitorName);
                    auto font = heading->font();
                    font.setBold(true);
                    heading->setFont(font);
                    heading->setContentsMargins(0, previousMonitor.isEmpty() ? 0 : 6, 0, 1);
                    heading->setToolTip(title.section(QStringLiteral(" · "), 0, -2));
                    rows->addWidget(heading);
                    previousMonitor = monitorName;
                }
            }
            auto *box = new QWidget;
            box->setObjectName(id);
            box->setProperty("current", current);
            const bool brightness = id.startsWith("monitor:brightness:") && maximum > 0;
            QBoxLayout *layout = brightness ? static_cast<QBoxLayout *>(new QVBoxLayout(box))
                                            : static_cast<QBoxLayout *>(new QHBoxLayout(box));
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(4);
            const auto shortTitle = monitors ? info.title.section(QStringLiteral(" · "), -1)
                                             : info.title.section(QStringLiteral(" · "), 0, 0);
            auto *name = label(shortTitle);
            name->setToolTip(info.title);
            if (!brightness)
                name->setMinimumWidth(monitors ? 48 : 62);
            layout->addWidget(name, brightness ? 0 : 1);
            const bool readOnly = qApp->applicationName() == QStringLiteral("dock-hardware-tuning-plugin-preview");
            if (brightness) {
                auto *line = new QHBoxLayout;
                auto *slider = new Dtk::Widget::DSlider(Qt::Horizontal);
                slider->setAccessibleName(info.title);
                slider->setMinimum(1);
                slider->setMaximum(maximum);
                slider->setValue(qBound(1, current.toInt(), maximum));
                slider->setToolTip("拖动即可实时调节显示器自身亮度。");
                auto *percent = label({});
                percent->setObjectName("slider-percent");
                percent->setMinimumWidth(36);
                percent->setAlignment(Qt::AlignCenter);
                line->addWidget(slider, 1);
                line->addWidget(percent);
                layout->addLayout(line);
                slider->slider()->setTracking(true);
                connect(slider, &Dtk::Widget::DSlider::valueChanged, box, [this, percent, maximum, id, readOnly](int value) {
                    percent->setText(QString::number(qRound(100.0 * value / maximum)) + "%");
                    if (!readOnly)
                        run(Operation::Apply, id, QString::number(value));
                });
                percent->setText(QString::number(qRound(100.0 * current.toInt() / maximum)) + "%");
            } else {
                auto *combo = new QComboBox;
                combo->setAccessibleName(info.title);
                combo->view()->setMouseTracking(true);
                for (const auto &option : control.value("options").toArray()) {
                    const auto value = option.toString();
                    combo->addItem(displayValue(info, value), value);
                    combo->setItemData(combo->count() - 1, info.help.value(value), Qt::ToolTipRole);
                }
                combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
                combo->setMinimumContentsLength(3);
                combo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
                layout->addWidget(combo, monitors ? 2 : 1);
                syncCombo(combo, id, current);
                connect(combo, QOverload<int>::of(&QComboBox::activated), box, [this, combo, id, box, readOnly](int index) {
                    const auto value = combo->itemData(index).toString();
                    const auto actual = box->property("current").toString();
                    if (readOnly || value.isEmpty() || matches(id, value, actual)) {
                        syncCombo(combo, id, actual);
                        return;
                    }
                    run(Operation::Apply, id, value);
                });
            }
            rows->addWidget(box);
        }
    }

    static bool matches(const QString &id, const QString &requested, const QString &actual)
    {
        return requested == actual || (id.startsWith("fan:") && requested.toInt() >= 70
                                        && actual.toInt() > 0 && qAbs(requested.toInt() - actual.toInt()) <= 3);
    }

    static void syncCombo(QComboBox *combo, const QString &id, const QString &actual)
    {
        const QSignalBlocker blocker(combo);
        constexpr int Transient = Qt::UserRole + 1;
        constexpr int Original = Qt::UserRole + 2;
        int selected = -1;
        for (int i = combo->count() - 1; i >= 0; --i) {
            if (combo->itemData(i, Transient).toBool())
                combo->removeItem(i);
            else if (combo->itemData(i, Original).isValid())
                combo->setItemText(i, combo->itemData(i, Original).toString());
        }
        for (int i = 0; i < combo->count(); ++i) {
            const auto value = combo->itemData(i).toString();
            if (!matches(id, value, actual))
                continue;
            selected = i;
            if (value != actual) {
                combo->setItemData(i, combo->itemText(i), Original);
                combo->setItemText(i, QStringLiteral("手动 %1%（%2 档）").arg(actual, value));
            }
            break;
        }
        if (selected < 0) {
            const auto label = actual.isEmpty() ? (id.startsWith("monitor:input:")
                ? QStringLiteral("输入源未确认") : QStringLiteral("当前值暂无"))
                : id.startsWith("fan:") && actual.toInt() > 0 ? QStringLiteral("手动 %1%").arg(actual)
                : actual;
            combo->insertItem(0, label, actual);
            combo->setItemData(0, true, Transient);
            selected = 0;
        }
        combo->setCurrentIndex(selected);
        combo->setToolTip(combo->itemData(selected, Qt::ToolTipRole).toString());
    }

    QWidget *findRow(const QString &id)
    {
        auto *rows = id.startsWith("monitor:") ? m_monitorRows : m_rows;
        for (int index = 0; index < rows->count(); ++index) {
            auto *row = rows->itemAt(index)->widget();
            if (row && row->objectName() == id)
                return row;
        }
        return nullptr;
    }

    void updateControl(const QString &id, const QString &actual)
    {
        auto *row = findRow(id);
        if (!row)
            return;
        row->setProperty("current", actual);
        if (auto *combo = row->findChild<QComboBox *>()) {
            syncCombo(combo, id, actual);
        } else if (auto *slider = row->findChild<Dtk::Widget::DSlider *>()) {
            const QSignalBlocker blocker(slider);
            slider->setValue(qBound(slider->minimum(), actual.toInt(), slider->maximum()));
            if (auto *percent = row->findChild<QLabel *>("slider-percent"))
                percent->setText(QString::number(qRound(100.0 * actual.toInt() / slider->maximum())) + "%");
        }
    }

    struct MetricRow {
        QString key;
        QString value;
        QString emphasis = {};
        int severity = 0;
    };
    using MetricRows = QList<MetricRow>;

    static int capacityRisk(double used, double total, double near)
    {
        if (total <= 0 || used < 0 || used > total)
            return 0;
        const double fraction = used / total;
        return fraction >= 0.95 ? 2 : fraction >= near ? 1 : 0;
    }

    void showMetrics(const MetricRows &values)
    {
        if (m_metricKeys.size() != values.size()) {
            for (auto *widget : m_metricKeys)
                delete widget;
            for (auto *widget : m_metricValues)
                delete widget;
            m_metricKeys.clear();
            m_metricValues.clear();
            for (int row = 0; row < values.size(); ++row) {
                auto *key = hint({});
                auto *value = label({});
                key->setAlignment(Qt::AlignLeft | Qt::AlignTop);
                value->setAlignment(Qt::AlignLeft | Qt::AlignTop);
                m_metrics->addWidget(key, row, 0);
                m_metrics->addWidget(value, row, 1);
                m_metricKeys.append(key);
                m_metricValues.append(value);
            }
        }
        for (int row = 0; row < values.size(); ++row) {
            const auto &entry = values.at(row);
            if (m_metricKeys.at(row)->text() != entry.key)
                m_metricKeys.at(row)->setText(entry.key);
            auto *value = m_metricValues.at(row);
            if (entry.severity > 0) {
                const bool dark = value->palette().color(QPalette::Window).lightness() < 128;
                const auto color = entry.severity == 2
                    ? (dark ? "#ff7b72" : "#ba1a1a") : (dark ? "#ffd15c" : "#8a6100");
                const auto emphasized = (entry.emphasis.isEmpty() ? entry.value : entry.emphasis).toHtmlEscaped();
                auto html = entry.value.toHtmlEscaped();
                html.replace(emphasized, QStringLiteral("<span style=\"color:%1\">%2</span>").arg(color, emphasized));
                value->setTextFormat(Qt::RichText);
                if (value->text() != html)
                    value->setText(html);
            } else {
                value->setTextFormat(Qt::PlainText);
                if (value->text() != entry.value)
                    value->setText(entry.value);
            }
        }
    }

    void renderTelemetry(const QJsonObject &data)
    {
        MetricRows lines;
        const auto total = data.value("totalTicks").toDouble(-1);
        const auto idle = data.value("idleTicks").toDouble(-1);
        QString usage = QStringLiteral("采样中");
        if (m_totalTicks >= 0 && m_idleTicks >= 0 && total > m_totalTicks && idle >= m_idleTicks) {
            const auto busy = 1.0 - (idle - m_idleTicks) / (total - m_totalTicks);
            usage = QString::number(qBound(0.0, busy, 1.0) * 100, 'f', 0) + "%";
        }
        m_totalTicks = total;
        m_idleTicks = idle;

        QStringList temperatures;
        const auto cpu = data.value("cpu").toObject();
        for (auto it = cpu.constBegin(); it != cpu.constEnd(); ++it)
            temperatures << formatMetric(it.value(), 1000, 0, " °C");
        const auto cpuTemp = temperatures.isEmpty() ? QStringLiteral("温度暂无") : temperatures.join(" / ");
        lines.append({"CPU", QStringLiteral("%1 · %2 · %3").arg(
            usage, cpuTemp, formatMetric(data.value("cpuPeakKHz"), 1000, 0, " MHz")),
            cpuTemp, data.value("cpuRisk").toInt()});

        const auto memory = data.value("memoryTotal").toDouble(-1);
        const auto available = data.value("memoryAvailable").toDouble(-1);
        const double used = available < 0 ? -1 : memory - available;
        lines.append({"内存", usedTotal(used, memory), {}, capacityRisk(used, memory, 0.85)});
        for (const auto &value : data.value("gpu").toArray()) {
            const auto gpu = value.toObject();
            const auto fan = gpu.value("fan").toString();
            if (!fan.isEmpty()) {
                const auto id = QStringLiteral("fan:") + gpu.value("id").toString();
                auto *row = findRow(id);
                if (row && row->property("current").toString() != fan)
                    updateControl(id, fan);
            }
            const auto gpuTemp = formatMetric(gpu.value("temperature"), 1000, 0, " °C");
            lines.append({"GPU", QStringLiteral("%1 · %2 · %3").arg(
                formatMetric(gpu.value("busy"), 1, 0, "%"), gpuTemp,
                formatMetric(gpu.value("rpm"), 1, 0, " RPM")), gpuTemp, gpu.value("risk").toInt()});
            lines.append({"显存", QStringLiteral("%1 · 控制器 %2").arg(
                usedTotal(gpu.value("vramUsed").toDouble(-1), gpu.value("vramTotal").toDouble(-1)),
                formatMetric(gpu.value("memoryBusy"), 1, 0, "%")), {},
                capacityRisk(gpu.value("vramUsed").toDouble(-1), gpu.value("vramTotal").toDouble(-1), 0.85)});
            lines.append({"GPU 频率", QStringLiteral("核心 %1 · 显存 %2 · %3").arg(
                formatMetric(gpu.value("coreMHz"), 1, 0, " MHz"), formatMetric(gpu.value("memoryMHz"), 1, 0, " MHz"),
                formatMetric(gpu.value("power"), 1000000, 1, " W"))});
        }
        const auto volumes = data.value("volumes").toObject();
        for (auto it = volumes.constBegin(); it != volumes.constEnd(); ++it) {
            const auto volume = it.value().toObject();
            const auto used = volume.value("used").toDouble(-1);
            const auto total = volume.value("total").toDouble(-1);
            lines.append({it.key() == "/" ? "系统卷" : "数据卷", usedTotal(used, total), {},
                          capacityRisk(used, total, 0.90)});
        }
        const auto now = data.value("time").toDouble(-1);
        QMap<QString, QPair<double, double>> counters;
        for (const auto &value : data.value("disks").toArray()) {
            const auto disk = value.toObject();
            const auto name = disk.value("id").toString();
            const auto rd = disk.value("read").toDouble(-1);
            const auto wr = disk.value("write").toDouble(-1);
            QString speed = QStringLiteral("读写采样中");
            const auto prior = m_diskCounters.constFind(name);
            if (prior != m_diskCounters.constEnd() && m_diskTime >= 0 && now > m_diskTime
                    && rd >= prior->first && wr >= prior->second) {
                const double scale = (now - m_diskTime) * 1048576;
                speed = QStringLiteral("读 %1 · 写 %2 MiB/s")
                    .arg((rd - prior->first) / scale, 0, 'f', 1)
                    .arg((wr - prior->second) / scale, 0, 'f', 1);
            }
            counters.insert(name, qMakePair(rd, wr));
            auto line = speed;
            if (disk.value("temperature").isDouble())
                line += QStringLiteral(" · ") + formatMetric(disk.value("temperature"), 1000, 0, " °C");
            lines.append({name, line});
        }
        m_diskTime = now;
        m_diskCounters = counters;
        showMetrics(lines);
    }

    QProcess *m_process;
    QTimer *m_timer;
    QPushButton *m_refresh = nullptr;
    QLabel *m_status = nullptr;
    QGridLayout *m_metrics = nullptr;
    QList<QLabel *> m_metricKeys;
    QList<QLabel *> m_metricValues;
    QVBoxLayout *m_rows = nullptr;
    QVBoxLayout *m_monitorRows = nullptr;
    QScrollArea *m_hardwareScroll = nullptr;
    QScrollArea *m_monitorScroll = nullptr;
    QScrollArea *m_metricsScroll = nullptr;
    QPointer<QScreen> m_screen;
    bool m_resizePending = false;
    Operation m_operation = Operation::None;
    QTimer m_monitorRetry;
    int m_monitorRetries = 0;
    bool m_monitorScanFailed = false;
    bool m_monitorsLoaded = false;
    bool m_monitorVisited = false;
    bool m_monitorRefreshPending = false;
    udev *m_udev = nullptr;
    udev_monitor *m_udevMonitor = nullptr;
    QSocketNotifier *m_hotplugNotifier = nullptr;
    QString m_activeId;
    QString m_activeValue;
    bool m_refreshPending = false;
    bool m_inputRefreshPending = false;
    QMap<QString, QString> m_pendingValues;
    double m_totalTicks = -1;
    double m_idleTicks = -1;
    double m_diskTime = -1;
    QMap<QString, QPair<double, double>> m_diskCounters;
};
}

class HardwarePlugin : public QObject, public PluginsItemInterfaceV2 {
    Q_OBJECT
    Q_INTERFACES(PluginsItemInterfaceV2)
    Q_PLUGIN_METADATA(IID ModuleInterface_iid_V2 FILE "dock.json")

public:
    ~HardwarePlugin() override
    {
        if (!qApp || QCoreApplication::closingDown())
            return;
        delete m_panel;
        delete m_icon;
        delete m_tip;
    }

    const QString pluginName() const override { return Item; }
    const QString pluginDisplayName() const override { return QStringLiteral("硬件调控"); }
    Dock::PluginFlags flags() const override { return Dock::Type_System | Dock::Attribute_Normal; }
    QIcon icon(Dock::IconType, Dock::ThemeType) const override { return hardwareIcon(); }

    void init(PluginProxyInterface *proxy) override
    {
        if (m_proxyInter)
            return;
        m_proxyInter = proxy;
        m_icon = new QLabel;
        m_icon->setAlignment(Qt::AlignCenter);
        m_icon->setPixmap(hardwareIcon().pixmap(26, 26));
        m_icon->setAccessibleName(pluginDisplayName());
        m_tip = new QLabel(QStringLiteral("硬件调控 · CPU、GPU 和 PCIe"));
        if (!pluginIsDisable())
            m_proxyInter->itemAdded(this, Item);
    }

    bool pluginIsAllowDisable() override { return true; }
    bool pluginIsDisable() override
    {
        return m_proxyInter && m_proxyInter->getValue(this, "disabled", false).toBool();
    }
    void pluginStateSwitched() override
    {
        if (!m_proxyInter)
            return;
        const bool disabled = !pluginIsDisable();
        m_proxyInter->saveValue(this, "disabled", disabled);
        if (disabled)
            m_proxyInter->itemRemoved(this, Item);
        else
            m_proxyInter->itemAdded(this, Item);
    }
    int itemSortKey(const QString &) override
    {
        return m_proxyInter ? m_proxyInter->getValue(this, "sort-key", 8).toInt() : 8;
    }
    void setSortKey(const QString &, int order) override
    {
        if (m_proxyInter)
            m_proxyInter->saveValue(this, "sort-key", order);
    }

    QWidget *itemWidget(const QString &key) override { return key == Item ? m_icon.data() : nullptr; }
    QWidget *itemTipsWidget(const QString &key) override { return key == Item ? m_tip.data() : nullptr; }
    QWidget *itemPopupApplet(const QString &key) override
    {
        if (key != Item)
            return nullptr;
        if (!m_panel)
            m_panel = new Panel;
        return m_panel;
    }

private:
    static QIcon hardwareIcon() { return QIcon(QStringLiteral(":/dock-hardware-tuning-plugin/hardware.svg")); }

    QPointer<Panel> m_panel;
    QPointer<QLabel> m_icon;
    QPointer<QLabel> m_tip;
};

#include "plugin.moc"
