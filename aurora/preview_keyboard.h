#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_KEYBOARD_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_KEYBOARD_H

#include "../core/include/megler.h"

#include <QPointer>
#include <QQuickItem>
#include <QSize>
#include <QString>

class QInputMethodEvent;
class QKeyEvent;

class PreviewKeyboard : public QQuickItem
{
    Q_OBJECT

public:
    explicit PreviewKeyboard(QQuickItem *parent = nullptr);

    // Keep this object alive until Megler's on_exit callback has completed.
    megler_keyboard_host host();
    void start();
    void stop();
    bool back();
    void setSurface(QQuickItem *surface, const QSize &pixels);

    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

protected:
    void inputMethodEvent(QInputMethodEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void showKeyboard(int kind, int flags, int enter);
    void hideKeyboard();
    void visibilityChanged();
    void updateHeight();

private:
    static void MEGLER_CB requestShow(void *user, int kind, int flags, int enter);
    static void MEGLER_CB requestHide(void *user);
    void dismiss(bool notifyAndroid);
    void updateInputMethod();
    void sendKey(int key);

    QPointer<QQuickItem> m_surface;
    QSize m_surfacePixels;
    QString m_tail;
    Qt::InputMethodHints m_hints;
    int m_enter = MEGLER_ENTER_DONE;
    bool m_active = false;
    bool m_requested = false;
    bool m_seenVisible = false;
    int m_lastHeight = -1;
};

#endif
