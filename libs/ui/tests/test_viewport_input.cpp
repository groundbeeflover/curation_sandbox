// SPDX-License-Identifier: Apache-2.0
//
// Input for features (issue #82, owner's addition of 2026-11-20): an event filter on the Viewport
// widget sees its mouse press, release and move (with and without a button), wheel, key press and
// release and focus loss, in both backends, with positions in the viewport's logical pixels; Tab
// and Backtab reach the filter instead of moving the keyboard focus; a click gives the viewport
// the keyboard focus (viewport.hpp, "Input for features").
//
// The headless tests run on the offscreen platform (QT_QPA_PLATFORM=offscreen); the window
// backend's test runs only on a platform with window surfaces. Both need a Vulkan device for the
// viewport, so they are tagged [gpu] and skip without one.

#include "ui_test.hpp"
#include "viewport/viewport.hpp"

#include <cs/ui/main_window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QTest>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

using cs::ui::MainWindow;
using cs::ui::Viewport;

// One event the filter saw.
struct Seen {
    QEvent::Type type = QEvent::None;
    QPointF position;                      // mouse and wheel: in the watched widget's pixels
    Qt::MouseButton button = Qt::NoButton; // mouse: the button that caused the event
    Qt::MouseButtons buttons;              // mouse: the buttons held
    QPoint angle_delta;                    // wheel
    int key = 0;                           // keys
};

std::string describe(const Seen& seen) {
    return "type " + std::to_string(static_cast<int>(seen.type)) + " at (" +
           std::to_string(seen.position.x()) + ", " + std::to_string(seen.position.y()) +
           ") button " + std::to_string(static_cast<int>(seen.button)) + " buttons " +
           std::to_string(seen.buttons.toInt()) + " key " + std::to_string(seen.key);
}

// A feature's event filter, installed on the viewport: records what it sees of the viewport's
// own input and lets every event through.
class InputRecorder : public QObject {
  public:
    explicit InputRecorder(QWidget& watched) : watched_(&watched) {
        watched.installEventFilter(this);
    }
    ~InputRecorder() override {
        if (watched_ != nullptr) {
            watched_->removeEventFilter(this);
        }
    }
    InputRecorder(const InputRecorder&) = delete;
    InputRecorder& operator=(const InputRecorder&) = delete;

    void clear() { seen_.clear(); }
    [[nodiscard]] const std::vector<Seen>& seen() const { return seen_; }

    // The events of this type, in order.
    [[nodiscard]] std::vector<Seen> of(QEvent::Type type) const {
        std::vector<Seen> out;
        std::copy_if(seen_.begin(), seen_.end(), std::back_inserter(out),
                     [type](const Seen& seen) { return seen.type == type; });
        return out;
    }
    [[nodiscard]] std::string log() const {
        std::string text;
        for (const Seen& seen : seen_) {
            text += describe(seen) + "\n";
        }
        return text;
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched != watched_) {
            return false;
        }
        Seen seen;
        seen.type = event->type();
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            seen.position = mouse->position();
            seen.button = mouse->button();
            seen.buttons = mouse->buttons();
            break;
        }
        case QEvent::Wheel: {
            const auto* wheel = static_cast<QWheelEvent*>(event);
            seen.position = wheel->position();
            seen.angle_delta = wheel->angleDelta();
            break;
        }
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
            seen.key = static_cast<QKeyEvent*>(event)->key();
            break;
        case QEvent::FocusOut:
            break;
        default:
            return false;
        }
        seen_.push_back(seen);
        return false;
    }

  private:
    QWidget* watched_;
    std::vector<Seen> seen_;
};

// A widget as a failure message names it: its class and object name, or "none".
std::string describe_widget(const QWidget* widget) {
    if (widget == nullptr) {
        return "none";
    }
    return std::string(widget->metaObject()->className()) + " \"" +
           widget->objectName().toStdString() + "\"";
}

// Whether the filter saw an event of this type at this position (mouse, wheel).
bool saw_at(const InputRecorder& recorder, QEvent::Type type, QPointF position) {
    const std::vector<Seen> events = recorder.of(type);
    return std::any_of(events.begin(), events.end(),
                       [&](const Seen& seen) { return seen.position == position; });
}

// Whether the filter saw a key event of this type for this key.
bool saw_key(const InputRecorder& recorder, QEvent::Type type, int key) {
    const std::vector<Seen> events = recorder.of(type);
    return std::any_of(events.begin(), events.end(),
                       [&](const Seen& seen) { return seen.key == key; });
}

// The application's window on the offscreen platform, its viewport headless, with a line edit as
// an inspector page (another widget that takes the keyboard focus, and Tab's next stop), shown,
// active, with a first frame drawn.
struct Shell {
    std::unique_ptr<MainWindow> window;
    Viewport* viewport = nullptr;
    QLineEdit* line_edit = nullptr;
};

Shell shown_shell() {
    cs::ui::test::require_offscreen();
    Shell shell;
    shell.window = cs::ui::test::make_shell();
    shell.viewport = shell.window->viewport();
    REQUIRE(shell.viewport->headless());
    shell.line_edit = new QLineEdit;
    shell.window->add_inspector_page(10, QStringLiteral("Test input"), shell.line_edit);
    shell.window->resize(1024, 700);
    shell.window->show();
    shell.window->activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(shell.window.get()));
    REQUIRE(shell.line_edit->isVisible());
    REQUIRE(shell.line_edit->isEnabled());
    // The viewport is big enough for the positions the tests use.
    REQUIRE(shell.viewport->width() > 200);
    REQUIRE(shell.viewport->height() > 200);
    return shell;
}

// The top-level window's native window, and a position of the viewport in it: the route the
// platform's events take, through the viewport's painting child (transparent to the mouse).
QPoint in_top_level(const Shell& shell, QPoint viewport_position) {
    return shell.viewport->mapTo(shell.window.get(), viewport_position);
}

} // namespace

// Acceptance criterion: the viewport's mouse press, release and move (also without a button)
// reach a feature's filter with positions in the viewport's logical pixels: a click at (30, 40)
// on the viewport is seen at (30, 40).
TEST_CASE("Viewport input (headless): mouse press, release and move reach a filter in the "
          "viewport's logical pixels",
          "[ui][gpu]") {
    Shell shell = shown_shell();
    Viewport* viewport = shell.viewport;
    InputRecorder recorder(*viewport);

    SECTION("sent to the viewport widget") {
        QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(30, 40));
        // Without a button: the viewport tracks the mouse.
        QTest::mouseMove(viewport, QPoint(50, 60));
        // A drag with the right button.
        QTest::mousePress(viewport, Qt::RightButton, Qt::NoModifier, QPoint(70, 80));
        QTest::mouseMove(viewport, QPoint(90, 100));
        QTest::mouseRelease(viewport, Qt::RightButton, Qt::NoModifier, QPoint(90, 100));
    }
    SECTION("sent to the top-level window where the viewport is") {
        QWindow* top = shell.window->windowHandle();
        REQUIRE(top != nullptr);
        QTest::mouseClick(top, Qt::LeftButton, Qt::NoModifier, in_top_level(shell, {30, 40}));
        QTest::mouseMove(top, in_top_level(shell, {50, 60}));
        QTest::mousePress(top, Qt::RightButton, Qt::NoModifier, in_top_level(shell, {70, 80}));
        QTest::mouseMove(top, in_top_level(shell, {90, 100}));
        QTest::mouseRelease(top, Qt::RightButton, Qt::NoModifier, in_top_level(shell, {90, 100}));
    }
    QApplication::processEvents();
    INFO("the filter saw:\n" << recorder.log());

    const std::vector<Seen> presses = recorder.of(QEvent::MouseButtonPress);
    const std::vector<Seen> releases = recorder.of(QEvent::MouseButtonRelease);
    REQUIRE(presses.size() == 2);
    REQUIRE(releases.size() == 2);
    CHECK(presses[0].position == QPointF(30, 40));
    CHECK(presses[0].button == Qt::LeftButton);
    CHECK(releases[0].position == QPointF(30, 40));
    CHECK(releases[0].button == Qt::LeftButton);
    CHECK(presses[1].position == QPointF(70, 80));
    CHECK(presses[1].button == Qt::RightButton);
    CHECK(releases[1].position == QPointF(90, 100));
    CHECK(releases[1].button == Qt::RightButton);

    // The move without a button, and the drag's move with the right button held.
    const std::vector<Seen> moves = recorder.of(QEvent::MouseMove);
    const auto hover = std::find_if(moves.begin(), moves.end(), [](const Seen& seen) {
        return seen.position == QPointF(50, 60);
    });
    REQUIRE(hover != moves.end());
    CHECK(hover->buttons == Qt::NoButton);
    const auto drag = std::find_if(moves.begin(), moves.end(), [](const Seen& seen) {
        return seen.position == QPointF(90, 100);
    });
    REQUIRE(drag != moves.end());
    CHECK(drag->buttons.testFlag(Qt::RightButton));
}

// The header's list (viewport.hpp): a double click reaches the filter too.
TEST_CASE("Viewport input (headless): a double click reaches a filter at its position",
          "[ui][gpu]") {
    Shell shell = shown_shell();
    InputRecorder recorder(*shell.viewport);
    QTest::mouseDClick(shell.window->windowHandle(), Qt::LeftButton, Qt::NoModifier,
                       in_top_level(shell, {120, 90}));
    QApplication::processEvents();
    INFO("the filter saw:\n" << recorder.log());
    CHECK(saw_at(recorder, QEvent::MouseButtonDblClick, QPointF(120, 90)));
}

// Acceptance criterion: the viewport's wheel reaches a feature's filter, at its position in the
// viewport's logical pixels.
TEST_CASE("Viewport input (headless): the wheel reaches a filter in the viewport's logical pixels",
          "[ui][gpu]") {
    Shell shell = shown_shell();
    InputRecorder recorder(*shell.viewport);
    QWindow* top = shell.window->windowHandle();
    REQUIRE(top != nullptr);

    // One notch forward (120 eighths of a degree), at (64, 48) of the viewport.
    QTest::wheelEvent(top, QPointF(in_top_level(shell, {64, 48})), QPoint(0, 120));
    QApplication::processEvents();
    INFO("the filter saw:\n" << recorder.log());
    const std::vector<Seen> wheels = recorder.of(QEvent::Wheel);
    REQUIRE(wheels.size() == 1);
    CHECK(wheels[0].position == QPointF(64, 48));
    CHECK(wheels[0].angle_delta == QPoint(0, 120));
}

// Acceptance criteria: key press and release reach a feature's filter; Tab and Backtab reach it
// instead of moving the keyboard focus, which stays in the viewport although another widget (the
// line edit) would take it.
TEST_CASE("Viewport input (headless): keys, Tab and Backtab included, reach a filter; the "
          "viewport keeps the focus",
          "[ui][gpu]") {
    Shell shell = shown_shell();
    Viewport* viewport = shell.viewport;
    QWindow* top = shell.window->windowHandle();
    REQUIRE(top != nullptr);
    viewport->setFocus(Qt::OtherFocusReason);
    REQUIRE(QTest::qWaitFor([&] { return viewport->hasFocus(); }, 1000));
    InputRecorder recorder(*viewport);

    // Through the window, as the platform sends them: to the focus widget.
    QTest::keyClick(top, Qt::Key_W);
    QTest::keyClick(top, Qt::Key_Tab);
    {
        INFO("the focus widget after Tab: " << describe_widget(QApplication::focusWidget()));
        CHECK(viewport->hasFocus());
        CHECK(QApplication::focusWidget() == viewport);
    }
    QTest::keyClick(top, Qt::Key_Backtab, Qt::ShiftModifier);
    CHECK(viewport->hasFocus());
    CHECK(QApplication::focusWidget() == viewport);
    CHECK_FALSE(shell.line_edit->hasFocus());
    QApplication::processEvents();

    INFO("the filter saw:\n" << recorder.log());
    CHECK(saw_key(recorder, QEvent::KeyPress, Qt::Key_W));
    CHECK(saw_key(recorder, QEvent::KeyRelease, Qt::Key_W));
    CHECK(saw_key(recorder, QEvent::KeyPress, Qt::Key_Tab));
    CHECK(saw_key(recorder, QEvent::KeyRelease, Qt::Key_Tab));
    CHECK(saw_key(recorder, QEvent::KeyPress, Qt::Key_Backtab));
    CHECK(saw_key(recorder, QEvent::KeyRelease, Qt::Key_Backtab));
    // Focus never left the viewport.
    CHECK(recorder.of(QEvent::FocusOut).empty());
}

// Acceptance criterion: a click gives the viewport the keyboard focus.
TEST_CASE("Viewport input (headless): a click on the viewport gives it the keyboard focus",
          "[ui][gpu]") {
    Shell shell = shown_shell();
    Viewport* viewport = shell.viewport;
    shell.line_edit->setFocus(Qt::OtherFocusReason);
    REQUIRE(QTest::qWaitFor([&] { return shell.line_edit->hasFocus(); }, 1000));
    REQUIRE_FALSE(viewport->hasFocus());

    SECTION("sent to the viewport widget") {
        QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(30, 40));
    }
    SECTION("sent to the top-level window where the viewport is") {
        QTest::mouseClick(shell.window->windowHandle(), Qt::LeftButton, Qt::NoModifier,
                          in_top_level(shell, {30, 40}));
    }
    QApplication::processEvents();
    CHECK(viewport->hasFocus());
    CHECK(QApplication::focusWidget() == viewport);
    CHECK_FALSE(shell.line_edit->hasFocus());
}

// Acceptance criterion: focus loss reaches a feature's filter, as FocusOut, when the keyboard
// focus moves from the viewport to another widget.
TEST_CASE("Viewport input (headless): FocusOut reaches a filter when the focus moves to another "
          "widget",
          "[ui][gpu]") {
    Shell shell = shown_shell();
    Viewport* viewport = shell.viewport;
    viewport->setFocus(Qt::OtherFocusReason);
    REQUIRE(QTest::qWaitFor([&] { return viewport->hasFocus(); }, 1000));
    InputRecorder recorder(*viewport);

    SECTION("set by the program") {
        shell.line_edit->setFocus(Qt::OtherFocusReason);
    }
    SECTION("by a click on the other widget") {
        QTest::mouseClick(shell.line_edit, Qt::LeftButton);
    }
    QApplication::processEvents();
    REQUIRE(shell.line_edit->hasFocus());
    INFO("the filter saw:\n" << recorder.log());
    CHECK(recorder.of(QEvent::FocusOut).size() == 1);
}

// ---- The window backend ------------------------------------------------------------------------

// Records where a window received its last wheel event, without consuming it.
class WheelPositionProbe : public QObject {
  public:
    WheelPositionProbe(QWindow& window, QPointF& position)
        : window_(&window), position_(&position) {
        window.installEventFilter(this);
    }
    ~WheelPositionProbe() override { window_->removeEventFilter(this); }
    WheelPositionProbe(const WheelPositionProbe&) = delete;
    WheelPositionProbe& operator=(const WheelPositionProbe&) = delete;
    WheelPositionProbe(WheelPositionProbe&&) = delete;
    WheelPositionProbe& operator=(WheelPositionProbe&&) = delete;

  protected:
    bool eventFilter(QObject* /*watched*/, QEvent* event) override {
        if (event->type() == QEvent::Wheel) {
            *position_ = static_cast<QWheelEvent*>(event)->position();
        }
        return false;
    }

  private:
    QWindow* window_;
    QPointF* position_;
};

// Acceptance criterion, with the window backend where a window system is available: the native
// window's press, move, release, wheel, key press and Tab reach the filter on the Viewport widget,
// with positions in the viewport's logical pixels, and a click on it gives the viewport the
// keyboard focus.
TEST_CASE("Viewport input (window): the native window's input reaches a filter on the viewport",
          "[ui][gpu]") {
    if (!cs::ui::platform_has_vulkan_surfaces()) {
        SKIP("needs a platform with window surfaces (run without QT_QPA_PLATFORM on a desktop); "
             "the platform is "
             << QGuiApplication::platformName().toStdString());
    }
    std::unique_ptr<MainWindow> window = cs::ui::test::make_shell();
    Viewport* viewport = window->viewport();
    REQUIRE_FALSE(viewport->headless());
    auto* line_edit = new QLineEdit;
    window->add_inspector_page(10, QStringLiteral("Test input"), line_edit);
    window->resize(1024, 700);
    window->show();
    REQUIRE(QTest::qWaitForWindowExposed(window.get()));
    window->activateWindow();
    if (!QTest::qWaitForWindowActive(window.get())) {
        SKIP("the window system did not activate the window, so no widget can hold the focus");
    }
    QWindow* surface = viewport->surface_window();
    REQUIRE(surface != nullptr);
    REQUIRE(QTest::qWaitForWindowExposed(surface));
    // A position of the viewport in the native window's own coordinates (the same when the native
    // window covers the viewport from its corner, as it should).
    const auto in_surface = [&](QPoint viewport_position) {
        return surface->mapFromGlobal(viewport->mapToGlobal(viewport_position));
    };

    line_edit->setFocus(Qt::OtherFocusReason);
    REQUIRE(QTest::qWaitFor([&] { return line_edit->hasFocus(); }, 1000));
    InputRecorder recorder(*viewport);

    QTest::mousePress(surface, Qt::LeftButton, Qt::NoModifier, in_surface({30, 40}));
    QTest::mouseMove(surface, in_surface({45, 55}));
    QTest::mouseRelease(surface, Qt::LeftButton, Qt::NoModifier, in_surface({45, 55}));
    QTest::mouseMove(surface, in_surface({60, 70}));
    // QTest::wheelEvent hands its position to QWindowSystemInterface as device pixels, so on a
    // screen scaled by 1.25 (X11 at 120 dpi) the native window itself receives (51.2, 38.4). The
    // viewport must see what the native window saw.
    QPointF surface_wheel(-1, -1);
    WheelPositionProbe probe(*surface, surface_wheel);
    QTest::wheelEvent(surface, QPointF(in_surface({64, 48})), QPoint(0, -120));
    QApplication::processEvents();

    {
        INFO("the filter saw:\n" << recorder.log());
        CHECK(saw_at(recorder, QEvent::MouseButtonPress, QPointF(30, 40)));
        CHECK(saw_at(recorder, QEvent::MouseMove, QPointF(45, 55)));
        CHECK(saw_at(recorder, QEvent::MouseButtonRelease, QPointF(45, 55)));
        // A move without a button.
        CHECK(saw_at(recorder, QEvent::MouseMove, QPointF(60, 70)));
        INFO("the native window saw the wheel at (" << surface_wheel.x() << ", "
                                                    << surface_wheel.y() << ")");
        REQUIRE(surface_wheel != QPointF(-1, -1));
        CHECK(saw_at(recorder, QEvent::Wheel, surface_wheel));
    }

    // The click gave the viewport the keyboard focus: it is the viewport's (or within it), and
    // the line edit lost it.
    CHECK(QTest::qWaitFor([&] { return !line_edit->hasFocus(); }, 1000));
    QWidget* focus = QApplication::focusWidget();
    {
        INFO("the focus widget after the click: " << describe_widget(focus));
        CHECK((focus == viewport || viewport->isAncestorOf(focus)));
    }

    // Keys, sent to the native window as the platform sends them there.
    QTest::keyClick(surface, Qt::Key_W);
    QTest::keyClick(surface, Qt::Key_Tab);
    QApplication::processEvents();
    {
        INFO("the filter saw:\n" << recorder.log());
        CHECK(saw_key(recorder, QEvent::KeyPress, Qt::Key_W));
        CHECK(saw_key(recorder, QEvent::KeyPress, Qt::Key_Tab));
    }
    // Tab did not move the focus out of the viewport.
    CHECK_FALSE(line_edit->hasFocus());
    focus = QApplication::focusWidget();
    INFO("the focus widget after Tab: " << describe_widget(focus));
    CHECK((focus == viewport || viewport->isAncestorOf(focus)));
    cs::ui::test::require_no_validation_messages(*window);
}
