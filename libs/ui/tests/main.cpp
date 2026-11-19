// SPDX-License-Identifier: Apache-2.0
//
// The ui tests' main: Catch2's, inside the QApplication that every Qt widget and window needs.
// The platform is Qt's choice, so tests that need a particular one check
// QGuiApplication::platformName() and skip, saying why: QT_QPA_PLATFORM=offscreen runs the
// headless tests (as CI does), and an unset QT_QPA_PLATFORM on a Wayland session runs the tests
// that need the compositor.

#include <catch2/catch_session.hpp>

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
