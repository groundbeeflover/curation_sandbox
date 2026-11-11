// SPDX-License-Identifier: Apache-2.0
// qt-vulkan-spike: a QMainWindow with a dock and a Vulkan viewport drawing a rotating triangle.
//
//   qt-vulkan-spike                      interactive: right-click the viewport to walk, Esc leaves
//   qt-vulkan-spike --shot out.png       composite screenshot after --after-ms, then quit
//   qt-vulkan-spike --resize-cycle ...   resize the window through four sizes first
//   qt-vulkan-spike --walk ...           enter relative mouse mode at start
//   qt-vulkan-spike --quit-after 8000    quit after 8 s (prints the status line)
//   qt-vulkan-spike --niri-shot out.png  also ask the niri compositor for a window screenshot
//   qt-vulkan-spike --headless           offscreen renderer and a plain widget (as on CI)
#include "main_window.hpp"
#include "viewport.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>

#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

namespace {

void print_status(spike::MainWindow& w, const char* when) {
    std::printf("[%s] %s\n", when, qPrintable(w.status_line()));
    std::fflush(stdout);
}

// Screenshot of our own top-level window as the compositor shows it (Linux with niri only).
void niri_screenshot(const QString& path, const QString& title) {
    QProcess list;
    list.start(QStringLiteral("niri"),
               {QStringLiteral("msg"), QStringLiteral("-j"), QStringLiteral("windows")});
    if (!list.waitForFinished(3000)) {
        std::fprintf(stderr, "niri is not available\n");
        return;
    }
    const QJsonArray windows = QJsonDocument::fromJson(list.readAllStandardOutput()).array();
    for (const QJsonValue& v : windows) {
        const QJsonObject o = v.toObject();
        // X11 windows belong to xwayland-satellite's pid, so the title matches too.
        if (o.value(QStringLiteral("pid")).toInteger() == QCoreApplication::applicationPid() ||
            o.value(QStringLiteral("title")).toString() == title) {
            const QString id = QString::number(o.value(QStringLiteral("id")).toInteger());
            QProcess::execute(QStringLiteral("niri"),
                              {QStringLiteral("msg"), QStringLiteral("action"),
                               QStringLiteral("screenshot-window"), QStringLiteral("--id"), id,
                               QStringLiteral("--path"), path});
            std::printf("[niri] window %s -> %s\n", qPrintable(id), qPrintable(path));
            return;
        }
    }
    std::fprintf(stderr, "niri: our window was not found\n");
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("qt-vulkan-spike"));
    QGuiApplication::setDesktopFileName(QStringLiteral("qt-vulkan-spike"));
    QApplication::setStyle(QStringLiteral("Fusion"));

    QCommandLineParser cli;
    cli.addHelpOption();
    const QCommandLineOption shot_opt(QStringLiteral("shot"), QStringLiteral("Composite PNG."),
                                      QStringLiteral("path"));
    const QCommandLineOption niri_opt(QStringLiteral("niri-shot"),
                                      QStringLiteral("Compositor PNG (niri)."),
                                      QStringLiteral("path"));
    const QCommandLineOption after_opt(QStringLiteral("after-ms"),
                                       QStringLiteral("Delay before screenshots."),
                                       QStringLiteral("ms"), QStringLiteral("2000"));
    const QCommandLineOption quit_opt(QStringLiteral("quit-after"), QStringLiteral("Quit after."),
                                      QStringLiteral("ms"));
    const QCommandLineOption resize_opt(QStringLiteral("resize-cycle"),
                                        QStringLiteral("Resize through four sizes first."));
    const QCommandLineOption walk_opt(QStringLiteral("walk"), QStringLiteral("Walk at start."));
    const QCommandLineOption headless_opt(QStringLiteral("headless"),
                                          QStringLiteral("Offscreen renderer in a widget."));
    const QCommandLineOption novalid_opt(QStringLiteral("no-validation"),
                                         QStringLiteral("Without the validation layer."));
    cli.addOptions(
        {shot_opt, niri_opt, after_opt, quit_opt, resize_opt, walk_opt, headless_opt, novalid_opt});
    cli.process(app);

    const QString platform = QGuiApplication::platformName();
    const bool headless = cli.isSet(headless_opt) || platform == QLatin1String("offscreen") ||
                          platform == QLatin1String("minimal");
    spike::MainWindow window(headless, !cli.isSet(novalid_opt));
    window.show();

    // A chain of timed steps, so every run is scripted and repeatable.
    struct Step {
        int delay_ms;
        std::function<void()> run;
    };
    std::vector<Step> steps;
    if (cli.isSet(walk_opt) && window.viewport_window()) {
        steps.push_back({500, [&] { window.viewport_window()->set_walk(true); }});
    }
    if (cli.isSet(resize_opt)) {
        for (QSize size : {QSize(700, 450), QSize(1400, 850), QSize(500, 380), QSize(1100, 700)}) {
            steps.push_back({400, [&window, size] {
                                 window.resize(size);
                                 print_status(window, "resize");
                             }});
        }
    }
    const bool shots = cli.isSet(shot_opt) || cli.isSet(niri_opt);
    if (shots) {
        steps.push_back({cli.value(after_opt).toInt(), [&] {
                             print_status(window, "shot");
                             if (cli.isSet(shot_opt)) {
                                 window.grab_composite().save(cli.value(shot_opt));
                             }
                             if (cli.isSet(niri_opt)) {
                                 niri_screenshot(cli.value(niri_opt), window.windowTitle());
                             }
                         }});
    }
    if (cli.isSet(quit_opt) || shots) {
        const int ms = cli.isSet(quit_opt) ? cli.value(quit_opt).toInt() : 300;
        steps.push_back({ms, [&] {
                             print_status(window, "final");
                             QApplication::quit();
                         }});
    }

    auto run_from = std::make_shared<std::function<void(size_t)>>();
    *run_from = [&steps, run_from](size_t i) {
        if (i >= steps.size()) {
            return;
        }
        QTimer::singleShot(steps[i].delay_ms, [&steps, run_from, i] {
            steps[i].run();
            (*run_from)(i + 1);
        });
    };
    (*run_from)(0);
    const int code = QApplication::exec();
    *run_from = nullptr; // break the cycle
    return code;
}
