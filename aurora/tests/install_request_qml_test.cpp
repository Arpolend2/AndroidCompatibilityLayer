#include <QColor>
#include <QGuiApplication>
#include <QMetaMethod>
#include <QMouseEvent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <QTranslator>
#include <QVariant>

#include "../side_mouse_buttons.h"
#include "../runtime_settings.h"

#include <cstdio>

class FakeProbe : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool previewRunning READ previewRunning NOTIFY previewRunningChanged)
    Q_PROPERTY(bool externalInstallBusy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QVariantList installedApps READ apps CONSTANT)
    Q_PROPERTY(QString result READ text CONSTANT)
    Q_PROPERTY(QString appResult READ empty CONSTANT)
    Q_PROPERTY(bool installConfirmationVisible READ visible NOTIFY visibilityChanged)
    Q_PROPERTY(QString installConfirmationToken READ text CONSTANT)
    Q_PROPERTY(QString installConfirmationLabel READ text CONSTANT)
    Q_PROPERTY(QString installConfirmationPackage READ text CONSTANT)
    Q_PROPERTY(quint32 installConfirmationVersion READ version CONSTANT)
    Q_PROPERTY(bool installConfirmationIsUpdate READ disabled CONSTANT)
    Q_PROPERTY(bool installConfirmationIsBundle READ disabled CONSTANT)
    Q_PROPERTY(QString deviceOperatingSystem READ text CONSTANT)
    Q_PROPERTY(QString deviceCpuArchitecture READ text CONSTANT)
    Q_PROPERTY(int renderSizeCap READ renderSizeCap NOTIFY runtimeSettingsChanged)
    Q_PROPERTY(bool standaloneRpmBusy READ rpmBusy NOTIFY rpmChanged)
    Q_PROPERTY(QString standaloneRpmStatus READ rpmStatus NOTIFY rpmChanged)
    Q_PROPERTY(bool standaloneRpmOpened READ rpmOpened NOTIFY rpmChanged)
    Q_PROPERTY(QString standaloneRpmPackage READ rpmTarget NOTIFY rpmChanged)
    Q_PROPERTY(QString androidLocale READ androidLocale NOTIFY runtimeSettingsChanged)
    Q_PROPERTY(QString systemAndroidLocale READ systemAndroidLocale CONSTANT)
    Q_PROPERTY(QString androidTheme READ androidTheme NOTIFY runtimeSettingsChanged)
    Q_PROPERTY(QString androidLocation READ androidLocation NOTIFY runtimeSettingsChanged)
    Q_PROPERTY(QString googleServicesProvider READ googleServicesProvider NOTIFY runtimeSettingsChanged)
    Q_PROPERTY(QString googleServicesGmsApkPath READ googleServicesGmsApkPath NOTIFY runtimeSettingsChanged)
    Q_PROPERTY(bool googleServicesBundled READ disabled CONSTANT)
    Q_PROPERTY(bool microgBusy READ disabled CONSTANT)
    Q_PROPERTY(QString microgStatus READ empty CONSTANT)
    Q_PROPERTY(bool microgDownloadActive READ disabled CONSTANT)
    Q_PROPERTY(int microgDownloadProgress READ unknownProgress CONSTANT)
    Q_PROPERTY(bool officialStoreBusy READ disabled CONSTANT)
    Q_PROPERTY(QString officialStoreStatus READ empty CONSTANT)
    Q_PROPERTY(bool officialStoreDownloadActive READ disabled CONSTANT)
    Q_PROPERTY(int officialStoreDownloadProgress READ unknownProgress CONSTANT)
    Q_PROPERTY(QString runtimeSettingsError READ empty CONSTANT)
    Q_PROPERTY(QString coreLog READ coreLog NOTIFY coreLogChanged)
    Q_PROPERTY(bool coreLogAvailable READ yes CONSTANT)
public:
    bool previewRunning() const { return m_previewRunning; }
    void setPreviewRunning(bool running) { m_previewRunning = running; emit previewRunningChanged(); }
    Q_INVOKABLE bool isAppRunning(const QString &package) const { return package == QLatin1String("org.example.other"); }
    bool busy() const { return m_busy; }
    void setBusy(bool busy) { m_busy = busy; emit busyChanged(); }
    bool yes() const { return true; }
    bool disabled() const { return false; }
    bool rpmBusy() const { return m_rpmBusy; }
    QString rpmStatus() const { return m_rpmStatus; }
    bool rpmOpened() const { return m_rpmOpened; }
    QString rpmTarget() const { return rpmPackage; }
    void setRpmState(bool busy, const QString &status, bool opened = false) {
        m_rpmBusy = busy;
        m_rpmStatus = status;
        m_rpmOpened = opened;
        emit rpmChanged();
    }
    int unknownProgress() const { return -1; }
    int renderSizeCap() const { return m_renderSizeCap; }
    QString androidLocale() const { return m_androidLocale; }
    QString systemAndroidLocale() const { return QStringLiteral("ru-RU"); }
    QString androidTheme() const { return m_androidTheme; }
    QString androidLocation() const { return m_androidLocation; }
    QString googleServicesProvider() const { return m_googleServicesProvider; }
    QString googleServicesGmsApkPath() const { return m_googleServicesGmsApkPath; }
    QString empty() const { return QString(); }
    QString coreLog() const { return m_coreLog; }
    Q_INVOKABLE QString saveCoreLog() { ++saveLogCount; return saveLogResult; }
    int saveLogCount = 0;
    QString saveLogResult = QStringLiteral("Saved for test");
    void setCoreLog(const QString &text) { m_coreLog = text; emit coreLogChanged(); }
    bool visible() const { return m_visible; }
    void setVisible(bool visible) { m_visible = visible; emit visibilityChanged(); }
    QVariantList apps() const {
        return {QVariantMap{{QStringLiteral("label"), QStringLiteral("Example")},
                            {QStringLiteral("package"), QStringLiteral("org.example.other")},
                            {QStringLiteral("permissions"), QVariantList{QStringLiteral("android.permission.CAMERA")}},
                            {QStringLiteral("permissionMaxSdk"), QVariantList{0}},
                            {QStringLiteral("icon"), QStringLiteral("data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAAIklEQVR42mNgoAb438DwHxdmWOCAG48aMGrAqAFUNoASAAC0gKuqwY9yQwAAAABJRU5ErkJggg==")},
                            {QStringLiteral("launchCount"), 1}},
                QVariantMap{{QStringLiteral("label"), QStringLiteral("example Two")},
                            {QStringLiteral("package"), QStringLiteral("org.example.second")},
                            {QStringLiteral("launchCount"), 4}},
                QVariantMap{{QStringLiteral("label"), QString::fromUtf8("Три")},
                            {QStringLiteral("package"), QStringLiteral("org.example.three")},
                            {QStringLiteral("launchCount"), 3}},
                QVariantMap{{QStringLiteral("label"), QString()},
                            {QStringLiteral("package"), QStringLiteral("org.example.four")},
                            {QStringLiteral("launchCount"), 2}},
                QVariantMap{{QStringLiteral("label"), QStringLiteral("Five")},
                            {QStringLiteral("package"), QStringLiteral("org.example.five")},
                            {QStringLiteral("launchCount"), 1}},
                QVariantMap{{QStringLiteral("label"), QStringLiteral("Six")},
                            {QStringLiteral("package"), QStringLiteral("org.example.six")},
                            {QStringLiteral("launchCount"), 1}},
                QVariantMap{{QStringLiteral("label"), QStringLiteral("Unused")},
                            {QStringLiteral("package"), QStringLiteral("org.example.unused")},
                            {QStringLiteral("launchCount"), 0}},
                QVariantMap{{QStringLiteral("label"), QStringLiteral("Aurora Store")},
                            {QStringLiteral("package"), QStringLiteral("com.aurora.store")},
                            {QStringLiteral("launchCount"), 100}}};
    }
    QString text() const { return QStringLiteral("test"); }
    quint32 version() const { return 1; }
    Q_INVOKABLE void setInstallUiReady(bool ready) { uiReady = ready; }
    Q_INVOKABLE void answerInstall(const QString &token, bool accepted) {
        answerToken = token;
        answer = accepted ? 1 : 0;
    }
    Q_INVOKABLE void setRenderSizeCap(int value) { m_renderSizeCap = value; emit runtimeSettingsChanged(); }
    Q_INVOKABLE void installStandaloneRpm(const QString &package) { rpmPackage = package; emit rpmChanged(); }
    Q_INVOKABLE QVariantList appPermissionGroups(const QString &package, const QVariantMap &app) {
        permissionInfoPackage = package;
        permissionInfo = app;
        return QVariantList() << QVariantMap{{QStringLiteral("group"), QStringLiteral("camera")},
                                             {QStringLiteral("title"), QStringLiteral("Camera")},
                                             {QStringLiteral("allowed"), true}};
    }
    Q_INVOKABLE bool setAppPermission(const QString &package, const QString &group, bool allowed) {
        permissionPackage = package;
        permissionGroup = group;
        permissionAllowed = allowed;
        return true;
    }
    Q_INVOKABLE void setAndroidLocale(const QString &value) { m_androidLocale = value; emit runtimeSettingsChanged(); }
    Q_INVOKABLE void setAndroidTheme(const QString &value) { m_androidTheme = value; emit runtimeSettingsChanged(); }
    Q_INVOKABLE bool validAndroidLocation(const QString &value) const {
        return RuntimeSettings::parseLocation(value.trimmed(), nullptr, nullptr);
    }
    Q_INVOKABLE void setAndroidLocation(const QString &value) { m_androidLocation = value; emit runtimeSettingsChanged(); }
    Q_INVOKABLE void setGoogleServicesProvider(const QString &value) { m_googleServicesProvider = value; emit runtimeSettingsChanged(); }
    Q_INVOKABLE void setGoogleServicesGmsApkPath(const QString &value) { m_googleServicesGmsApkPath = value; emit runtimeSettingsChanged(); }
    Q_INVOKABLE void openMicrog() { ++microgOpenCount; setGoogleServicesProvider(QStringLiteral("microg")); }
    Q_INVOKABLE void updateMicrog(const QString &path) { updatePath = path; updateCalled = true; }
    Q_INVOKABLE void openOfficialStore() {}
    Q_INVOKABLE void updateOfficialStore() {}
    Q_INVOKABLE void updateOfficialStoreFromLocal(const QString &) {}
    Q_INVOKABLE bool removeApp(const QString &package) { removedPackage = package; return true; }
    Q_INVOKABLE void runInstalled(const QString &package) { openedPackage = package; }
    bool uiReady = false;
    QString answerToken;
    int answer = -1;
    QString removedPackage;
    QString openedPackage;
    QString rpmPackage;
    QString permissionPackage;
    QString permissionInfoPackage;
    QVariantMap permissionInfo;
    QString permissionGroup;
    bool permissionAllowed = true;
    QString updatePath;
    bool updateCalled = false;
    int microgOpenCount = 0;
signals:
    void standaloneRpmError(const QString &appPackage, const QString &message);
    void signatureReminder();
    void visibilityChanged();
    void externalInstallNotice();
    void controllerCloseBlocked();
    void busyChanged();
    void coreLogChanged();
    void runtimeSettingsChanged();
    void previewRunningChanged();
    void rpmChanged();
    void microgDownloadNotification(const QString &message, int percent, bool complete);
    void officialStoreDownloadNotification(const QString &message, int percent, bool complete);
private:
    bool m_visible = true;
    bool m_busy = false;
    bool m_previewRunning = false;
    bool m_rpmBusy = false;
    bool m_rpmOpened = false;
    QString m_rpmStatus;
    int m_renderSizeCap = 0;
    QString m_androidLocale = QStringLiteral("en-US");
    QString m_androidTheme;
    QString m_androidLocation;
    QString m_googleServicesProvider = QStringLiteral("none");
    QString m_googleServicesGmsApkPath;
    QString m_coreLog;
};

static bool fits(const QQuickItem *item, const QQuickItem *parent)
{
    const QPointF topLeft = item->mapToItem(parent, QPointF());
    return item->isVisible() && item->width() > 0 && item->height() > 0 &&
           topLeft.x() >= 0 && topLeft.y() >= 0 &&
           topLeft.x() + item->width() <= parent->width() &&
           topLeft.y() + item->height() <= parent->height();
}

static bool click(QQuickItem *button)
{
    const QMetaObject *meta = button->metaObject();
    for (int i = 0; i < meta->methodCount(); ++i) {
        const QMetaMethod method = meta->method(i);
        if (method.methodType() == QMetaMethod::Signal && method.name() == "clicked"
            && method.parameterCount() == 1) {
            void *event = nullptr;
            const QByteArray type = method.parameterTypes().at(0);
            return method.invoke(button, Qt::DirectConnection, QGenericArgument(type.constData(), &event));
        }
    }
    return false;
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    qmlRegisterType<SideMouseButtons>("AndroidCompatibilityLayer.Navigation", 1, 0, "SideMouseButtons");
    if (argc != 2 && !(argc == 3 && (QByteArray(argv[2]) == "--permissions-only" ||
                                     QByteArray(argv[2]) == "--rpm-menu-only" ||
                                     QByteArray(argv[2]) == "--gesture-setting-only" ||
                                     QByteArray(argv[2]) == "--translation-only"))) return 2;
    QTranslator translator;
    if (argc == 3 && QByteArray(argv[2]) == "--translation-only" &&
        (!translator.load(QString::fromLocal8Bit(qgetenv("ANDROID_COMPATIBILITY_LAYER_TRANSLATION_QM"))) ||
         !app.installTranslator(&translator))) return 1;
    FakeProbe probe;
    {
        QQuickView view;
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.rootContext()->setContextProperty(QStringLiteral("probe"), &probe);
        view.resize(480, 854);
        view.setSource(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
        if (view.status() != QQuickView::Ready) {
            for (const QQmlError &error : view.errors())
                std::fprintf(stderr, "%s\n", qPrintable(error.toString()));
            return 1;
        }
        QTest::qWait(100);

        QObject *root = view.rootObject();
        if (argc == 3 && QByteArray(argv[2]) == "--translation-only") {
            QObject *button = root->findChild<QObject *>(QStringLiteral("installApk"));
            if (!button || button->property("text").toString() !=
                QString::fromUtf8("Добавить установочный пакет")) return 1;
            return 0;
        }
        auto prompt = root->findChild<QQuickItem *>(QStringLiteral("installConfirmation"));
        auto cancel = root->findChild<QQuickItem *>(QStringLiteral("cancelInstall"));
        auto confirm = root->findChild<QQuickItem *>(QStringLiteral("confirmInstall"));
        auto scroll = root->findChild<QQuickItem *>(QStringLiteral("installConfirmationScroll"));
        auto warning = root->findChild<QQuickItem *>(QStringLiteral("installWarning"));
        if (!probe.uiReady || !prompt || !cancel || !confirm || !scroll || !warning ||
            !prompt->isVisible() || !prompt->parentItem() ||
            !prompt->parentItem()->property("__silica_page").isValid() ||
            warning->property("text").toString().isEmpty()) {
            std::fprintf(stderr, "Confirmation or current page not ready\n");
            return 1;
        }

        QQuickItem *page = prompt->parentItem();
        QObject *addApk = root->findChild<QObject *>(QStringLiteral("installApk"));
        if (!addApk || root->findChild<QObject *>(QStringLiteral("addApkToLibrary")) ||
            addApk->property("text").toString() != QStringLiteral("Add APK or bundle")) {
            std::fprintf(stderr, "Library add action is not confined to the pulley menu\n");
            return 1;
        }
        QQuickItem *mainScroll = root->findChild<QQuickItem *>(QStringLiteral("mainScroll"));
        QQuickItem *navigationBackground = root->findChild<QQuickItem *>(QStringLiteral("navigationBackground"));
        if (!mainScroll || !mainScroll->clip() || !navigationBackground ||
            navigationBackground->property("color").value<QColor>().alpha() != 255) {
            std::fprintf(stderr, "Navigation background or content clipping is missing\n");
            return 1;
        }
        probe.setBusy(true);
        if (!page->property("busy").toBool()) return 1;
        probe.setBusy(false);
        if (page->property("busy").toBool()) return 1;
        if (root->property("allowedOrientations") != page->property("allowedOrientations") ||
            page->property("_allowedOrientations") != page->property("allowedOrientations")) {
            std::fprintf(stderr, "Confirmation is restricted by window orientation\n");
            return 1;
        }
        struct LayoutCase { int angle; int width; int height; };
        for (const LayoutCase &layout : {
                 LayoutCase{0, 480, 854}, LayoutCase{90, 854, 480},
                 LayoutCase{180, 480, 854}, LayoutCase{270, 854, 480},
                 LayoutCase{90, 320, 240}}) {
            page->setRotation(layout.angle);
            page->setWidth(layout.width);
            page->setHeight(layout.height);
            QCoreApplication::processEvents();
            if (prompt->parentItem() != page || prompt->width() != page->width() ||
                prompt->height() != page->height() || !fits(cancel, prompt) ||
                !fits(confirm, prompt) || scroll->height() <= 0) {
                std::fprintf(stderr, "Invalid confirmation geometry at %d degrees\n", layout.angle);
                std::fprintf(stderr, "page %.0fx%.0f prompt %.0fx%.0f cancel %d confirm %d scroll %.0f content %.0f warning %.0f\n",
                             page->width(), page->height(), prompt->width(), prompt->height(),
                             fits(cancel, prompt), fits(confirm, prompt), scroll->height(),
                             scroll->property("contentHeight").toReal(), warning->height());
                const QPointF pos = cancel->mapToItem(prompt, QPointF());
                std::fprintf(stderr, "visible page %d prompt %d cancel %d, cancel %.0f,%.0f %.0fx%.0f\n",
                             page->isVisible(), prompt->isVisible(), cancel->isVisible(), pos.x(), pos.y(),
                             cancel->width(), cancel->height());
                std::fprintf(stderr, "probe %d prompt property %d\n", probe.visible(), prompt->property("visible").toBool());
                return 1;
            }
            for (QQuickItem *button : {cancel, confirm}) {
                probe.answer = -1;
                if (!click(button) || probe.answer != (button == confirm ? 1 : 0) ||
                    probe.answerToken != QStringLiteral("test")) {
                    std::fprintf(stderr, "Button did not answer at %d degrees\n", layout.angle);
                    return 1;
                }
            }
        }
        QQmlExpression push(qmlContext(root), root,
            QStringLiteral("pageStack.push(Qt.createQmlObject('import QtQuick 2.0; import Sailfish.Silica 1.0; Page { allowedOrientations: Orientation.All }', pageStack), {}, PageStackAction.Immediate)"));
        push.evaluate();
        if (push.hasError()) {
            std::fprintf(stderr, "%s\n", qPrintable(push.error().toString()));
            return 1;
        }
        QCoreApplication::processEvents();
        if (!prompt->parentItem() || prompt->parentItem() == page ||
            !prompt->parentItem()->property("__silica_page").isValid()) {
            std::fprintf(stderr, "Confirmation did not follow the current page\n");
            return 1;
        }
        emit probe.externalInstallNotice();
        QCoreApplication::processEvents();
        if (prompt->parentItem() != page) {
            std::fprintf(stderr, "Error notice did not return to the library page\n");
            return 1;
        }
        probe.setVisible(false);
        if (prompt->isVisible() ||
            root->property("allowedOrientations") != root->property("defaultAllowedOrientations")) return 1;
        QObject *appsRepeater = root->findChild<QObject *>(QStringLiteral("installedAppsRepeater"));
        QQuickItem *appItem = nullptr;
        if (appsRepeater)
            QMetaObject::invokeMethod(appsRepeater, "itemAt", Q_RETURN_ARG(QQuickItem *, appItem), Q_ARG(int, 0));
        if (!appItem) {
            std::fprintf(stderr, "Library list does not contain its first app (repeater=%d count=%d)\n",
                         appsRepeater != nullptr, appsRepeater ? appsRepeater->property("count").toInt() : -1);
            return 1;
        }
        if (page->property("apps").toList().size() != 8 ||
            appsRepeater->property("count").toInt() != 8 ||
            root->findChild<QObject *>(QStringLiteral("openOfficialStore"))) {
            std::fprintf(stderr, "Library list is incorrect\n");
            return 1;
        }
        QObject *icon = appItem->findChild<QObject *>(QStringLiteral("libraryAppIcon"));
        QTest::qWait(100);
        if (!icon || icon->property("status").toInt() != 1) {
            std::fprintf(stderr, "APK icon did not load\n");
            return 1;
        }
        auto grid = root->findChild<QQuickItem *>(QStringLiteral("libraryGrid"));
        QQuickItem *second = nullptr;
        QQuickItem *third = nullptr;
        QMetaObject::invokeMethod(appsRepeater, "itemAt", Q_RETURN_ARG(QQuickItem *, second), Q_ARG(int, 1));
        QMetaObject::invokeMethod(appsRepeater, "itemAt", Q_RETURN_ARG(QQuickItem *, third), Q_ARG(int, 2));
        QQmlExpression expectedColumns(qmlContext(page), page,
            QStringLiteral("Screen.sizeCategory >= Screen.Large ? 2 : 1"));
        if (!grid || !second || !third || expectedColumns.hasError() ||
            grid->property("columns").toInt() != expectedColumns.evaluate().toInt() ||
            grid->x() <= 0 || grid->width() >= page->width()) {
            std::fprintf(stderr, "Library grid layout is incorrect (x=%.0f y=%.0f width=%.0f page=%.0f)\n",
                         grid ? grid->x() : -1, grid ? grid->y() : -1,
                         grid ? grid->width() : -1, page->width());
            return 1;
        }
        if (!grid->setProperty("columns", 2)) return 1;
        QTest::qWait(100);
        const qreal spacing = grid->property("spacing").toReal();
        if (spacing <= 0 || qAbs(appItem->width() * 2 + spacing - grid->width()) > 1 ||
            second->x() <= appItem->x() || second->y() != appItem->y() ||
            third->x() != appItem->x() || third->y() <= appItem->y()) {
            std::fprintf(stderr, "Tablet library layout is incorrect\n");
            return 1;
        }
        if (!grid->setProperty("columns", 1)) return 1;
        QTest::qWait(100);
        if (qAbs(appItem->width() - grid->width()) > 1 ||
            second->x() != appItem->x() || second->y() <= appItem->y()) {
            std::fprintf(stderr, "Phone library layout is incorrect\n");
            return 1;
        }
        if (!click(appItem)) {
            std::fprintf(stderr, "App library item could not be opened\n");
            return 1;
        }
        QTest::qWait(500);
        if (!root->findChild<QObject *>(QStringLiteral("appDetailsScroll")) ||
            prompt->parentItem()->property("appPackage").toString() != QStringLiteral("org.example.other") ||
            !probe.openedPackage.isEmpty()) {
            std::fprintf(stderr, "Opening an app from the library did not show its details\n");
            return 1;
        }
        if (argc == 3 && QByteArray(argv[2]) == "--permissions-only") {
            if (root->findChild<QObject *>(QStringLiteral("appPermissions")) ||
                probe.permissionInfoPackage != QStringLiteral("org.example.other") ||
                probe.permissionInfo.value(QStringLiteral("permissions")).toList().value(0).toString() !=
                    QStringLiteral("android.permission.CAMERA")) {
                std::fprintf(stderr, "Permissions were not loaded from the app details\n");
                return 1;
            }
            QObject *repeater = root->findChild<QObject *>(QStringLiteral("permissionsRepeater"));
            QQuickItem *toggle = nullptr;
            if (repeater)
                QMetaObject::invokeMethod(repeater, "itemAt", Q_RETURN_ARG(QQuickItem *, toggle), Q_ARG(int, 0));
            if (!toggle || !toggle->property("checked").toBool()) {
                std::fprintf(stderr, "Permission switch missing or not enabled\n");
                return 1;
            }
            if (!click(toggle)) {
                std::fprintf(stderr, "Could not change permission switch\n");
                return 1;
            }
            if (probe.permissionPackage != QStringLiteral("org.example.other") ||
                probe.permissionGroup != QStringLiteral("camera") || probe.permissionAllowed) {
                std::fprintf(stderr, "Permission action was not forwarded: package=%s group=%s allowed=%d checked=%d\n",
                             qPrintable(probe.permissionPackage), qPrintable(probe.permissionGroup),
                             probe.permissionAllowed, toggle->property("checked").toBool());
                return 1;
            }
            return 0;
        }
        QQuickItem *rpmItem = root->findChild<QQuickItem *>(QStringLiteral("installAsRpm"));
        QObject *detailsMenu = root->findChild<QObject *>(QStringLiteral("appDetailsPulleyMenu"));
        QQuickItem *remove = root->findChild<QQuickItem *>(QStringLiteral("removeFromLibrary"));
        if (!detailsMenu || !rpmItem || !remove ||
            detailsMenu->findChild<QObject *>(QStringLiteral("installAsRpm")) != rpmItem ||
            detailsMenu->findChild<QObject *>(QStringLiteral("removeFromLibrary")) != remove ||
            detailsMenu->findChild<QObject *>(QStringLiteral("appPermissions")) ||
            !rpmItem->property("enabled").toBool() || !QMetaObject::invokeMethod(rpmItem, "clicked") ||
            probe.rpmPackage != QStringLiteral("org.example.other")) {
            std::fprintf(stderr, "RPM action did not select the app\n");
            return 1;
        }
        probe.setRpmState(true, QStringLiteral("Creating standalone RPM..."));
        QCoreApplication::processEvents();
        QObject *rpmStatus = root->findChild<QObject *>(QStringLiteral("detailRpmStatus"));
        QObject *rpmProgress = root->findChild<QObject *>(QStringLiteral("detailRpmProgress"));
        if (rpmItem->property("enabled").toBool() || remove->property("enabled").toBool() || !rpmStatus ||
            !rpmStatus->property("visible").toBool() || !rpmProgress ||
            !rpmProgress->property("visible").toBool() || !rpmProgress->property("indeterminate").toBool()) {
            std::fprintf(stderr, "RPM creation status is not visible or the button remains enabled\n");
            return 1;
        }
        probe.setRpmState(false, QStringLiteral("Could not create RPM"));
        QCoreApplication::processEvents();
        if (!rpmStatus->property("visible").toBool() ||
            rpmStatus->property("text").toString() != QStringLiteral("Could not create RPM")) {
            std::fprintf(stderr, "RPM error is not visible on the app page\n");
            return 1;
        }
        QObject *rpmBanner = root->findChild<QObject *>(QStringLiteral("rpmErrorBanner"));
        if (!rpmBanner) return 1;
        const QString originalBanner = rpmBanner->property("previewBody").toString();
        emit probe.standaloneRpmError(QStringLiteral("org.example.other"), QStringLiteral("Visible on app page"));
        if (rpmBanner->property("previewBody").toString() != originalBanner) {
            std::fprintf(stderr, "RPM banner shown on the matching app page\n");
            return 1;
        }
        emit probe.standaloneRpmError(QStringLiteral("org.example.different"), QStringLiteral("Different app error"));
        if (rpmBanner->property("previewBody").toString() != QStringLiteral("Different app error")) {
            std::fprintf(stderr, "RPM banner missing for a different app\n");
            return 1;
        }
        probe.setRpmState(false, QString());
        QTest::qWait(100);
        if (argc == 3 && QByteArray(argv[2]) == "--rpm-menu-only") return 0;
        probe.setPreviewRunning(true);
        QTest::qWait(100);
        if (!remove || remove->property("enabled").toBool() ||
            root->findChild<QObject *>(QStringLiteral("forceCloseFromLibrary")) ||
            root->findChild<QObject *>(QStringLiteral("appInfoFromLibrary"))) {
            std::fprintf(stderr, "Running app actions are incorrect\n");
            return 1;
        }
        probe.setPreviewRunning(false);
        QCoreApplication::processEvents();
        if (root->findChild<QQuickItem *>(QStringLiteral("edgeOnlyBack"))) {
            std::fprintf(stderr, "Per-app back gesture setting is still present\n");
            return 1;
        }
        if (argc == 3 && QByteArray(argv[2]) == "--gesture-setting-only") return 0;
        if (!remove->property("enabled").toBool() || !QMetaObject::invokeMethod(remove, "clicked")) return 1;
        QTest::qWait(500);
        QObject *dialog = prompt->parentItem();
        if (!dialog || !QMetaObject::invokeMethod(dialog, "accept") ||
            probe.removedPackage != QStringLiteral("org.example.other")) {
            std::fprintf(stderr, "Library removal was not confirmed for the selected package\n");
            return 1;
        }
        QTest::qWait(500);
        if (prompt->parentItem() != page) {
            std::fprintf(stderr, "Removal did not return to the library\n");
            return 1;
        }
        if (root->findChild<QObject *>(QStringLiteral("standaloneRpmStatus"))) {
            std::fprintf(stderr, "RPM status is present in the library\n");
            return 1;
        }
        emit probe.standaloneRpmError(QStringLiteral("org.example.other"), QStringLiteral("Library error"));
        if (rpmBanner->property("previewBody").toString() != QStringLiteral("Library error")) {
            std::fprintf(stderr, "RPM banner missing on the library page\n");
            return 1;
        }
        QQuickItem *mainMenu = root->findChild<QQuickItem *>(QStringLiteral("mainPulleyMenu"));
        if (!mainMenu || !mainMenu->isVisible() ||
            !page->setProperty("section", 1) || mainMenu->isVisible() ||
            !page->setProperty("section", 3) || mainMenu->isVisible()) {
            std::fprintf(stderr, "Page menu visibility is incorrect\n");
            return 1;
        }
        QObject *cap = root->findChild<QObject *>(QStringLiteral("renderSize960"));
        QObject *resolution = root->findChild<QObject *>(QStringLiteral("renderSize"));
        QObject *localeCombo = root->findChild<QObject *>(QStringLiteral("androidLocale"));
        QObject *systemLocale = root->findChild<QObject *>(QStringLiteral("localeSystem"));
        QObject *otherLocale = root->findChild<QObject *>(QStringLiteral("localeOther"));
        QObject *themeDark = root->findChild<QObject *>(QStringLiteral("themeDark"));
        const bool capClicked = cap && QMetaObject::invokeMethod(cap, "clicked");
        if (!resolution || resolution->property("label").toString() != QStringLiteral("Resolution") ||
            !capClicked || !systemLocale || !QMetaObject::invokeMethod(systemLocale, "clicked") ||
            probe.renderSizeCap() != 960 || !probe.androidLocale().isEmpty() || !localeCombo ||
            localeCombo->property("currentIndex").toInt() != 0 ||
            !themeDark || !QMetaObject::invokeMethod(themeDark, "clicked") ||
            probe.androidTheme() != QStringLiteral("dark") ||
            !otherLocale || !QMetaObject::invokeMethod(otherLocale, "clicked")) {
            std::fprintf(stderr, "Settings controls failed\n");
            return 1;
        }
        QQmlExpression darkTheme(qmlContext(page), page,
                                 QStringLiteral("window.palette.colorScheme === Theme.LightOnDark"));
        if (!darkTheme.evaluate().toBool() || darkTheme.hasError()) {
            std::fprintf(stderr, "Manager theme did not follow the selected theme\n");
            return 1;
        }
        QColor darkBackground = page->property("backgroundColor").value<QColor>();
        if (darkBackground.alpha() != 255 ||
            prompt->parentItem()->property("backgroundColor").value<QColor>() != darkBackground) {
            std::fprintf(stderr, "Dark theme does not set an opaque dialog and page background\n");
            return 1;
        }
        QTest::qWait(500);
        QObject *localeInput = root->findChild<QObject *>(QStringLiteral("androidLocaleInput"));
        QObject *localeDialog = root->findChild<QObject *>(QStringLiteral("androidLocaleDialog"));
        if (!localeInput || localeInput->property("text").toString() != QStringLiteral("ru-RU") ||
            !localeInput->setProperty("text", QStringLiteral("de-DE")) ||
            !localeDialog || !QMetaObject::invokeMethod(localeDialog, "accept") ||
            probe.androidLocale() != QStringLiteral("de-DE")) {
            std::fprintf(stderr, "Custom locale dialog failed\n");
            return 1;
        }
        QTest::qWait(500);
        probe.setAndroidTheme(QStringLiteral("light"));
        QCoreApplication::processEvents();
        const QColor lightBackground = page->property("backgroundColor").value<QColor>();
        if (lightBackground.alpha() != 255 || lightBackground == darkBackground ||
            qGray(lightBackground.rgb()) <= qGray(darkBackground.rgb())) {
            std::fprintf(stderr, "Light and dark page backgrounds are incorrect\n");
            return 1;
        }
        probe.setAndroidTheme(QString());
        QCoreApplication::processEvents();
        if (page->property("backgroundColor").value<QColor>().alpha() != 0) {
            std::fprintf(stderr, "System theme did not retain the transparent background\n");
            return 1;
        }
        if (localeCombo->property("currentIndex").toInt() != 2) return 1;
        QObject *fixedLocation = root->findChild<QObject *>(QStringLiteral("locationFixed"));
        if (!fixedLocation || !QMetaObject::invokeMethod(fixedLocation, "clicked")) return 1;
        QTest::qWait(500);
        QObject *locationInput = root->findChild<QObject *>(QStringLiteral("androidLocationInput"));
        QObject *locationDialog = root->findChild<QObject *>(QStringLiteral("androidLocationDialog"));
        QObject *locationError = root->findChild<QObject *>(QStringLiteral("androidLocationValidationError"));
        if (!locationInput || !locationDialog || !locationError ||
            !locationInput->setProperty("text", QStringLiteral("100, 100")) ||
            locationDialog->property("canAccept").toBool() || !locationError->property("visible").toBool() ||
            !locationInput->setProperty("text", QStringLiteral("55.5, 37.5")) ||
            !locationDialog->property("canAccept").toBool() || locationError->property("visible").toBool() ||
            !QMetaObject::invokeMethod(locationDialog, "accept") ||
            probe.androidLocation() != QStringLiteral("55.5, 37.5")) return 1;
        QTest::qWait(500);
        probe.setAndroidTheme(QStringLiteral("dark"));
        probe.setAndroidLocale(QStringLiteral("fr-FR"));
        QTest::qWait(100);
        QObject *locationCombo = root->findChild<QObject *>(QStringLiteral("androidLocation"));
        if (probe.androidLocation() != QStringLiteral("55.5, 37.5") ||
            !locationCombo || locationCombo->property("currentIndex").toInt() != 1 ||
            locationCombo->property("value").toString() != QStringLiteral("55.5, 37.5")) {
            std::fprintf(stderr, "Fixed location did not remain selected or displayed: %s\n",
                         locationCombo ? qPrintable(locationCombo->property("value").toString()) : "(missing)");
            return 1;
        }
        if (!QMetaObject::invokeMethod(fixedLocation, "clicked")) return 1;
        QTest::qWait(500);
        locationInput = root->findChild<QObject *>(QStringLiteral("androidLocationInput"));
        locationDialog = root->findChild<QObject *>(QStringLiteral("androidLocationDialog"));
        if (!locationInput || locationInput->property("text").toString() != QStringLiteral("55.5, 37.5") ||
            !locationDialog || !QMetaObject::invokeMethod(locationDialog, "reject")) return 1;
        QTest::qWait(500);
        if (probe.androidLocation() != QStringLiteral("55.5, 37.5")) return 1;
        QObject *deviceLocation = root->findChild<QObject *>(QStringLiteral("locationDevice"));
        if (!deviceLocation || !QMetaObject::invokeMethod(deviceLocation, "clicked") ||
            !probe.androidLocation().isEmpty()) return 1;
        QQuickItem *storeEdgeSwitch = root->findChild<QQuickItem *>(QStringLiteral("storeEdgeOnlyBack"));
        if (storeEdgeSwitch) {
            std::fprintf(stderr, "Aurora Store must not expose an edge navigation setting\n");
            return 1;
        }
        if (root->findChild<QObject *>(QStringLiteral("favoriteAppLimit")) ||
            root->findChild<QObject *>(QStringLiteral("alwaysInstallRpm"))) return 1;
        page->setRotation(0);
        page->setWidth(480);
        page->setHeight(854);
        view.resize(480, 854);
        QTest::qWait(100);
        QQuickItem *microgSection = root->findChild<QQuickItem *>(QStringLiteral("microgSection"));
        QQuickItem *provider = root->findChild<QQuickItem *>(QStringLiteral("googleServicesProvider"));
        if (!microgSection || microgSection->isVisible() || !provider || !provider->isVisible()) {
            std::fprintf(stderr, "Disabled microG section or provider visibility is incorrect\n");
            return 1;
        }
        QObject *microgOption = root->findChild<QObject *>(QStringLiteral("microgProviderOption"));
        if (!microgOption || !QMetaObject::invokeMethod(microgOption, "clicked") ||
            probe.googleServicesProvider() != QStringLiteral("microg") || !microgSection->isVisible() ||
            probe.microgOpenCount != 0 || probe.updateCalled) {
            std::fprintf(stderr, "microG provider selection did not persist\n");
            return 1;
        }
        QQuickItem *updateMicrog = root->findChild<QQuickItem *>(QStringLiteral("downloadGoogleServices"));
        QQuickItem *openMicrog = root->findChild<QQuickItem *>(QStringLiteral("openGoogleServices"));
        QQuickItem *selectMicrog = root->findChild<QQuickItem *>(QStringLiteral("selectGoogleServicesApk"));
        QObject *configureLabel = openMicrog ? openMicrog->findChild<QObject *>(QStringLiteral("actionLabel")) : nullptr;
        QObject *downloadLabel = updateMicrog ? updateMicrog->findChild<QObject *>(QStringLiteral("actionLabel")) : nullptr;
        QObject *selectLabel = selectMicrog ? selectMicrog->findChild<QObject *>(QStringLiteral("actionLabel")) : nullptr;
        QObject *storeOnline = root->findChild<QObject *>(QStringLiteral("updateOfficialStoreOnline"));
        QObject *storeLocal = root->findChild<QObject *>(QStringLiteral("updateOfficialStoreLocal"));
        QObject *storeDownloadLabel = storeOnline ? storeOnline->findChild<QObject *>(QStringLiteral("actionLabel")) : nullptr;
        QObject *storeSelectLabel = storeLocal ? storeLocal->findChild<QObject *>(QStringLiteral("actionLabel")) : nullptr;
        if (!openMicrog || !updateMicrog || !selectMicrog ||
            openMicrog->parentItem() != updateMicrog->parentItem() ||
            selectMicrog->parentItem() != updateMicrog->parentItem() ||
            !configureLabel || configureLabel->property("text").toString() != QStringLiteral("Configure") ||
            !downloadLabel || downloadLabel->property("text").toString() != QStringLiteral("Download from GitHub") ||
            !selectLabel || selectLabel->property("text").toString() != QStringLiteral("Choose APK file") ||
            !storeDownloadLabel || storeDownloadLabel->property("text").toString() != QStringLiteral("Download from GitLab") ||
            !storeSelectLabel || storeSelectLabel->property("text").toString() != QStringLiteral("Choose APK file") ||
            !click(openMicrog) || probe.microgOpenCount != 1) {
            std::fprintf(stderr, "microG actions are not in the same row or open failed\n");
            return 1;
        }
        if (!updateMicrog || !click(updateMicrog) || !probe.updateCalled || !probe.updatePath.isEmpty()) {
            std::fprintf(stderr, "microG download was not requested\n");
            return 1;
        }
        view.resize(320, 854);
        page->setRotation(0);
        page->setWidth(320);
        page->setHeight(854);
        QTest::qWait(100);
        for (const char *name : {"openGoogleServices", "downloadGoogleServices", "selectGoogleServicesApk",
                                 "updateOfficialStoreOnline", "updateOfficialStoreLocal"}) {
            QQuickItem *button = root->findChild<QQuickItem *>(QString::fromLatin1(name));
            if (!button || !button->parentItem() || !fits(button, button->parentItem())) {
                std::fprintf(stderr, "Settings action does not fit: %s x=%g width=%g height=%g parent=%gx%g visible=%d\n",
                             name, button ? button->x() : -1, button ? button->width() : -1,
                             button ? button->height() : -1,
                             button && button->parentItem() ? button->parentItem()->width() : -1,
                             button && button->parentItem() ? button->parentItem()->height() : -1,
                             button && button->isVisible());
                return 1;
            }
            QQuickItem *label = button->findChild<QQuickItem *>(QStringLiteral("actionLabel"));
            if (!label || label->width() + 0.1 < label->property("paintedWidth").toReal() ||
                label->height() + 0.1 < label->property("paintedHeight").toReal()) {
                std::fprintf(stderr, "Settings action text is clipped: %s (%gx%g / %gx%g)\n", name,
                             label ? label->width() : -1, label ? label->height() : -1,
                             label ? label->property("paintedWidth").toReal() : -1,
                             label ? label->property("paintedHeight").toReal() : -1);
                return 1;
            }
        }
        if (openMicrog->x() + openMicrog->width() > updateMicrog->x() ||
            updateMicrog->x() + updateMicrog->width() > selectMicrog->x()) {
            std::fprintf(stderr, "microG actions overlap\n");
            return 1;
        }
        if (qAbs(openMicrog->width() - updateMicrog->width()) > 0.1 ||
            qAbs(openMicrog->width() - selectMicrog->width()) > 0.1 ||
            qAbs(openMicrog->height() - updateMicrog->height()) > 0.1 ||
            qAbs(openMicrog->height() - selectMicrog->height()) > 0.1) {
            std::fprintf(stderr, "microG actions have different sizes\n");
            return 1;
        }
        probe.setGoogleServicesProvider(QStringLiteral("none"));
        if (microgSection->isVisible() || !provider->isVisible()) {
            std::fprintf(stderr, "microG section remained visible after disabling provider\n");
            return 1;
        }
        view.resize(480, 854);
        QCoreApplication::processEvents();
        if (!page->setProperty("section", 2)) return 1;
        if (!mainMenu->isVisible()) {
            std::fprintf(stderr, "Log menu visibility is incorrect\n");
            return 1;
        }
        QQuickItem *copyLog = root->findChild<QQuickItem *>(QStringLiteral("copyLog"));
        QQuickItem *saveLog = root->findChild<QQuickItem *>(QStringLiteral("saveLog"));
        QQuickItem *shareLog = root->findChild<QQuickItem *>(QStringLiteral("shareLog"));
        if (!copyLog || !saveLog || !shareLog || copyLog->property("enabled").toBool() ||
            saveLog->property("text").toString() != QStringLiteral("Save to file") ||
            saveLog->property("enabled").toBool() ||
            shareLog->property("enabled").toBool()) {
            std::fprintf(stderr, "Empty log actions are not disabled\n");
            return 1;
        }
        probe.setCoreLog(QStringLiteral("first line\nsecond line\n"));
        QQuickItem *logText = root->findChild<QQuickItem *>(QStringLiteral("coreLogText"));
        if (!logText || logText->x() <= 0 ||
            logText->property("text").toString() != QStringLiteral("second line\nfirst line")) {
            std::fprintf(stderr, "Log is not displayed newest first\n");
            return 1;
        }
        if (!copyLog->property("enabled").toBool() || !shareLog->property("enabled").toBool() ||
            !QMetaObject::invokeMethod(copyLog, "clicked")) {
            std::fprintf(stderr, "Log pulley actions are not available\n");
            return 1;
        }
        QObject *logActionMessage = root->findChild<QObject *>(QStringLiteral("logActionMessage"));
        if (!saveLog->property("enabled").toBool() || !QMetaObject::invokeMethod(saveLog, "clicked") ||
            probe.saveLogCount != 1 || !logActionMessage ||
            logActionMessage->property("text").toString() != probe.saveLogResult) return 1;
        probe.saveLogResult = QStringLiteral("Save failed for test");
        if (!QMetaObject::invokeMethod(saveLog, "clicked") || probe.saveLogCount != 2 ||
            logActionMessage->property("text").toString() != probe.saveLogResult) return 1;
        QQmlExpression clipboard(qmlContext(root), root, QStringLiteral("Clipboard.text"));
        if (clipboard.evaluate().toString() != QStringLiteral("first line\nsecond line\n") || clipboard.hasError()) {
            std::fprintf(stderr, "Log pulley copy did not copy the log\n");
            return 1;
        }
        QQuickItem *follow = root->findChild<QQuickItem *>(QStringLiteral("logFollow"));
        if (!follow || follow->property("text").toString() != QStringLiteral("Hold") ||
            !QMetaObject::invokeMethod(follow, "clicked") || follow->property("text").toString() != QStringLiteral("Follow") ||
            !QMetaObject::invokeMethod(follow, "clicked") || follow->property("text").toString() != QStringLiteral("Hold")) {
            std::fprintf(stderr, "Log follow control did not toggle\n");
            return 1;
        }
        probe.setCoreLog(QStringLiteral("older line\n").repeated(200) + QStringLiteral("latest line"));
        QTest::qWait(100);
        mainScroll->setProperty("contentY", 100);
        probe.setCoreLog(probe.coreLog() + QStringLiteral("\nnewest line"));
        QTest::qWait(100);
        if (!logText->property("text").toString().startsWith(QStringLiteral("newest line\nlatest line\n")) ||
            mainScroll->property("contentY").toReal() > 0.1) {
            std::fprintf(stderr, "Log following did not return to the newest entries at the top\n");
            return 1;
        }
    }
    if (probe.uiReady) return 1;
    std::puts("Install confirmation QML OK");
    return 0;
}

#include "install_request_qml_test.moc"
