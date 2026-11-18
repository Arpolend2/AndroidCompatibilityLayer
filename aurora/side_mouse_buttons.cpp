#include "side_mouse_buttons.h"

#include <QEvent>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>

SideMouseButtons::SideMouseButtons(QObject *parent) : QObject(parent) {}

void SideMouseButtons::setTarget(QObject *target)
{
    if (m_target == target) return;
    QQuickItem *item = qobject_cast<QQuickItem *>(target);
    QWindow *window = item ? item->window() : qobject_cast<QWindow *>(target);
    if (target && !item && !window) {
        qWarning("SideMouseButtons target is not an item or window.");
        return;
    }
    QObject::disconnect(m_windowConnection);
    m_target = target;
    if (item) {
        m_windowConnection = connect(item, &QQuickItem::windowChanged, this,
                                     [this](QQuickWindow *current) { setWindow(current); });
    }
    setWindow(window);
    emit targetChanged();
}

void SideMouseButtons::setWindow(QWindow *window)
{
    if (m_window == window) return;
    if (m_window) m_window->removeEventFilter(this);
    m_window = window;
    m_backPressed = false;
    m_forwardPressed = false;
    if (m_window) m_window->installEventFilter(this);
}

bool SideMouseButtons::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_window) return false;
    if (event->type() == QEvent::WindowDeactivate) {
        m_backPressed = false;
        m_forwardPressed = false;
        return false;
    }
    if (event->type() != QEvent::MouseButtonPress &&
        event->type() != QEvent::MouseButtonDblClick &&
        event->type() != QEvent::MouseButtonRelease)
        return false;

    QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
    bool *pressed = nullptr;
    if (mouse->button() == Qt::BackButton) pressed = &m_backPressed;
    else if (mouse->button() == Qt::ForwardButton) pressed = &m_forwardPressed;
    else return false;

    if (event->type() == QEvent::MouseButtonRelease) {
        *pressed = false;
    } else if (!*pressed) {
        *pressed = true;
        if (mouse->button() == Qt::BackButton) emit backRequested();
        else emit forwardRequested();
    }
    return true;
}
