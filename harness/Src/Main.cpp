#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QEventLoop>
#include <QFileInfo>
#include <QIcon>
#include <QPixmap>
#include <QTimer>
#include <QDebug>

#include "BackendFactory.h"
#include "AppVersion.h"
#include "Icons.h"
#include "Log.h"
#include "MainWindow.h"
#include "Settings.h"
#include "Theme.h"

namespace {

QIcon buildAppIcon()
{
    return QIcon(Icons::pixmap(Icons::Sparkles, 64,
                               AppSettings::accentColor(AppSettings::instance().accent())));
}

/*!
 * Headless mode: `QwenImageHarness --run "a brass key" [options]`
 *
 * Runs exactly the same backend and graph builder as the GUI, prints the saved
 * paths on stdout and the backend log on stderr. Exit code 0 on success.
 */
int runHeadless(QApplication &app, const QCommandLineParser &parser, PageMode mode)
{
    AppSettings &settings = AppSettings::instance();
    if (parser.isSet("out"))
        settings.setOutputDir(parser.value("out"));
    settings.ensureDirectories();

    GenerationRequest req;
    req.mode = mode;
    req.prompt = parser.value("run");
    req.negativePrompt = parser.value("negative");
    req.size = QSize(parser.value("size").toInt(), parser.value("size").toInt());
    req.steps = parser.value("steps").toInt();
    req.cfg = parser.value("cfg").toDouble();
    req.seed = parser.isSet("seed") ? parser.value("seed").toLongLong() : -1;
    req.sampler = parser.value("sampler");

    if (parser.isSet("image"))
        req.images << QFileInfo(parser.value("image")).absoluteFilePath();
    for (const QString &extra : parser.values("ref"))
        req.images << QFileInfo(extra).absoluteFilePath();

    if (mode != PageMode::Generate && req.images.isEmpty()) {
        qCritical("mode '%s' needs --image", qPrintable(pageModeId(mode)));
        return 2;
    }
    if (mode == PageMode::MaskInfill) {
        if (!parser.isSet("mask")) {
            qCritical("mask mode needs --mask");
            return 2;
        }
        req.maskPath = QFileInfo(parser.value("mask")).absoluteFilePath();
    }

    Backend *backend = createBackend(&app);

    QObject::connect(backend, &Backend::stateChanged, [](int state) {
        qInfo("[backend] state -> %d", state);
    });
    QObject::connect(backend, &Backend::progress, [](const QString &, int pct, const QString &stage) {
        qInfo("[progress] %3d%% %s", pct, qPrintable(stage));
    });

    int exitCode = 0;
    QEventLoop loop;
    QObject::connect(backend, &Backend::jobFinished, [&](const GenerationResult &result) {
        if (result.cancelled) {
            qInfo("[done] cancelled");
        } else if (!result.error.isEmpty()) {
            qCritical("[error] %s", qPrintable(result.error));
            exitCode = 1;
        } else {
            for (const GeneratedImage &img : result.images)
                printf("%s\n", qPrintable(img.path));
            fflush(stdout);
            qInfo("[done] %d image(s)", result.images.size());
        }
        loop.quit();
    });
    QObject::connect(backend, &Backend::jobFailed, [&](const QString &, const QString &message) {
        qCritical("[failed] %s", qPrintable(message));
        exitCode = 1;
        loop.quit();
    });

    backend->startServer();

    // wait for the server, then submit
    QTimer submitTimer;
    submitTimer.setInterval(500);
    QObject::connect(&submitTimer, &QTimer::timeout, [&]() {
        if (backend->state() != Backend::Running)
            return;
        submitTimer.stop();
        qInfo("[run] %s", qPrintable(req.prompt));
        if (backend->submit(req).isEmpty()) {
            exitCode = 1;
            loop.quit();
        }
    });
    submitTimer.start();

    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, [&]() {
        qCritical("[failed] timeout");
        exitCode = 1;
        loop.quit();
    });
    guard.start(30 * 60 * 1000);

    loop.exec();
    backend->stopServer();
    return exitCode;
}

} // namespace

int main(int argc, char *argv[])
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
#endif
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, true);

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("Qwen");
    QCoreApplication::setApplicationName("Qwen Image Harness");
    QCoreApplication::setApplicationVersion(QString::fromLatin1(APP_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription("Qwen-Image-2.1 desktop frontend");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(QCommandLineOption("run", "Headless: run one job with this prompt.", "prompt"));
    parser.addOption(QCommandLineOption("mode", "generate|edit|faceswap|mask", "mode", "generate"));
    parser.addOption(QCommandLineOption("image", "Primary image (edit/faceswap/mask).", "path"));
    parser.addOption(QCommandLineOption("ref", "Reference image; repeatable.", "path"));
    parser.addOption(QCommandLineOption("mask", "Mask PNG, white = regenerate.", "path"));
    parser.addOption(QCommandLineOption("negative", "Negative prompt.", "text", ""));
    parser.addOption(QCommandLineOption("size", "Square size in pixels.", "px", "1024"));
    parser.addOption(QCommandLineOption("steps", "Sampling steps (<=12 picks the turbo checkpoint).", "n", "8"));
    parser.addOption(QCommandLineOption("cfg", "CFG scale.", "x", "1.0"));
    parser.addOption(QCommandLineOption("seed", "Fixed seed.", "n"));
    parser.addOption(QCommandLineOption("sampler", "Sampler name.", "name", "Euler"));
    parser.addOption(QCommandLineOption("out", "Output directory override.", "dir"));
    parser.process(app);

    Theme::apply();
    Theme::setAccent(AppSettings::accentColor(AppSettings::instance().accent()));
    AppSettings::instance().ensureDirectories();
    Log::instance().setLogFile(AppSettings::instance().logFilePath());
    Log::instance().setMirrorToFile(true);

    if (parser.isSet("run")) {
        const QString m = parser.value("mode").toLower();
        PageMode mode = PageMode::Generate;
        if (m == "edit")
            mode = PageMode::Edit;
        else if (m == "faceswap" || m == "face")
            mode = PageMode::FaceSwap;
        else if (m == "mask")
            mode = PageMode::MaskInfill;
        return runHeadless(app, parser, mode);
    }

    app.setWindowIcon(buildAppIcon());

    MainWindow window;
    window.show();

    return app.exec();
}
