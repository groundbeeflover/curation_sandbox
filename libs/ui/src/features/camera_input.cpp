// SPDX-License-Identifier: Apache-2.0
//
// Moving round the room (M1-27, ADR 0006 Views, ADR 0011 points 9 and 10): orbit by default, walk
// with Tab. The viewport's input (viewport.hpp, "Input for features") drives editor's camera
// controllers, which turn it into cameras for the viewport.
//
// - Orbit: dragging with the left button turns the camera round a target, the wheel zooms. The
//   target is the point of the view's ray nearest the middle of the room the camera is in.
// - Walk: Tab starts it with the viewport's pointer lock (relative motion, never deltas of warped
//   positions). The mouse looks round; W, A, S, D (or the arrow keys) walk at eye height inside the
//   room. Tab again, Esc, the viewport losing the focus and the compositor ending the lock each
//   end it. Each switch starts the next controller from the camera the last one left, so the view
//   keeps its direction both ways.
//
// What walking needs from the curator is said in the status bar under the viewport: a widget
// cannot draw over the window backend's native Vulkan window.

#include "features/feature.hpp"
#include "viewport/viewport.hpp"

#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/editor/camera_controller.hpp>
#include <cs/world/world.hpp>

#include <QElapsedTimer>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QObject>
#include <QPointF>
#include <QStatusBar>
#include <QTimer>
#include <QWheelEvent>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

namespace cs::ui {

namespace {

// How often walking moves the eye while a key is held: about 60 times a second.
constexpr int walk_tick_ms = 16;
// The orbit's distance where the view does not look towards the middle of a room. [C]
constexpr double default_orbit_distance_m = 3.0;
// A middle of the room nearer than this along the view is too near to orbit round. [C]
constexpr double min_orbit_target_m = 0.5;

// Whether the point lies inside the outline (even-odd rule, either way round).
bool inside(const std::vector<doc::Vec2>& outline, glm::dvec2 p) {
    bool in = false;
    for (std::size_t i = 0, j = outline.size() - 1; i < outline.size(); j = i++) {
        const doc::Vec2& a = outline[i];
        const doc::Vec2& b = outline[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
            in = !in;
        }
    }
    return in;
}

// The room the eye stands in, in plan: the first in order of id whose outline holds it, else the
// first room, else none.
const doc::Room* room_at(const doc::Document& document, glm::dvec3 eye_m) {
    const doc::Room* first = nullptr;
    for (const auto& [id, entity] : document.entities()) {
        if (!entity.room || entity.room->outline_m.size() < 3) {
            continue;
        }
        if (inside(entity.room->outline_m, glm::dvec2(eye_m))) {
            return &*entity.room;
        }
        if (first == nullptr) {
            first = &*entity.room;
        }
    }
    return first;
}

// How far ahead of the camera the orbit's target is: where the view's ray passes nearest the
// middle of the room (the middle of its outline's bounds, half way up).
double orbit_distance_m(const doc::Document& document, const world::Camera& camera) {
    const doc::Room* room = room_at(document, camera.position_m);
    if (room == nullptr) {
        return default_orbit_distance_m;
    }
    glm::dvec2 low(std::numeric_limits<double>::max());
    glm::dvec2 high(std::numeric_limits<double>::lowest());
    for (const doc::Vec2& point : room->outline_m) {
        low = glm::min(low, glm::dvec2(point.x, point.y));
        high = glm::max(high, glm::dvec2(point.x, point.y));
    }
    const glm::dvec3 middle((low + high) / 2.0,
                            (room->floor_height_m + room->ceiling_height_m) / 2.0);
    const double along = glm::dot(middle - camera.position_m, camera.forward);
    return along >= min_orbit_target_m ? along : default_orbit_distance_m;
}

class CameraInput : public QObject {
  public:
    CameraInput(MainWindow& window, Viewport& viewport)
        : QObject(&viewport), window_(window), viewport_(viewport) {
        viewport.installEventFilter(this);
        tick_.setInterval(walk_tick_ms);
        connect(&tick_, &QTimer::timeout, this, [this] { advance(); });
        connect(&viewport, &Viewport::pointer_locked_changed, this,
                [this](bool locked) { pointer_locked_changed(locked); });
        connect(&viewport, &Viewport::pointer_motion, this,
                [this](double dx, double dy) { look(dx, dy); });
        connect(&window, &MainWindow::document_changed, this, [this] { room_changed(); });
        connect(&window, &MainWindow::project_changed, this, [this] { project_changed(); });
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched != &viewport_) {
            return false;
        }
        switch (event->type()) {
        case QEvent::MouseButtonPress:
            mouse_pressed(*static_cast<QMouseEvent*>(event));
            return false; // the click also gives the viewport the keyboard focus
        case QEvent::MouseMove:
            return mouse_moved(*static_cast<QMouseEvent*>(event));
        case QEvent::MouseButtonRelease:
            if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
                dragging_ = false;
            }
            return false;
        case QEvent::Wheel:
            return wheel(*static_cast<QWheelEvent*>(event));
        case QEvent::KeyPress:
            return key(*static_cast<QKeyEvent*>(event), true);
        case QEvent::KeyRelease:
            return key(*static_cast<QKeyEvent*>(event), false);
        case QEvent::FocusOut:
            stop_walking(); // ADR 0011, point 10
            return false;
        default:
            return false;
        }
    }

  private:
    enum class Mode {
        orbit,
        waiting, // for the compositor to lock the pointer (Wayland, the pointer outside the view)
        walking,
    };

    // ---- Orbit --------------------------------------------------------------------------------

    // The orbit, started again from the viewport's camera when something else moved it (a new
    // project, its viewpoint, New room).
    editor::OrbitController& orbit() {
        const world::Camera& camera = viewport_.camera();
        if (!orbit_ || !shown_ || camera != *shown_) {
            orbit_.emplace(camera, orbit_distance_m(window_.document(), camera));
            shown_ = camera;
        }
        return *orbit_;
    }

    void mouse_pressed(const QMouseEvent& event) {
        if (mode_ == Mode::orbit && event.button() == Qt::LeftButton) {
            dragging_ = true;
            last_mouse_ = event.position();
        }
    }

    bool mouse_moved(const QMouseEvent& event) {
        const bool left_held = event.buttons().testFlag(Qt::LeftButton);
        if (mode_ != Mode::orbit || !dragging_ || !left_held) {
            dragging_ = dragging_ && left_held;
            return false;
        }
        const QPointF delta = event.position() - last_mouse_;
        last_mouse_ = event.position();
        editor::CameraInput input;
        input.mouse_delta_px = {delta.x(), delta.y()};
        drive(orbit(), input);
        return true;
    }

    bool wheel(const QWheelEvent& event) {
        if (mode_ != Mode::orbit) {
            return false;
        }
        editor::CameraInput input;
        input.wheel_notches = event.angleDelta().y() / 120.0;
        drive(orbit(), input);
        return true;
    }

    // ---- Keys ---------------------------------------------------------------------------------

    bool key(const QKeyEvent& event, bool pressed) {
        const int code = event.key();
        if (code == Qt::Key_Tab || code == Qt::Key_Backtab) {
            if (pressed && !event.isAutoRepeat()) {
                toggle_walk();
            }
            return true; // never a move of the keyboard focus
        }
        if (pressed && code == Qt::Key_Escape && mode_ != Mode::orbit) {
            // While walking the pointer lock takes Esc first; this is Esc while it waits.
            stop_walking();
            return true;
        }
        bool* held = movement_key(code);
        if (held == nullptr || mode_ != Mode::walking) {
            return false; // back_to_orbit() let go of every key
        }
        if (!event.isAutoRepeat() && *held != pressed) {
            advance(); // walk the time so far with the keys as they were
            *held = pressed;
            update_tick();
        }
        return true;
    }

    // The flag of a movement key, or null for any other key.
    bool* movement_key(int code) {
        switch (code) {
        case Qt::Key_W:
        case Qt::Key_Up:
            return &keys_.forward;
        case Qt::Key_S:
        case Qt::Key_Down:
            return &keys_.back;
        case Qt::Key_A:
        case Qt::Key_Left:
            return &keys_.left;
        case Qt::Key_D:
        case Qt::Key_Right:
            return &keys_.right;
        default:
            return nullptr;
        }
    }

    bool any_key_held() const { return keys_.forward || keys_.back || keys_.left || keys_.right; }

    void update_tick() {
        if (mode_ == Mode::walking && any_key_held()) {
            if (!tick_.isActive()) {
                clock_.start();
                tick_.start();
            }
        } else {
            tick_.stop();
        }
    }

    // ---- Walk ---------------------------------------------------------------------------------

    editor::WalkRoom walk_room_for(const world::Camera& camera) const {
        const doc::Room* room = room_at(window_.document(), camera.position_m);
        return room != nullptr ? editor::walk_room(*room) : editor::WalkRoom{};
    }

    void toggle_walk() {
        if (mode_ == Mode::orbit) {
            start_walking();
        } else {
            stop_walking();
        }
    }

    void start_walking() {
        const world::Camera camera = viewport_.camera();
        walk_.emplace(walk_room_for(camera), window_.document().settings(), camera);
        mode_ = Mode::waiting;
        dragging_ = false;
        // On the fallback, and on Wayland with the pointer in the view, the lock is granted (and
        // pointer_locked_changed(true) arrives) within lock_pointer().
        if (!viewport_.lock_pointer()) {
            mode_ = Mode::orbit;
            walk_.reset();
            say(MainWindow::tr("Walking needs the pointer held in the view, which this system "
                               "does not allow"));
            return;
        }
        if (mode_ == Mode::waiting) {
            say(MainWindow::tr("Move the pointer into the view to start walking. Esc to cancel"));
        }
    }

    void pointer_locked_changed(bool locked) {
        if (locked) {
            if (mode_ == Mode::waiting) {
                mode_ = Mode::walking;
                show(walk_->camera()); // at eye height, inside the room
                say(MainWindow::tr("Walking: W A S D to move, the mouse to look. Esc to stop "
                                   "walking"));
                update_tick();
            }
        } else if (mode_ != Mode::orbit) {
            // Esc, focus loss, unlock_pointer() or the compositor's unlocked event.
            back_to_orbit();
        }
    }

    void stop_walking() {
        if (mode_ == Mode::orbit) {
            return;
        }
        viewport_.unlock_pointer();
        // unlock_pointer() reports the end through pointer_locked_changed(false); in case it had
        // nothing to end:
        if (mode_ != Mode::orbit) {
            back_to_orbit();
        }
    }

    // The orbit starts again from what the viewport shows: where the walk left the view, or where
    // it was if the walk never began (the lock not granted yet).
    void back_to_orbit() {
        advance();
        mode_ = Mode::orbit;
        walk_.reset();
        keys_ = {};
        tick_.stop();
        orbit_.reset();
        unsay();
    }

    void look(double dx, double dy) {
        if (mode_ != Mode::walking) {
            return;
        }
        editor::CameraInput input;
        input.mouse_delta_px = {dx, dy};
        drive(*walk_, input);
    }

    // Walks the time since the last step with the keys held.
    void advance() {
        if (mode_ != Mode::walking || !tick_.isActive()) {
            return;
        }
        editor::CameraInput input = keys_;
        input.elapsed_s = static_cast<double>(clock_.nsecsElapsed()) * 1e-9;
        clock_.restart();
        drive(*walk_, input);
    }

    // ---- The project --------------------------------------------------------------------------

    // The walls may have moved: a walk goes on in the room as it is now.
    void room_changed() {
        if (mode_ != Mode::orbit) {
            advance();
            walk_.emplace(walk_room_for(viewport_.camera()), window_.document().settings(),
                          viewport_.camera());
        }
    }

    // New and Open show the project from its first viewpoint, which ends a walk (and orbit()
    // starts from it); a save leaves the camera where it was. Before this feature has shown a
    // camera (a walk still waiting for the pointer) it cannot tell them apart, so it ends the walk.
    void project_changed() {
        if (!shown_ || viewport_.camera() != *shown_) {
            stop_walking();
        } else {
            room_changed();
        }
    }

    // ---- Output -------------------------------------------------------------------------------

    // Moves a controller with the input and shows its camera.
    template <class Controller>
    void drive(Controller& controller, const editor::CameraInput& input) {
        controller.update(input);
        show(controller.camera());
    }

    void show(const world::Camera& camera) {
        viewport_.set_camera(camera);
        shown_ = viewport_.camera();
    }

    void say(const QString& message) {
        said_ = message;
        window_.statusBar()->showMessage(message);
    }

    // Clears the status bar's message if it is still this feature's.
    void unsay() {
        if (!said_.isEmpty() && window_.statusBar()->currentMessage() == said_) {
            window_.statusBar()->clearMessage();
        }
        said_.clear();
    }

    MainWindow& window_;
    Viewport& viewport_;
    Mode mode_ = Mode::orbit;
    std::optional<editor::OrbitController> orbit_;
    std::optional<editor::WalkController> walk_; // exactly while the mode is not orbit
    std::optional<world::Camera> shown_;         // the camera this feature last showed
    bool dragging_ = false;
    QPointF last_mouse_;
    editor::CameraInput keys_; // only the movement keys are used
    QTimer tick_;
    QElapsedTimer clock_;
    QString said_;
};

} // namespace

void register_camera_input(MainWindow& window) {
    if (Viewport* viewport = window.viewport()) {
        // Owned by the viewport, so it goes with it.
        new CameraInput(window, *viewport);
    }
}

} // namespace cs::ui
