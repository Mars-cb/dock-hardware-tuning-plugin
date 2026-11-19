#include <QtWidgets>
#include <pluginsiteminterface_v2.h>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("dock-hardware-tuning-plugin-preview"));
    if (app.arguments().size() < 2) {
        qCritical("Usage: dock-hardware-tuning-plugin-preview <plugin.so> [screenshot.png]");
        return 2;
    }
    QPluginLoader loader(app.arguments().at(1));
    auto *plugin = qobject_cast<PluginsItemInterfaceV2 *>(loader.instance());
    if (!plugin) {
        qCritical().noquote() << loader.errorString();
        return 1;
    }

    QWidget window;
    window.setWindowTitle(plugin->pluginDisplayName());
    auto *layout = new QVBoxLayout(&window);
    auto *panel = plugin->itemPopupApplet(plugin->pluginName());
    if (!panel)
        return 1;
    layout->addWidget(panel);
    window.show();

    if (app.arguments().size() == 3) {
        QTimer::singleShot(5000, &window, [&] {
            window.hide();
            window.show();
        });
        QTimer::singleShot(7200, &window, [&] {
            bool hardware = false;
            bool monitor = false;
            bool slider = false;
            for (auto *combo : panel->findChildren<QComboBox *>()) {
                hardware |= combo->accessibleName() == QStringLiteral("CPU 调频");
                monitor |= combo->accessibleName().contains(QStringLiteral("输入源"));
            }
            for (auto *control : panel->findChildren<QWidget *>())
                slider |= QByteArray(control->metaObject()->className()).endsWith("::DSlider");
            const bool saved = window.grab().save(app.arguments().at(2));
            app.exit(hardware && monitor && slider && saved ? 0 : 1);
        });
    }
    return app.exec();
}
