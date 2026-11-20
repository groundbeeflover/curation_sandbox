// SPDX-License-Identifier: Apache-2.0
//
// curation [<project folder>] [--screenshot <file.png>]
//
// Opens the main window, with the project folder if one is given. --screenshot saves the window,
// viewport included, once the viewport has drawn its first frame, and quits: for documentation
// and for checking a change without watching the window.

#include <cs/ui/main_window.hpp>

#include <QApplication>
#include <QCommandLineParser>
#include <QImage>
#include <QTimer>

#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>

namespace {

// How long --screenshot waits for the first frame before saving what there is.
constexpr int screenshot_timeout_ms = 10'000;
// After the first frame, a little longer, so that the window has settled at its size.
constexpr int screenshot_settle_ms = 500;

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Curation Sandbox"));
    QApplication::setOrganizationName(QStringLiteral("Curation Sandbox"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Build a room, hang artworks at their true size, light them, read the lux."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("project"),
                                 QStringLiteral("A project folder to open."),
                                 QStringLiteral("[project]"));
    const QCommandLineOption screenshot_option(
        QStringLiteral("screenshot"),
        QStringLiteral("Save the window to <file> (PNG) once the viewport has drawn, then quit."),
        QStringLiteral("file"));
    parser.addOption(screenshot_option);
    parser.process(app);
    const QStringList positional = parser.positionalArguments();
    if (positional.size() > 1) {
        std::cerr << "curation: give at most one project folder\n";
        return 2;
    }

    std::unique_ptr<cs::ui::MainWindow> window;
    try {
        std::optional<std::filesystem::path> project;
        if (!positional.isEmpty()) {
            project = std::filesystem::path(positional.front().toStdU16String());
        }
        window = cs::ui::make_main_window(project);
    } catch (const std::exception& error) {
        std::cerr << "curation: " << error.what() << '\n';
        return 1;
    }
    window->show();

    // Lives until exec() returns: the timers below call it.
    const QString screenshot_file = parser.value(screenshot_option);
    bool saved = false;
    const auto save_screenshot = [&] {
        if (saved) {
            return;
        }
        saved = true;
        if (!window->screenshot().save(screenshot_file)) {
            std::cerr << "curation: cannot write " << screenshot_file.toStdString() << '\n';
            QApplication::exit(1);
            return;
        }
        std::cout << "curation: saved " << screenshot_file.toStdString() << " ("
                  << window->validation_status().toStdString() << ", "
                  << window->exposure_status().toStdString() << ")\n";
        QApplication::exit(0);
    };
    if (parser.isSet(screenshot_option)) {
        QObject::connect(
            window.get(), &cs::ui::MainWindow::viewport_frame_drawn, window.get(),
            [&] { QTimer::singleShot(screenshot_settle_ms, window.get(), save_screenshot); },
            Qt::SingleShotConnection);
        QTimer::singleShot(screenshot_timeout_ms, window.get(), save_screenshot);
    }
    return QApplication::exec();
}
