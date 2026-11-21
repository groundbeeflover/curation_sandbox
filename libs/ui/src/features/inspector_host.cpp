// SPDX-License-Identifier: Apache-2.0
//
// The inspector: a dock on the right that shows the pages other features hand to
// MainWindow::add_inspector_page, one tab each, in their order. Pages added before this feature
// registers and after it are shown alike, so registration order does not matter.

#include "features/feature.hpp"

#include <QDockWidget>
#include <QLabel>
#include <QStackedWidget>
#include <QTabWidget>

namespace cs::ui {

namespace {

// Shown while no feature has a page.
QLabel* make_placeholder(QWidget* parent) {
    auto* label = new QLabel(MainWindow::tr("Nothing to inspect yet"), parent);
    label->setAlignment(Qt::AlignCenter);
    label->setObjectName(QStringLiteral("inspector_placeholder"));
    return label;
}

} // namespace

void register_inspector_host(MainWindow& window) {
    auto* dock = new QDockWidget(MainWindow::tr("Inspector"), &window);
    dock->setObjectName(QStringLiteral("inspector"));
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    auto* stack = new QStackedWidget(dock);
    QLabel* placeholder = make_placeholder(stack);
    auto* tabs = new QTabWidget(stack);
    tabs->setObjectName(QStringLiteral("inspector_pages"));
    stack->addWidget(placeholder);
    stack->addWidget(tabs);
    dock->setWidget(stack);

    const auto insert = [&window, stack, tabs](int index) {
        const InspectorPage& page = window.inspector_pages()[static_cast<std::size_t>(index)];
        tabs->insertTab(index, page.widget, page.title);
        page.widget->show();
        stack->setCurrentWidget(tabs);
    };
    for (int i = 0; i < static_cast<int>(window.inspector_pages().size()); ++i) {
        insert(i);
    }
    QObject::connect(&window, &MainWindow::inspector_page_added, dock, insert);

    window.addDockWidget(Qt::RightDockWidgetArea, dock);
}

} // namespace cs::ui
