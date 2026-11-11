// SPDX-License-Identifier: Apache-2.0
// The CI question of M1-04: can a QTest on the offscreen platform open the shell, check its
// widgets and save a screenshot that includes the viewport's frame? Run with
// QT_QPA_PLATFORM=offscreen; on CI the Vulkan device is lavapipe.
#include "main_window.hpp"
#include "viewport.hpp"

#include <QDir>
#include <QDockWidget>
#include <QGuiApplication>
#include <QLabel>
#include <QMenuBar>
#include <QTest>

class TestShell : public QObject {
    Q_OBJECT
  private slots:
    void opens_and_grabs_a_screenshot() {
        QVERIFY2(QGuiApplication::platformName() == QLatin1String("offscreen"),
                 "run with QT_QPA_PLATFORM=offscreen");
        spike::MainWindow window(/*headless=*/true, /*validation=*/true);
        window.resize(1100, 700);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        spike::ViewportState& state = window.viewport_state();
        // A skip is silent, so CI sets SPIKE_REQUIRE_VULKAN to turn a missing device into a
        // failure.
        if (!state.init_error.isEmpty() && qEnvironmentVariableIsSet("SPIKE_REQUIRE_VULKAN")) {
            QFAIL(qPrintable(QStringLiteral("no Vulkan device: ") + state.init_error));
        }
        if (!state.init_error.isEmpty()) {
            QSKIP(qPrintable(QStringLiteral("no Vulkan device: ") + state.init_error));
        }
        QVERIFY(QTest::qWaitFor([&] { return state.renderer.frames() >= 3; }, 5000));

        QCOMPARE(window.windowTitle(), QStringLiteral("Curation Sandbox - Qt Vulkan spike"));
        QVERIFY(window.findChild<QDockWidget*>(QStringLiteral("inspector")) != nullptr);
        QCOMPARE(window.menuBar()->actions().size(), 2);

        const QImage shot = window.grab_composite();
        QVERIFY(!shot.isNull());
        const QString dir = qEnvironmentVariable("SPIKE_SCREENSHOT_DIR", QDir::currentPath());
        QDir().mkpath(dir);
        const QString path = dir + QStringLiteral("/offscreen-shell.png");
        QVERIFY(shot.save(path));
        qInfo("screenshot: %s (%dx%d), device: %s", qPrintable(path), shot.width(), shot.height(),
              state.renderer.device_name().c_str());

        // The triangle's centroid is the viewport's centre at every angle, so the centre pixel
        // is the triangle's colour, not the dark clear colour.
        const QWidget* vp = window.viewport_widget();
        const QPoint centre = vp->mapTo(&window, vp->rect().center());
        const qreal dpr = shot.devicePixelRatio();
        const QColor c = shot.pixelColor(int(centre.x() * dpr), int(centre.y() * dpr));
        QVERIFY2(c.red() + c.green() + c.blue() > 150, qPrintable(c.name()));
        QCOMPARE(state.renderer.validation_messages(), 0);
    }
};

QTEST_MAIN(TestShell)
#include "tst_shell.moc"
