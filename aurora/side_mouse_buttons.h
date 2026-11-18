#pragma once

#include <QObject>
#include <QPointer>
#include <QWindow>

class SideMouseButtons : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *target READ target WRITE setTarget NOTIFY targetChanged)

public:
    explicit SideMouseButtons(QObject *parent = nullptr);
    QObject *target() const { return m_target.data(); }
    QWindow *window() const { return m_window.data(); }
    void setTarget(QObject *target);

signals:
    void targetChanged();
    void backRequested();
    void forwardRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setWindow(QWindow *window);
    QPointer<QObject> m_target;
    QPointer<QWindow> m_window;
    QMetaObject::Connection m_windowConnection;
    bool m_backPressed = false;
    bool m_forwardPressed = false;
};
