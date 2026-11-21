#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QImage>
#include <QTemporaryDir>
#include <QVariantMap>
#include <cstdio>

class Model : public QObject {
    Q_OBJECT
    Q_PROPERTY(int count READ count)
public:
    int count() const { return paths.size(); }
    Q_INVOKABLE QVariantMap get(int i) const { return {{"filePath", paths.at(i)}}; }
    QStringList paths;
};

class Picker : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString title MEMBER title)
    Q_PROPERTY(QStringList nameFilters MEMBER filters)
    Q_PROPERTY(QObject *selectedContent READ selectedContent CONSTANT)
    Q_PROPERTY(QVariantMap selectedContentProperties MEMBER properties NOTIFY selectedContentPropertiesChanged)
public:
    QObject *selectedContent() { return &model; }
    QString title;
    QStringList filters;
    QVariantMap properties;
    Model model;
signals:
    void accepted();
    void rejected();
    void selectedContentPropertiesChanged();
};

class Host : public QObject {
    Q_OBJECT
public:
    int selectedCount = 0, canceledCount = 0, failed = 0, confirmed = 0, validationCount = 0;
    bool validPhoto = true;
    QStringList paths;
    Q_INVOKABLE void setUiReady(bool) {}
    Q_INVOKABLE void selectedMany(const QString &, const QStringList &p) { ++selectedCount; paths = p; }
    Q_INVOKABLE void selected(const QString &, const QString &p) { ++selectedCount; paths = QStringList() << p; }
    Q_INVOKABLE void canceled(const QString &) { ++canceledCount; }
    Q_INVOKABLE void showFailed(const QString &) { ++failed; }
    Q_INVOKABLE void validatePhoto(const QString &token, const QString &) { ++validationCount; emit photoValidated(token, validPhoto); }
    Q_INVOKABLE void confirmPhoto(const QString &) { ++confirmed; }
signals:
    void showPicker(const QString &token, const QStringList &filters, bool multiple);
    void showPhoto(const QString &token, const QString &path);
    void closePhoto(const QString &token);
    void photoValidated(const QString &token, bool valid);
};

class Dialog : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(int allowedOrientations MEMBER allowedOrientations)
    Q_PROPERTY(int status MEMBER status NOTIFY statusChanged)
    Q_PROPERTY(bool canAccept MEMBER canAccept)
public:
    int allowedOrientations = 0, status = 0;
    bool canAccept = false;
    Q_INVOKABLE void reject() { emit rejected(); }
    Q_INVOKABLE void accept() { if (canAccept) emit accepted(); }
signals:
    void accepted();
    void rejected();
    void statusChanged();
};

class Header : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString title MEMBER title)
    Q_PROPERTY(QString acceptText MEMBER acceptText)
    Q_PROPERTY(QString cancelText MEMBER cancelText)
public:
    Header() { setHeight(80); }
    QString title, acceptText, cancelText;
};

class Button : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString text MEMBER text)
public:
    Button() { setWidth(200); setHeight(80); }
    QString text;
signals:
    void clicked();
};

class Capture : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready MEMBER ready)
public:
    bool ready = true;
    int canceled = 0;
    Q_INVOKABLE int captureToLocation(const QString &) { return 7; }
    Q_INVOKABLE void cancelCapture() { ++canceled; }
signals:
    void imageCaptured(int requestId, const QString &preview);
    void imageSaved(int requestId, const QString &path);
    void captureFailed(int requestId, const QString &message);
};

class Camera : public QObject {
    Q_OBJECT
    Q_PROPERTY(int captureMode MEMBER captureMode)
    Q_PROPERTY(int cameraState MEMBER cameraState)
    Q_PROPERTY(QString errorString MEMBER errorString)
    Q_PROPERTY(Capture *imageCapture READ imageCapture CONSTANT)
public:
    enum Mode { CaptureStillImage = 1, UnloadedState = 0 };
    Q_ENUM(Mode)
    int captureMode = 0, cameraState = 0, stopped = 0;
    QString errorString;
    Capture capture;
    Capture *imageCapture() { return &capture; }
    Q_INVOKABLE void stop() { ++stopped; }
    Q_INVOKABLE void start() {}
signals:
    void error();
};

class VideoOutput : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QObject *source MEMBER source)
    Q_PROPERTY(int fillMode MEMBER fillMode)
public:
    enum FillMode { PreserveAspectFit = 1 };
    Q_ENUM(FillMode)
    QObject *source = nullptr;
    int fillMode = 0;
};

class Enums : public QObject {
    Q_OBJECT
public:
    enum Values { All = 15, Deactivating = 3 };
    Q_ENUM(Values)
};

class Stack : public QObject {
    Q_OBJECT
public:
    Picker *current = nullptr;
    QObject *page = nullptr;
    Q_INVOKABLE QObject *push(QQmlComponent *component, const QVariantMap &properties) {
        QObject *obj = component->beginCreate(component->creationContext());
        if (!obj) return nullptr;
        for (auto i = properties.constBegin(); i != properties.constEnd(); ++i) obj->setProperty(i.key().toUtf8(), i.value());
        component->completeCreate();
        page = obj;
        if (QQuickItem *item = qobject_cast<QQuickItem *>(obj)) {
            item->setWidth(1200);
            item->setHeight(600);
        }
        current = qobject_cast<Picker *>(obj);
        return obj;
    }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc != 3) return 2;
    qmlRegisterType<Picker>("Sailfish.Pickers", 1, 0, "MultiFilePickerDialog");
    qmlRegisterType<Picker>("Sailfish.Pickers", 1, 0, "FilePickerPage");
    qmlRegisterType<Picker>("Sailfish.Silica", 1, 0, "Unused");
    qmlRegisterType<Dialog>("Sailfish.Silica", 1, 0, "Dialog");
    qmlRegisterType<Header>("Sailfish.Silica", 1, 0, "DialogHeader");
    qmlRegisterType<Button>("Sailfish.Silica", 1, 0, "Button");
    qmlRegisterSingletonType<Enums>("Sailfish.Silica", 1, 0, "Orientation",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new Enums; });
    qmlRegisterSingletonType<Enums>("Sailfish.Silica", 1, 0, "PageStatus",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new Enums; });
    const bool realMultimedia = !qgetenv("MEGLER_TEST_REAL_MULTIMEDIA").isEmpty();
    if (!realMultimedia) {
        qmlRegisterUncreatableType<Capture>("QtMultimedia", 5, 6, "Capture", "group only");
        qmlRegisterType<Camera>("QtMultimedia", 5, 6, "Camera");
        qmlRegisterType<VideoOutput>("QtMultimedia", 5, 6, "VideoOutput");
    }
    QQmlEngine engine;
    engine.addImportPath(QString::fromLocal8Bit(argv[2]));
    QQmlComponent component(&engine, QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
    Host host;
    Stack stack;
    QObject *root = component.beginCreate(engine.rootContext());
    if (!root) { for (const auto &error : component.errors()) std::fprintf(stderr, "%s\n", qPrintable(error.toString())); return 1; }
    root->setProperty("host", QVariant::fromValue(static_cast<QObject *>(&host)));
    root->setProperty("stack", QVariant::fromValue(static_cast<QObject *>(&stack)));
    component.completeCreate();
    if (realMultimedia) {
        emit host.showPhoto("real", "/tmp/photo.jpg");
        if (!stack.page) return 17;
        QCoreApplication::processEvents();
        delete stack.page;
        delete root;
        std::puts("QtMultimedia QML load OK");
        return 0;
    }
    emit host.showPicker("1", {"*.txt"}, true);
    if (!stack.current) return 3;
    stack.current->model.paths = QStringList() << "/one.txt" << "/two.txt";
    emit stack.current->accepted();
    emit stack.current->rejected();
    if (host.selectedCount != 1 || host.paths != QStringList({"/one.txt", "/two.txt"}) || host.canceledCount) return 4;
    delete stack.current;
    emit host.showPicker("2", {"*.txt"}, true);
    if (!stack.current) return 5;
    emit stack.current->rejected();
    delete stack.current;
    if (host.canceledCount != 1) return 6;
    emit host.showPicker("3", {"*.txt"}, false);
    if (!stack.current) return 7;
    stack.current->properties = {{"filePath", "/one.txt"}};
    emit stack.current->selectedContentPropertiesChanged();
    delete stack.current;
    if (host.selectedCount != 2 || host.paths != QStringList({"/one.txt"}) || host.canceledCount != 1 || host.failed) return 8;

    QTemporaryDir photoDir;
    const QString photoPath = photoDir.path() + QStringLiteral("/photo.jpg");
    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(Qt::red);
    if (!photoDir.isValid() || !image.save(photoPath, "JPEG")) return 18;
    emit host.showPhoto("4", photoPath);
    Dialog *dialog = qobject_cast<Dialog *>(stack.page);
    if (!dialog) return 9;
    Camera *camera = dialog->findChild<Camera *>("captureCamera");
    Button *button = dialog->findChild<Button *>("captureButton");
    QQuickItem *header = dialog->findChild<QQuickItem *>("captureHeader");
    QQuickItem *viewport = dialog->findChild<QQuickItem *>("captureViewport");
    QCoreApplication::processEvents();
    if (!camera || !button || !header || !viewport || !button->isEnabled() ||
        viewport->height() <= 0 || viewport->y() < header->height() ||
        viewport->y() + viewport->height() > button->y() ||
        button->y() + button->height() > dialog->height()) return 10;
    emit button->clicked();
    emit camera->capture.imageSaved(6, photoPath);
    if (dialog->canAccept) return 11;
    emit camera->capture.imageSaved(7, photoPath);
    QCoreApplication::processEvents();
    if (!dialog->canAccept || camera->stopped != 1) {
        std::fprintf(stderr, "photo state: canAccept=%d stopped=%d failed=%d validations=%d taking=%d\n",
                     dialog->canAccept, camera->stopped, host.failed, host.validationCount,
                     dialog->property("taking").toBool());
        return 12;
    }
    dialog->accept();
    delete dialog;
    if (host.confirmed != 1 || host.canceledCount != 1 || host.failed) return 13;

    emit host.showPhoto("5", photoPath);
    dialog = qobject_cast<Dialog *>(stack.page);
    camera = dialog ? dialog->findChild<Camera *>("captureCamera") : nullptr;
    if (!camera) return 14;
    emit host.closePhoto("5");
    if (camera->capture.canceled != 1 || camera->stopped != 1 || host.canceledCount != 2) return 15;
    delete dialog;
    if (host.canceledCount != 2) return 16;

    emit host.showPhoto("6", photoPath);
    dialog = qobject_cast<Dialog *>(stack.page);
    camera = dialog ? dialog->findChild<Camera *>("captureCamera") : nullptr;
    button = dialog ? dialog->findChild<Button *>("captureButton") : nullptr;
    if (!camera || !button) return 19;
    emit button->clicked();
    emit camera->capture.captureFailed(7, "backend error");
    delete dialog;
    if (host.failed != 1 || host.canceledCount != 2 || host.confirmed != 1) return 20;

    emit host.showPhoto("7", photoPath);
    dialog = qobject_cast<Dialog *>(stack.page);
    camera = dialog ? dialog->findChild<Camera *>("captureCamera") : nullptr;
    button = dialog ? dialog->findChild<Button *>("captureButton") : nullptr;
    if (!camera || !button) return 21;
    host.validPhoto = false;
    emit button->clicked();
    emit camera->capture.imageSaved(7, photoPath);
    delete dialog;
    if (host.failed != 2 || host.canceledCount != 2 || host.confirmed != 1) return 22;
    delete root;
    std::puts("AndroidFilePicker QML OK");
    return 0;
}

#include "android_file_picker_qml_test.moc"
