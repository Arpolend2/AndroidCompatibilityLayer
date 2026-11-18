#include "../preview_keyboard.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>

#include <cstdlib>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
std::vector<QString> calls;
}

extern "C" void megler_text_commit(const char *text) { calls.push_back(QStringLiteral("text:") + QString::fromUtf8(text)); }
extern "C" void megler_text_key(int key) { calls.push_back(QStringLiteral("key:") + QString::number(key)); }
extern "C" void megler_keyboard_closed() { calls.push_back(QStringLiteral("closed")); }
extern "C" void megler_keyboard_height(int height) { calls.push_back(QStringLiteral("height:") + QString::number(height)); }

static void check(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QQuickItem root;
    PreviewKeyboard keyboard(&root);
    keyboard.start();
    const megler_keyboard_host host = keyboard.host();
    std::thread worker([&host]() {
        host.show(host.user, MEGLER_KEYBOARD_NUMBER, MEGLER_KEYBOARD_F_DECIMAL, MEGLER_ENTER_NEXT);
    });
    worker.join();
    QCoreApplication::processEvents();
    check(keyboard.inputMethodQuery(Qt::ImEnabled).toBool(), "Queued show was not applied");
    check(keyboard.inputMethodQuery(Qt::ImHints).toInt() & Qt::ImhFormattedNumbersOnly, "Number hints missing");
    check(keyboard.inputMethodQuery(Qt::ImEnterKeyType).toInt() == Qt::EnterKeyNext, "Enter hint missing");
    calls.clear();

    QInputMethodEvent composing(QStringLiteral("draft"), {});
    QCoreApplication::sendEvent(&keyboard, &composing);
    check(calls.empty(), "Preedit was committed prematurely");
    host.show(host.user, MEGLER_KEYBOARD_NUMBER, MEGLER_KEYBOARD_F_DECIMAL | MEGLER_KEYBOARD_F_AGAIN, MEGLER_ENTER_NEXT);
    QCoreApplication::processEvents();
    QInputMethodEvent commit;
    commit.setCommitString(QString::fromUtf8("A😀"));
    QCoreApplication::sendEvent(&keyboard, &commit);
    check(calls.size() == 1 && calls[0] == QString::fromUtf8("text:A😀"), "UTF-8 commit failed");

    QInputMethodEvent replace;
    replace.setCommitString(QStringLiteral("B"), -2, 2);
    QCoreApplication::sendEvent(&keyboard, &replace);
    check(calls.size() == 3 && calls[1] == QStringLiteral("key:1") && calls[2] == QStringLiteral("text:B"),
          "Surrogate replacement did not emit one Backspace followed by text");
    check(keyboard.inputMethodQuery(Qt::ImSurroundingText).toString() == QStringLiteral("AB"), "Tail is incorrect");

    QInputMethodEvent unsupported;
    unsupported.setCommitString(QStringLiteral("wrong"), -3, 3);
    QCoreApplication::sendEvent(&keyboard, &unsupported);
    check(calls.size() == 3 && !unsupported.isAccepted(), "Unknown replacement was not rejected");

    QList<QInputMethodEvent::Attribute> attributes;
    attributes.append(QInputMethodEvent::Attribute(QInputMethodEvent::Selection, 0, 1, QVariant()));
    QInputMethodEvent selection(QString(), attributes);
    selection.setCommitString(QStringLiteral("wrong"));
    QCoreApplication::sendEvent(&keyboard, &selection);
    check(calls.size() == 3 && !selection.isAccepted(), "Unknown selection was not rejected");

    host.show(host.user, MEGLER_KEYBOARD_NUMBER, MEGLER_KEYBOARD_F_DECIMAL | MEGLER_KEYBOARD_F_AGAIN, MEGLER_ENTER_NEXT);
    QCoreApplication::processEvents();
    check(keyboard.inputMethodQuery(Qt::ImSurroundingText).toString() == QStringLiteral("AB"), "Repeated show lost context");
    host.show(host.user, MEGLER_KEYBOARD_PASSWORD, 0, MEGLER_ENTER_DONE);
    QCoreApplication::processEvents();
    check(keyboard.inputMethodQuery(Qt::ImSurroundingText).toString().isEmpty(), "New field retained old context");
    check(keyboard.inputMethodQuery(Qt::ImHints).toInt() & Qt::ImhHiddenText, "Password hint missing");

    QKeyEvent backspace(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    QCoreApplication::sendEvent(&keyboard, &backspace);
    check(calls.back() == QStringLiteral("key:1"), "Backspace not forwarded");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(&keyboard, &enter);
    check(calls.back() == QStringLiteral("key:2"), "Enter not forwarded");
    check(keyboard.back() && calls.back() == QStringLiteral("closed"), "Back did not close the keyboard");
    check(!keyboard.back(), "Back was consumed twice");

    host.show(host.user, MEGLER_KEYBOARD_PASSWORD, 0, MEGLER_ENTER_DONE);
    QCoreApplication::processEvents();
    calls.clear();
    host.hide(host.user);
    QCoreApplication::processEvents();
    check(!keyboard.inputMethodQuery(Qt::ImEnabled).toBool() && calls.empty(), "Android hide echoed a user close");

    host.show(host.user, MEGLER_KEYBOARD_PASSWORD, 0, MEGLER_ENTER_DONE);
    keyboard.stop();
    QCoreApplication::processEvents();
    check(!keyboard.inputMethodQuery(Qt::ImEnabled).toBool(), "Queued show survived stop");
    std::puts("Preview keyboard OK");
    return 0;
}
