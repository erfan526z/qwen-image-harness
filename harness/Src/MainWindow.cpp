#include "MainWindow.h"

#include "AppVersion.h"
#include "BackendFactory.h"
#include "CreatePage.h"
#include "GalleryPage.h"
#include "Icons.h"
#include "Log.h"
#include "Settings.h"
#include "SettingsPage.h"
#include "Shell.h"
#include "TerminalPage.h"
#include "Theme.h"
#include "Viewer.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QShortcut>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

namespace {

const int GripSize = 6;
const int CornerSize = 12;

enum EdgeFlag {
    EdgeNone = 0,
    EdgeLeft = 1,
    EdgeRight = 2,
    EdgeTop = 4,
    EdgeBottom = 8,
    EdgeTopLeft = EdgeTop | EdgeLeft,
    EdgeTopRight = EdgeTop | EdgeRight,
    EdgeBottomLeft = EdgeBottom | EdgeLeft,
    EdgeBottomRight = EdgeBottom | EdgeRight
};

Qt::Edges qtEdges(int edge)
{
    Qt::Edges e;
    if (edge & EdgeLeft)
        e |= Qt::LeftEdge;
    if (edge & EdgeRight)
        e |= Qt::RightEdge;
    if (edge & EdgeTop)
        e |= Qt::TopEdge;
    if (edge & EdgeBottom)
        e |= Qt::BottomEdge;
    return e;
}

Qt::CursorShape cursorFor(int edge)
{
    switch (edge) {
    case EdgeLeft:
    case EdgeRight:
        return Qt::SizeHorCursor;
    case EdgeTop:
    case EdgeBottom:
        return Qt::SizeVerCursor;
    case EdgeTopLeft:
    case EdgeBottomRight:
        return Qt::SizeFDiagCursor;
    default:
        return Qt::SizeBDiagCursor;
    }
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_titleBar(nullptr)
    , m_sidebar(nullptr)
    , m_status(nullptr)
    , m_toast(nullptr)
    , m_viewer(nullptr)
    , m_pages(nullptr)
    , m_gallery(nullptr)
    , m_terminal(nullptr)
    , m_settings(nullptr)
    , m_backend(nullptr)
    , m_resizing(false)
    , m_resizeEdge(EdgeNone)
{
    setWindowTitle(QString("Qwen Image Harness %1").arg(QString::fromLatin1(APP_VERSION)));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setMinimumSize(1300, 700);

    m_backend = createBackend(this);

    buildUi();
    buildGrips();

    connect(m_backend, &Backend::stateChanged, this, &MainWindow::onBackendState);
    connect(m_backend, &Backend::progress, this, &MainWindow::onProgress);
    connect(m_backend, &Backend::jobFinished, this, &MainWindow::onJobFinished);
    connect(m_backend, &Backend::jobFailed, this, &MainWindow::onJobFailed);

    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect avail = screen->availableGeometry();
        const int w = qMin(1440, avail.width() - 60);
        const int h = qMin(900, avail.height() - 40);
        resize(qMax(1300, w), qMax(700, h));
        move(avail.center() - rect().center());
    }

    updateBackendState();

    const QString last = AppSettings::instance().lastPage();
    goToPage(last.isEmpty() ? QString("generate") : last);

    AppSettings &settings = AppSettings::instance();
    const int removed = settings.applyRetention();
    if (removed > 0)
        Logging::info(QString("Retention removed %1 old image(s)").arg(removed));

    Logging::info("Qwen Image Harness ready");

    if (settings.server().autoStart)
        QTimer::singleShot(400, this, [this]() { toggleServer(); });
}

MainWindow::~MainWindow()
{
}

QString MainWindow::pageIdForIndex(int index) const
{
    if (index < 0 || index >= m_pageIds.size())
        return QString();
    return m_pageIds.at(index);
}

void MainWindow::buildUi()
{
    QWidget *root = new QWidget(this);
    root->setObjectName("Root");
    setCentralWidget(root);

    QVBoxLayout *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    m_titleBar = new TitleBar(root);
    rootLayout->addWidget(m_titleBar);

    QHBoxLayout *body = new QHBoxLayout();
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);

    m_sidebar = new Sidebar(root);
    body->addWidget(m_sidebar);

    m_pages = new QStackedWidget(root);
    m_pages->setObjectName("ContentArea");
    body->addWidget(m_pages, 1);

    rootLayout->addLayout(body, 1);

    m_status = new StatusStrip(root);
    rootLayout->addWidget(m_status);

    buildPages();
    buildShortcuts();

    m_viewer = new ImageViewerOverlay(root);
    m_viewer->hide();
    connect(m_viewer, &ImageViewerOverlay::discardRequested, this, &MainWindow::onDiscardRequested);
    connect(m_viewer, &ImageViewerOverlay::revealRequested, this, &MainWindow::onRevealRequested);

    m_toast = new Toast(root);
    m_toast->hide();

    connect(m_titleBar, &TitleBar::minimizeRequested, this, &QWidget::showMinimized);
    connect(m_titleBar, &TitleBar::maximizeRequested, this, [this]() {
        if (isMaximized())
            showNormal();
        else
            showMaximized();
        updateMaximizedState();
    });
    connect(m_titleBar, &TitleBar::closeRequested, this, &QWidget::close);
    connect(m_sidebar, &Sidebar::pageRequested, this, &MainWindow::goToPage);
    connect(m_sidebar, &Sidebar::toggleServerRequested, this, &MainWindow::toggleServer);
    connect(m_status, &StatusStrip::cancelRequested, this, [this]() {
        if (!m_activeJobId.isEmpty())
            m_backend->cancel(m_activeJobId);
    });
}

void MainWindow::buildPages()
{
    const PageMode modes[] = {PageMode::Generate, PageMode::Edit,
                              PageMode::FaceSwap, PageMode::MaskInfill};

    for (int i = 0; i < 4; ++i) {
        CreatePage *page = new CreatePage(modes[i], m_pages);
        page->applyDefaults(AppSettings::instance().defaults());
        m_pages->addWidget(page);
        m_pageIds.append(page->pageId());
        m_createPages.append(page);

        connect(page, &CreatePage::submitRequested, this, &MainWindow::onJobRequested);
        connect(page, &CreatePage::viewRequested, this, &MainWindow::onViewRequested);
        connect(page, &CreatePage::revealRequested, this, &MainWindow::onRevealRequested);
        connect(page, &CreatePage::galleryRequested, this, [this]() { goToPage("gallery"); });
        connect(page, &CreatePage::cancelRequested, this, [this]() {
            if (!m_activeJobId.isEmpty())
                m_backend->cancel(m_activeJobId);
        });
        connect(page, &CreatePage::discardRequested, this, &MainWindow::onDiscardRequested);
    }

    m_gallery = new GalleryPage(m_pages);
    m_pages->addWidget(m_gallery);
    m_pageIds.append("gallery");
    connect(m_gallery, &GalleryPage::viewRequested, this, &MainWindow::onViewRequested);
    connect(m_gallery, &GalleryPage::revealRequested, this, &MainWindow::onRevealRequested);
    connect(m_gallery, &GalleryPage::discardRequested, this, &MainWindow::onDiscardRequested);

    m_terminal = new TerminalPage(m_pages);
    m_pages->addWidget(m_terminal);
    m_pageIds.append("terminal");

    m_settings = new SettingsPage(m_pages);
    m_pages->addWidget(m_settings);
    m_pageIds.append("settings");
    connect(m_settings, &SettingsPage::saved, this, &MainWindow::onSettingsSaved);
    connect(m_settings, &SettingsPage::outputCleared, this, &MainWindow::onOutputCleared);
}

void MainWindow::buildShortcuts()
{
    for (int i = 0; i < m_pageIds.size() && i < 9; ++i) {
        const QString pageId = m_pageIds.at(i);
        QShortcut *shortcut = new QShortcut(QKeySequence(QString("Ctrl+%1").arg(i + 1)), this);
        connect(shortcut, &QShortcut::activated, this, [this, pageId]() { goToPage(pageId); });
    }
}

void MainWindow::buildGrips()
{
    const int edges[] = {EdgeLeft, EdgeRight, EdgeTop, EdgeBottom,
                         EdgeTopLeft, EdgeTopRight, EdgeBottomLeft, EdgeBottomRight};

    for (int i = 0; i < 8; ++i) {
        QWidget *grip = new QWidget(this);
        grip->setObjectName("Grip");
        grip->setProperty("edge", int(edges[i]));
        grip->setCursor(cursorFor(edges[i]));
        grip->setMouseTracking(true);
        grip->installEventFilter(this);
        grip->raise();
        m_grips.append(grip);
    }
    layoutGrips();
}

void MainWindow::layoutGrips()
{
    if (m_grips.size() < 8)
        return;

    const int w = width();
    const int h = height();

    m_grips[0]->setGeometry(0, CornerSize, GripSize, h - 2 * CornerSize);
    m_grips[1]->setGeometry(w - GripSize, CornerSize, GripSize, h - 2 * CornerSize);
    m_grips[2]->setGeometry(CornerSize, 0, w - 2 * CornerSize, GripSize);
    m_grips[3]->setGeometry(CornerSize, h - GripSize, w - 2 * CornerSize, GripSize);
    m_grips[4]->setGeometry(0, 0, CornerSize, CornerSize);
    m_grips[5]->setGeometry(w - CornerSize, 0, CornerSize, CornerSize);
    m_grips[6]->setGeometry(0, h - CornerSize, CornerSize, CornerSize);
    m_grips[7]->setGeometry(w - CornerSize, h - CornerSize, CornerSize, CornerSize);

    for (int i = 0; i < m_grips.size(); ++i) {
        m_grips[i]->setVisible(!isMaximized());
        m_grips[i]->raise();
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    QWidget *grip = qobject_cast<QWidget *>(watched);
    if (grip && m_grips.contains(grip)) {
        const int edge = grip->property("edge").toInt();

        if (event->type() == QEvent::MouseButtonPress) {
            QMouseEvent *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                m_resizing = true;
                m_resizeEdge = edge;
                m_resizeStartPos = me->globalPos();
                m_resizeStartGeom = geometry();
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
                if (QWindow *handle = windowHandle()) {
                    handle->startSystemResize(qtEdges(edge));
                    m_resizing = false;
                }
#endif
                return true;
            }
        } else if (event->type() == QEvent::MouseMove && m_resizing) {
            QMouseEvent *me = static_cast<QMouseEvent *>(event);
            const QPoint d = me->globalPos() - m_resizeStartPos;
            QRect g = m_resizeStartGeom;

            int l = g.left();
            int t = g.top();
            int r = g.right();
            int b = g.bottom();

            if (m_resizeEdge & EdgeLeft)
                l += d.x();
            if (m_resizeEdge & EdgeRight)
                r += d.x();
            if (m_resizeEdge & EdgeTop)
                t += d.y();
            if (m_resizeEdge & EdgeBottom)
                b += d.y();

            if (r - l + 1 < minimumWidth()) {
                if (m_resizeEdge & EdgeLeft)
                    l = r - minimumWidth() + 1;
                else
                    r = l + minimumWidth() - 1;
            }
            if (b - t + 1 < minimumHeight()) {
                if (m_resizeEdge & EdgeTop)
                    t = b - minimumHeight() + 1;
                else
                    b = t + minimumHeight() - 1;
            }

            setGeometry(QRect(QPoint(l, t), QPoint(r, b)));
            return true;
        } else if (event->type() == QEvent::MouseButtonRelease) {
            if (m_resizing) {
                m_resizing = false;
                return true;
            }
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (!m_grips.isEmpty())
        layoutGrips();

    QWidget *host = centralWidget();
    if (m_viewer && m_viewer->isVisible() && host)
        m_viewer->setGeometry(host->rect());

    if (m_toast && m_toast->isVisible() && host)
        m_toast->move((host->width() - m_toast->width()) / 2,
                      host->height() - m_toast->height() - 34 - 14);
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange)
        updateMaximizedState();
}

void MainWindow::updateMaximizedState()
{
    m_titleBar->setMaximized(isMaximized());
    if (!m_grips.isEmpty())
        layoutGrips();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    AppSettings &settings = AppSettings::instance();
    settings.setLastPage(pageIdForIndex(m_pages->currentIndex()));
    settings.save();

    if (settings.server().stopOnExit && m_backend->state() != Backend::Stopped)
        m_backend->stopServer();

    event->accept();
}

void MainWindow::goToPage(const QString &pageId)
{
    const int index = m_pageIds.indexOf(pageId);
    if (index < 0)
        return;
    m_pages->setCurrentIndex(index);
    m_sidebar->setActive(pageId);
    AppSettings::instance().setLastPage(pageId);
}

CreatePage *MainWindow::pageForId(const QString &pageId) const
{
    for (int i = 0; i < m_createPages.size(); ++i) {
        if (m_createPages.at(i)->pageId() == pageId)
            return m_createPages.at(i);
    }
    return nullptr;
}

void MainWindow::updateBackendState()
{
    const int state = int(m_backend->state());
    const int port = AppSettings::instance().server().port;
    m_sidebar->setBackendState(state, port);

    const bool ready = (state == Backend::Running);
    for (int i = 0; i < m_createPages.size(); ++i)
        m_createPages.at(i)->setBackendReady(ready);
}

void MainWindow::toggleServer()
{
    if (m_backend->state() == Backend::Running)
        m_backend->stopServer();
    else if (m_backend->state() == Backend::Stopped || m_backend->state() == Backend::Failed)
        m_backend->startServer();
}

void MainWindow::onBackendState(int state)
{
    updateBackendState();

    switch (state) {
    case Backend::Running:
        m_status->setServerColor(Theme::Ok);
        showToast("Backend is running", 1);
        break;
    case Backend::Stopped:
        m_status->setServerColor(Theme::TextFaint);
        break;
    case Backend::Failed:
        m_status->setServerColor(Theme::Err);
        showToast("Backend failed to start", 2);
        break;
    case Backend::Starting:
    case Backend::Stopping:
        m_status->setServerColor(Theme::Warn);
        break;
    default:
        break;
    }
}

void MainWindow::onJobRequested(const GenerationRequest &request, const QString &pageId)
{
    if (m_backend->state() != Backend::Running) {
        showToast("Start the backend first", 0);
        Logging::warn("Job request ignored: backend is not running");
        return;
    }

    if (!m_activeJobId.isEmpty()) {
        showToast("A job is already running", 0);
        return;
    }

    const QString jobId = m_backend->submit(request);
    if (jobId.isEmpty()) {
        showToast("Could not queue the job", 2);
        return;
    }

    m_activeJobId = jobId;
    m_jobPageId = pageId;

    CreatePage *page = pageForId(pageId);
    if (page)
        page->setBusy(true);

    m_status->setBusy(true);
    m_status->setProgress(1, "Queued");
}

void MainWindow::onProgress(const QString &jobId, int percent, const QString &stage)
{
    Q_UNUSED(jobId)
    m_status->setProgress(percent, stage);

    CreatePage *page = pageForId(m_jobPageId);
    if (page)
        page->setProgress(percent, stage);
}

void MainWindow::onJobFinished(const GenerationResult &result)
{
    CreatePage *page = pageForId(m_jobPageId);
    if (page) {
        page->setBusy(false);
        if (!result.cancelled && result.error.isEmpty())
            page->showResult(result);
    }

    m_status->setBusy(false);
    m_activeJobId.clear();
    m_jobPageId.clear();

    if (result.cancelled) {
        showToast("Job cancelled", 0);
    } else if (!result.error.isEmpty()) {
        showToast(result.error, 2);
    } else {
        showToast(QString("%1 image%2 saved").arg(result.images.size()).arg(result.images.size() == 1 ? "" : "s"), 1);
        m_gallery->refresh();
    }
}

void MainWindow::onJobFailed(const QString &jobId, const QString &message)
{
    Q_UNUSED(jobId)

    CreatePage *page = pageForId(m_jobPageId);
    if (page)
        page->setBusy(false);

    m_status->setBusy(false);
    m_activeJobId.clear();
    m_jobPageId.clear();

    showToast(message, 2);
}

void MainWindow::onViewRequested(const QStringList &paths, int index)
{
    if (paths.isEmpty() || !m_viewer)
        return;

    QWidget *host = centralWidget();
    if (host)
        m_viewer->setGeometry(host->rect());
    m_viewer->open(paths, index);
    m_viewer->raise();
}

void MainWindow::onRevealRequested(const QString &path)
{
    openFolder(path);
}

void MainWindow::openFolder(const QString &path)
{
    QString dir;
    if (path.isEmpty()) {
        dir = AppSettings::instance().outputDir();
    } else {
        const QFileInfo fi(path);
        dir = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
    }

    if (dir.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MainWindow::onDiscardRequested(const QString &path)
{
    if (path.isEmpty())
        return;

    m_backend->discardImage(path);

    for (int i = 0; i < m_createPages.size(); ++i)
        m_createPages.at(i)->removePath(path);
    if (m_gallery)
        m_gallery->removePath(path);

    showToast("Image discarded", 0);
}

void MainWindow::onSettingsSaved()
{
    updateBackendState();
    m_sidebar->refreshIcons();
    if (m_gallery)
        m_gallery->refresh();
    showToast("Settings applied", 1);
}

void MainWindow::onOutputCleared()
{
    for (int i = 0; i < m_createPages.size(); ++i)
        m_createPages.at(i)->clearOutput();
    if (m_gallery)
        m_gallery->refresh();
    showToast("Output folder cleared", 1);
}

void MainWindow::showToast(const QString &text, int kind)
{
    if (m_toast)
        m_toast->showMessage(text, kind);
}
