#include "preview_keyboard.h"

#include <QDebug>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QQuickWindow>

namespace {

Qt::InputMethodHints inputHints(int kind, int flags)
{
    Qt::InputMethodHints hints = Qt::ImhNone;
    switch (kind) {
    case MEGLER_KEYBOARD_NUMBER:
        hints |= (flags & (MEGLER_KEYBOARD_F_DECIMAL | MEGLER_KEYBOARD_F_SIGNED))
                ? Qt::ImhFormattedNumbersOnly : Qt::ImhDigitsOnly;
        break;
    case MEGLER_KEYBOARD_PHONE: hints |= Qt::ImhDialableCharactersOnly; break;
    case MEGLER_KEYBOARD_EMAIL: hints |= Qt::ImhEmailCharactersOnly; break;
    case MEGLER_KEYBOARD_URL: hints |= Qt::ImhUrlCharactersOnly; break;
    case MEGLER_KEYBOARD_PASSWORD:
        hints |= Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText;
        break;
    case MEGLER_KEYBOARD_NUMBER_PASSWORD:
        hints |= Qt::ImhDigitsOnly | Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText;
        break;
    case MEGLER_KEYBOARD_DATETIME: hints |= Qt::ImhDate | Qt::ImhTime; break;
    default: break;
    }
    if (flags & MEGLER_KEYBOARD_F_MULTILINE) hints |= Qt::ImhMultiLine;
    if (flags & MEGLER_KEYBOARD_F_NO_SUGGESTIONS) hints |= Qt::ImhNoPredictiveText;
    if (flags & MEGLER_KEYBOARD_F_CAP_ALL) hints |= Qt::ImhUppercaseOnly;
    else if (!(flags & (MEGLER_KEYBOARD_F_CAP_SENTENCES | MEGLER_KEYBOARD_F_CAP_WORDS)))
        hints |= Qt::ImhNoAutoUppercase;
    return hints;
}

int enterKeyType(int enter)
{
    switch (enter) {
    case MEGLER_ENTER_NEWLINE: return Qt::EnterKeyReturn;
    case MEGLER_ENTER_GO: return Qt::EnterKeyGo;
    case MEGLER_ENTER_SEARCH: return Qt::EnterKeySearch;
    case MEGLER_ENTER_SEND: return Qt::EnterKeySend;
    case MEGLER_ENTER_NEXT: return Qt::EnterKeyNext;
    case MEGLER_ENTER_PREVIOUS: return Qt::EnterKeyPrevious;
    default: return Qt::EnterKeyDone;
    }
}

int scalarCount(const QString &text)
{
    int count = 0;
    for (int i = 0; i < text.size(); ++i, ++count) {
        if (text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate())
            ++i;
    }
    return count;
}

}

PreviewKeyboard::PreviewKeyboard(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemAcceptsInputMethod, true);
    setWidth(1);
    setHeight(1);
    QInputMethod *im = QGuiApplication::inputMethod();
    connect(im, &QInputMethod::visibleChanged, this, &PreviewKeyboard::visibilityChanged);
    connect(im, &QInputMethod::keyboardRectangleChanged, this, &PreviewKeyboard::updateHeight);
}

megler_keyboard_host PreviewKeyboard::host()
{
    megler_keyboard_host result = {};
    result.user = this;
    result.show = requestShow;
    result.hide = requestHide;
    return result;
}

void PreviewKeyboard::start()
{
    m_active = true;
    m_lastHeight = -1;
}

void PreviewKeyboard::stop()
{
    dismiss(false);
    m_active = false;
    if (m_surface) disconnect(m_surface, nullptr, this, nullptr);
    m_surface.clear();
}

bool PreviewKeyboard::back()
{
    if (!m_active || !m_requested) return false;
    dismiss(true);
    return true;
}

void PreviewKeyboard::setSurface(QQuickItem *surface, const QSize &pixels)
{
    if (m_surface != surface) {
        if (m_surface) disconnect(m_surface, nullptr, this, nullptr);
        m_surface = surface;
        if (surface) {
            connect(surface, &QQuickItem::widthChanged, this, &PreviewKeyboard::updateHeight);
            connect(surface, &QQuickItem::heightChanged, this, &PreviewKeyboard::updateHeight);
            connect(surface, &QQuickItem::xChanged, this, &PreviewKeyboard::updateHeight);
            connect(surface, &QQuickItem::yChanged, this, &PreviewKeyboard::updateHeight);
        }
    }
    m_surfacePixels = pixels;
    updateHeight();
}

void MEGLER_CB PreviewKeyboard::requestShow(void *user, int kind, int flags, int enter)
{
    QMetaObject::invokeMethod(static_cast<PreviewKeyboard *>(user), "showKeyboard", Qt::QueuedConnection,
                              Q_ARG(int, kind), Q_ARG(int, flags), Q_ARG(int, enter));
}

void MEGLER_CB PreviewKeyboard::requestHide(void *user)
{
    QMetaObject::invokeMethod(static_cast<PreviewKeyboard *>(user), "hideKeyboard", Qt::QueuedConnection);
}

void PreviewKeyboard::showKeyboard(int kind, int flags, int enter)
{
    if (!m_active) return;
    const bool again = m_requested && (flags & MEGLER_KEYBOARD_F_AGAIN);
    if (!again) {
        m_requested = false;
        QGuiApplication::inputMethod()->reset();
        m_tail.clear();
        m_hints = inputHints(kind, flags);
        m_enter = enter;
    }
    m_requested = true;
    forceActiveFocus();
    updateInputMethod();
    QGuiApplication::inputMethod()->show();
    updateHeight();
}

void PreviewKeyboard::hideKeyboard()
{
    if (m_active) dismiss(false);
}

void PreviewKeyboard::dismiss(bool notifyAndroid)
{
    const bool wasRequested = m_requested;
    m_requested = false;
    m_seenVisible = false;
    m_tail.clear();
    QGuiApplication::inputMethod()->reset();
    QGuiApplication::inputMethod()->hide();
    setFocus(false);
    if (m_active && wasRequested && notifyAndroid) {
        if (m_remote) emit keyboardClosed(); else megler_keyboard_closed();
    }
    if (m_active && wasRequested && m_lastHeight != 0) {
        sendHeight(0);
        m_lastHeight = 0;
    }
}

void PreviewKeyboard::visibilityChanged()
{
    if (!m_active || !m_requested) return;
    if (QGuiApplication::inputMethod()->isVisible()) {
        m_seenVisible = true;
        updateHeight();
    } else if (m_seenVisible) {
        dismiss(true);
    }
}

void PreviewKeyboard::updateHeight()
{
    if (!m_active || !m_requested) return;
    int height = 0;
    if (m_surface && !m_surfacePixels.isEmpty() && m_surface->width() > 0 && m_surface->height() > 0) {
        const QSizeF size = QSizeF(m_surfacePixels).scaled(QSizeF(m_surface->width(), m_surface->height()),
                                                          Qt::KeepAspectRatio);
        const QPointF topLeft = m_surface->mapToScene(QPointF((m_surface->width() - size.width()) / 2,
                                                             (m_surface->height() - size.height()) / 2));
        const QRectF target(topLeft, size);
        const QRectF keyboard = QGuiApplication::inputMethod()->keyboardRectangle();
        const QRectF overlap = target.intersected(keyboard);
        if (!overlap.isEmpty() && overlap.bottom() >= target.bottom() - 1)
            height = qBound(0, qRound(overlap.height() * m_surfacePixels.height() / size.height()), m_surfacePixels.height());
    }
    if (height != m_lastHeight) {
        sendHeight(height);
        m_lastHeight = height;
    }
}

QVariant PreviewKeyboard::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImEnabled: return m_active && m_requested;
    case Qt::ImHints: return int(m_hints);
    case Qt::ImEnterKeyType: return enterKeyType(m_enter);
    case Qt::ImSurroundingText: return m_tail;
    case Qt::ImCursorPosition:
    case Qt::ImAnchorPosition: return m_tail.size();
    case Qt::ImCurrentSelection: return QString();
    case Qt::ImCursorRectangle: return QRectF(0, 0, 1, 1);
    default: return QQuickItem::inputMethodQuery(query);
    }
}

void PreviewKeyboard::updateInputMethod()
{
    QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
}

void PreviewKeyboard::inputMethodEvent(QInputMethodEvent *event)
{
    if (!m_active || !m_requested) {
        event->ignore();
        return;
    }
    const int start = event->replacementStart();
    const int length = event->replacementLength();
    // Megler does not expose Android's surrounding text or an arbitrary replacement API.
    if (start > 0 || length < 0 || -qint64(start) > m_tail.size() ||
        qint64(start) + length != 0 ||
        (start < 0 && m_tail.at(m_tail.size() + start).isLowSurrogate())) {
        qWarning() << "Preview keyboard cannot apply an input method replacement outside its known text.";
        event->ignore();
        return;
    }
    for (const QInputMethodEvent::Attribute &attribute : event->attributes()) {
        if (attribute.type == QInputMethodEvent::Selection &&
            (qint64(attribute.start) != qint64(m_tail.size()) + start + event->commitString().size() ||
             attribute.length != 0)) {
            qWarning() << "Preview keyboard cannot apply an input method selection.";
            event->ignore();
            return;
        }
    }
    if (length) {
        const int from = m_tail.size() + start;
        const int count = scalarCount(m_tail.mid(from, length));
        for (int i = 0; i < count; ++i) {
            if (m_remote) emit textKey(MEGLER_TEXT_BACKSPACE); else megler_text_key(MEGLER_TEXT_BACKSPACE);
        }
        m_tail.truncate(from);
    }
    if (!event->commitString().isEmpty()) {
        commitText(event->commitString());
        m_tail += event->commitString();
    }
    updateInputMethod();
    event->accept();
}

void PreviewKeyboard::sendKey(int key)
{
    if (m_remote) emit textKey(key); else megler_text_key(key);
    if (key == MEGLER_TEXT_BACKSPACE && !m_tail.isEmpty()) {
        int from = m_tail.size() - 1;
        if (m_tail.at(from).isLowSurrogate() && from > 0 && m_tail.at(from - 1).isHighSurrogate()) --from;
        m_tail.truncate(from);
    } else if (key != MEGLER_TEXT_BACKSPACE) {
        m_tail.clear();
    }
    updateInputMethod();
}

void PreviewKeyboard::keyPressEvent(QKeyEvent *event)
{
    if (!m_active || !m_requested) {
        event->ignore();
        return;
    }
    int key = 0;
    switch (event->key()) {
    case Qt::Key_Backspace: key = MEGLER_TEXT_BACKSPACE; break;
    case Qt::Key_Delete: key = MEGLER_TEXT_DELETE; break;
    case Qt::Key_Return:
    case Qt::Key_Enter: key = MEGLER_TEXT_ENTER; break;
    case Qt::Key_Left: key = MEGLER_TEXT_LEFT; break;
    case Qt::Key_Right: key = MEGLER_TEXT_RIGHT; break;
    case Qt::Key_Up: key = MEGLER_TEXT_UP; break;
    case Qt::Key_Down: key = MEGLER_TEXT_DOWN; break;
    case Qt::Key_Home: key = MEGLER_TEXT_HOME; break;
    case Qt::Key_End: key = MEGLER_TEXT_END; break;
    case Qt::Key_Escape:
    case Qt::Key_Back: back(); event->accept(); return;
    default: break;
    }
    if (key) {
        sendKey(key);
    } else if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
               && !event->text().isEmpty() && event->text().at(0).isPrint()) {
        commitText(event->text());
        m_tail += event->text();
        updateInputMethod();
    } else {
        event->ignore();
        return;
    }
    event->accept();
}

void PreviewKeyboard::commitText(const QString &text)
{
    if (m_remote) emit textCommitted(text);
    else {
        const QByteArray utf8 = text.toUtf8();
        megler_text_commit(utf8.constData());
    }
}

void PreviewKeyboard::sendHeight(int height)
{
    if (m_remote) emit keyboardHeight(height); else megler_keyboard_height(height);
}
