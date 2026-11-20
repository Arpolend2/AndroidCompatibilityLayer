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
    void setRemote(bool remote) { m_remote = remote; }

    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

signals:
    void textCommitted(const QString &text);
    void textKey(int key);
    void keyboardClosed();
    void keyboardHeight(int height);

protected:
    void inputMethodEvent(QInputMethodEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

public slots:
    void showKeyboard(int kind, int flags, int enter);
    void hideKeyboard();

private slots:
    void visibilityChanged();
    void updateHeight();

private:
    static void MEGLER_CB requestShow(void *user, int kind, int flags, int enter);
    static void MEGLER_CB requestHide(void *user);
    void dismiss(bool notifyAndroid);
    void updateInputMethod();
    void sendKey(int key);
    void commitText(const QString &text);
    void sendHeight(int height);

    QPointer<QQuickItem> m_surface;
    QSize m_surfacePixels;
    QString m_tail;
    Qt::InputMethodHints m_hints;
    int m_enter = MEGLER_ENTER_DONE;
    bool m_active = false;
    bool m_remote = false;
    bool m_requested = false;
    bool m_seenVisible = false;
    int m_lastHeight = -1;
};

#endif
