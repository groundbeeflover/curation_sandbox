// SPDX-License-Identifier: Apache-2.0
//
// PointerLock (ADR 0011): the grab-and-warp fallback, driven with synthetic events on the
// offscreen platform, and the Wayland lock, granted by a real compositor on a Wayland session.

#include "viewport/pointer_lock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QEvent>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTest>
#include <QWidget>
#include <QWindow>

namespace {

using cs::ui::PointerLock;

void require_offscreen() {
    const QString platform = QGuiApplication::platformName();
    if (platform != QLatin1String("offscreen")) {
        SKIP("drives the grab-and-warp fallback with synthetic events, which needs "
             "QT_QPA_PLATFORM=offscreen; the platform is "
             << platform.toStdString());
    }
}

// A shown and active window on the offscreen platform, 400 x 300 logical pixels.
struct ActiveWindow {
    static constexpr QPoint centre{200, 150};
    QWindow window;

    ActiveWindow() {
        window.setGeometry(100, 100, 400, 300);
        window.show();
        window.requestActivate();
        REQUIRE(QTest::qWaitForWindowActive(&window));
    }

    QPoint global_centre() const { return window.mapToGlobal(centre); }

    // Moves the mouse to a point of the window, as a user would. On the offscreen platform
    // QCursor::setPos moves the cursor and sends the window the move. QTest::mouseMove sends the
    // move but leaves the cursor where it was, at the centre, where Qt would skip PointerLock's
    // warp back as a no-op.
    void move_mouse_to(QPoint local) {
        QCursor::setPos(window.screen(), window.mapToGlobal(local));
        QCoreApplication::processEvents();
    }
};

// Counts the mouse moves that reach a window, before any filter installed earlier sees them.
class MoveCounter : public QObject {
  public:
    int moves = 0;
    bool eventFilter(QObject*, QEvent* event) override {
        moves += event->type() == QEvent::MouseMove ? 1 : 0;
        return false;
    }
};

} // namespace

TEST_CASE("PointerLock: lock() fails on a window that is not shown", "[ui]") {
    QWindow window;
    PointerLock lock(&window);
    QSignalSpy locked(&lock, &PointerLock::locked);
    QSignalSpy unlocked(&lock, &PointerLock::unlocked);

    CHECK_FALSE(lock.lock());
    CHECK_FALSE(lock.is_locked());
    lock.unlock();
    CHECK(locked.isEmpty());
    CHECK(unlocked.isEmpty());
    CHECK(window.cursor().shape() != Qt::BlankCursor);
}

TEST_CASE("PointerLock fallback: lock() holds the pointer at once, hidden, at the centre", "[ui]") {
    require_offscreen();
    ActiveWindow w;
    PointerLock lock(&w.window);
    QSignalSpy locked(&lock, &PointerLock::locked);

    REQUIRE(lock.lock());
    CHECK(lock.is_locked());
    CHECK(locked.count() == 1);
    CHECK(w.window.cursor().shape() == Qt::BlankCursor);
    CHECK(QCursor::pos(w.window.screen()) == w.global_centre());

    // Locking again while on changes nothing.
    CHECK(lock.lock());
    CHECK(locked.count() == 1);
}

TEST_CASE("PointerLock fallback: a move of known size is motion of that size; the warp back "
          "to the centre is none",
          "[ui]") {
    require_offscreen();
    ActiveWindow w;
    PointerLock lock(&w.window);
    QSignalSpy motion(&lock, &PointerLock::motion);
    REQUIRE(lock.lock());
    MoveCounter counter;
    w.window.installEventFilter(&counter);

    // lock() warped the cursor to the centre: that move arrives, and is not motion.
    QCoreApplication::processEvents();
    CHECK(counter.moves == 1);
    CHECK(motion.isEmpty());

    // The move, then the warp back to the centre.
    w.move_mouse_to(ActiveWindow::centre + QPoint(12, -9));
    CHECK(counter.moves == 3);
    REQUIRE(motion.count() == 1);
    CHECK(motion[0][0].toDouble() == 12.0);
    CHECK(motion[0][1].toDouble() == -9.0);
    CHECK(QCursor::pos(w.window.screen()) == w.global_centre());

    w.move_mouse_to(ActiveWindow::centre + QPoint(-30, 25));
    CHECK(counter.moves == 5);
    REQUIRE(motion.count() == 2);
    CHECK(motion[1][0].toDouble() == -30.0);
    CHECK(motion[1][1].toDouble() == 25.0);
    CHECK(QCursor::pos(w.window.screen()) == w.global_centre());
}

TEST_CASE("PointerLock fallback: a move that arrives in place of the warp's is measured from the "
          "centre",
          "[ui]") {
    require_offscreen();
    ActiveWindow w;
    PointerLock lock(&w.window);
    QSignalSpy motion(&lock, &PointerLock::motion);
    REQUIRE(lock.lock());
    QCoreApplication::processEvents();

    // On Windows the warp's own move can merge into the user's next one, so no move at the
    // centre comes between them. QTest::mouseMove leaves the offscreen cursor at the centre, as
    // after a real warp, and Qt skips the warp to where the cursor already is: the same order.
    QTest::mouseMove(&w.window, ActiveWindow::centre + QPoint(40, 0));
    QTest::mouseMove(&w.window, ActiveWindow::centre + QPoint(3, -2));
    REQUIRE(motion.count() == 2);
    CHECK(motion[0][0].toDouble() == 40.0);
    CHECK(motion[0][1].toDouble() == 0.0);
    CHECK(motion[1][0].toDouble() == 3.0);
    CHECK(motion[1][1].toDouble() == -2.0);
}

TEST_CASE("PointerLock fallback: Esc, focus loss and unlock() each end walk mode once", "[ui]") {
    require_offscreen();
    ActiveWindow w;
    w.window.setCursor(Qt::CrossCursor);
    PointerLock lock(&w.window);
    QSignalSpy unlocked(&lock, &PointerLock::unlocked);
    QSignalSpy motion(&lock, &PointerLock::motion);
    REQUIRE(lock.lock());
    QWindow other;

    SECTION("Esc") {
        QTest::keyClick(&w.window, Qt::Key_Escape);
    }
    SECTION("focus loss") {
        other.setGeometry(550, 100, 200, 200);
        other.show();
        other.requestActivate();
        REQUIRE(QTest::qWaitForWindowActive(&other));
    }
    SECTION("unlock()") {
        lock.unlock();
    }

    CHECK_FALSE(lock.is_locked());
    CHECK(unlocked.count() == 1);
    CHECK(w.window.cursor().shape() == Qt::CrossCursor);

    // Once walk mode is off nothing ends it again, and moves are not motion.
    lock.unlock();
    QTest::keyClick(&w.window, Qt::Key_Escape);
    w.move_mouse_to(ActiveWindow::centre + QPoint(7, 7));
    CHECK(unlocked.count() == 1);
    CHECK(motion.isEmpty());
}

TEST_CASE("PointerLock fallback: destroying the lock ends walk mode", "[ui]") {
    require_offscreen();
    ActiveWindow w;
    w.window.setCursor(Qt::CrossCursor);
    int unlocked = 0;
    {
        PointerLock lock(&w.window);
        QObject::connect(&lock, &PointerLock::unlocked, [&] { ++unlocked; });
        REQUIRE(lock.lock());
    }
    CHECK(unlocked == 1);
    CHECK(w.window.cursor().shape() == Qt::CrossCursor);
}

TEST_CASE("PointerLock on Wayland: the compositor grants the lock", "[ui]") {
    const QString platform = QGuiApplication::platformName();
    if (!platform.startsWith(QLatin1String("wayland"))) {
        SKIP("needs a Wayland session with QT_QPA_PLATFORM unset; the platform is "
             << platform.toStdString());
    }
    // A widget, because a Wayland surface is shown only once it has a buffer, which a plain
    // QWindow never draws. Full screen, so that the pointer is over it wherever it was.
    QWidget widget;
    widget.setWindowTitle(QStringLiteral("cs_ui_tests: pointer lock"));
    widget.showFullScreen();
    REQUIRE(QTest::qWaitForWindowExposed(&widget));
    PointerLock lock(widget.windowHandle());
    QSignalSpy locked(&lock, &PointerLock::locked);
    QSignalSpy unlocked(&lock, &PointerLock::unlocked);

    REQUIRE(lock.lock());
    INFO("the compositor grants the lock once the window has focus and the pointer is over it");
    REQUIRE(locked.wait(5000));
    CHECK(widget.windowHandle()->cursor().shape() == Qt::BlankCursor);

    lock.unlock();
    CHECK_FALSE(lock.is_locked());
    CHECK(unlocked.count() == 1);
    CHECK(widget.windowHandle()->cursor().shape() != Qt::BlankCursor);
}
