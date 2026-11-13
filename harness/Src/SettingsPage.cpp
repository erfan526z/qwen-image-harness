#include "SettingsPage.h"

#include "AppVersion.h"
#include "Icons.h"
#include "Log.h"
#include "Settings.h"
#include "Theme.h"
#include "Widgets.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSysInfo>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QPixmap swatchPixmap(const QColor &color, int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(255, 255, 255, 40), 1.0));
    p.setBrush(color);
    p.drawEllipse(QRectF(1, 1, size - 2, size - 2));
    p.end();
    return pm;
}

QWidget *fieldBox(QWidget *parent, const QString &label, QWidget *content)
{
    QWidget *box = new QWidget(parent);
    QVBoxLayout *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    QLabel *l = new QLabel(label, box);
    l->setObjectName("Hint");
    v->addWidget(l);
    v->addWidget(content);
    return box;
}

} // namespace

SettingsPage::SettingsPage(QWidget *parent)
    : QWidget(parent)
    , m_outputDir(nullptr)
    , m_logDir(nullptr)
    , m_maskDir(nullptr)
    , m_autoSave(nullptr)
    , m_sidecar(nullptr)
    , m_retention(nullptr)
    , m_retentionDays(nullptr)
    , m_command(nullptr)
    , m_arguments(nullptr)
    , m_workdir(nullptr)
    , m_port(nullptr)
    , m_autoStart(nullptr)
    , m_stopOnExit(nullptr)
    , m_width(nullptr)
    , m_height(nullptr)
    , m_steps(nullptr)
    , m_cfg(nullptr)
    , m_batch(nullptr)
    , m_sampler(nullptr)
    , m_accents(nullptr)
    , m_dirtyLabel(nullptr)
    , m_saveButton(nullptr)
    , m_revertButton(nullptr)
    , m_loading(false)
{
    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(22, 16, 22, 16);
    root->setSpacing(14);

    // Header
    QHBoxLayout *header = new QHBoxLayout();
    header->setSpacing(10);

    QVBoxLayout *titles = new QVBoxLayout();
    titles->setSpacing(2);
    QLabel *title = new QLabel("Settings", this);
    title->setObjectName("PageTitle");
    titles->addWidget(title);
    QLabel *subtitle = new QLabel("Directories, data controls and how the backend is launched.", this);
    subtitle->setObjectName("PageSub");
    titles->addWidget(subtitle);
    header->addLayout(titles);
    header->addStretch(1);

    m_dirtyLabel = new QLabel(QString(), this);
    m_dirtyLabel->setObjectName("Hint");
    header->addWidget(m_dirtyLabel);

    m_revertButton = new QPushButton("Revert", this);
    m_revertButton->setObjectName("Ghost");
    m_revertButton->setCursor(Qt::PointingHandCursor);
    connect(m_revertButton, &QPushButton::clicked, this, &SettingsPage::revert);
    header->addWidget(m_revertButton);

    m_saveButton = new QPushButton("Save changes", this);
    m_saveButton->setObjectName("Primary");
    m_saveButton->setCursor(Qt::PointingHandCursor);
    connect(m_saveButton, &QPushButton::clicked, this, &SettingsPage::save);
    header->addWidget(m_saveButton);

    root->addLayout(header);

    // Scroll body
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    QWidget *host = new QWidget(scroll);
    QVBoxLayout *hostLayout = new QVBoxLayout(host);
    hostLayout->setContentsMargins(0, 0, 6, 0);
    hostLayout->setSpacing(0);

    QWidget *content = new QWidget(host);
    content->setMaximumWidth(980);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    QVBoxLayout *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 8);
    contentLayout->setSpacing(12);

    contentLayout->addWidget(buildDirectoriesCard());
    contentLayout->addWidget(buildDataCard());
    contentLayout->addWidget(buildServerCard());
    contentLayout->addWidget(buildDefaultsCard());
    contentLayout->addWidget(buildAppearanceCard());
    contentLayout->addWidget(buildAboutCard());
    contentLayout->addStretch(1);

    QHBoxLayout *centering = new QHBoxLayout();
    centering->setContentsMargins(0, 0, 0, 0);
    centering->addStretch(1);
    centering->addWidget(content, 20);
    centering->addStretch(1);
    hostLayout->addLayout(centering);

    scroll->setWidget(host);

    root->addWidget(scroll, 1);

    loadFromSettings();
}

QWidget *SettingsPage::dirRow(const QString &label, QLineEdit *edit, const QString &hint)
{
    QWidget *row = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    QLabel *caption = new QLabel(label, row);
    caption->setObjectName("Dim");
    caption->setFixedWidth(140);
    layout->addWidget(caption);

    edit->setFixedHeight(34);
    edit->setToolTip(hint);
    layout->addWidget(edit, 1);

    QPushButton *browse = new QPushButton("Browse", row);
    browse->setCursor(Qt::PointingHandCursor);
    connect(browse, &QPushButton::clicked, this, [this, edit]() {
        const QString dir = QFileDialog::getExistingDirectory(this, "Choose folder", edit->text());
        if (dir.isEmpty())
            return;
        edit->setText(QDir::toNativeSeparators(dir));
    });
    layout->addWidget(browse);

    QToolButton *open = Widgets::iconButton(Icons::Folder, "Open folder", row, 16);
    connect(open, &QToolButton::clicked, this, [edit]() {
        if (!edit->text().isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(edit->text()));
    });
    layout->addWidget(open);

    connect(edit, &QLineEdit::textChanged, this, &SettingsPage::markDirty);
    return row;
}

QWidget *SettingsPage::switchRow(const QString &title, const QString &desc, ToggleSwitch *toggle,
                                 QWidget *extra)
{
    QWidget *row = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(12);

    QVBoxLayout *texts = new QVBoxLayout();
    texts->setContentsMargins(0, 0, 0, 0);
    texts->setSpacing(2);

    QLabel *t = new QLabel(title, row);
    t->setFont(Theme::ui(12, QFont::DemiBold));
    texts->addWidget(t);

    QLabel *d = new QLabel(desc, row);
    d->setObjectName("Hint");
    d->setWordWrap(true);
    texts->addWidget(d);

    layout->addLayout(texts, 1);
    if (extra)
        layout->addWidget(extra, 0, Qt::AlignVCenter);
    layout->addWidget(toggle, 0, Qt::AlignVCenter);

    connect(toggle, &ToggleSwitch::toggled, this, &SettingsPage::markDirty);
    return row;
}

QWidget *SettingsPage::buildDirectoriesCard()
{
    Card *card = new Card("Directories", this);

    m_outputDir = new QLineEdit(card);
    m_logDir = new QLineEdit(card);
    m_maskDir = new QLineEdit(card);

    card->body()->addWidget(dirRow("Output images", m_outputDir,
                                   "Generated images are written here and the gallery watches it."));
    card->body()->addWidget(dirRow("Logs", m_logDir, "studio.log is appended here."));
    card->body()->addWidget(dirRow("Masks", m_maskDir, "Working masks are written here before a job."));

    return card;
}

QWidget *SettingsPage::buildDataCard()
{
    Card *card = new Card("Data controls", this);

    m_autoSave = new ToggleSwitch(card);
    card->body()->addWidget(switchRow("Save generated images", 
                                      "Write every result to the output directory as soon as it is ready.",
                                      m_autoSave));

    m_sidecar = new ToggleSwitch(card);
    card->body()->addWidget(switchRow("Write JSON sidecars",
                                      "Store the prompt, seed and parameters next to each image.",
                                      m_sidecar));

    m_retention = new ToggleSwitch(card);
    m_retentionDays = new QSpinBox(card);
    m_retentionDays->setRange(1, 3650);
    m_retentionDays->setFixedWidth(70);
    m_retentionDays->setSuffix(" d");

    QWidget *retentionExtra = new QWidget(card);
    QHBoxLayout *retentionLayout = new QHBoxLayout(retentionExtra);
    retentionLayout->setContentsMargins(0, 0, 0, 0);
    retentionLayout->setSpacing(6);
    QLabel *older = new QLabel("older than", retentionExtra);
    older->setObjectName("Hint");
    retentionLayout->addWidget(older);
    retentionLayout->addWidget(m_retentionDays);

    card->body()->addWidget(switchRow("Auto-clean old images",
                                      "Delete generated images older than the window below.",
                                      m_retention, retentionExtra));

    connect(m_retention, &ToggleSwitch::toggled, this, [this](bool on) {
        m_retentionDays->setEnabled(on);
        markDirty();
    });
    connect(m_retentionDays, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &SettingsPage::markDirty);

    card->body()->addWidget(Widgets::separator(card));

    QHBoxLayout *actions = new QHBoxLayout();
    actions->setSpacing(8);

    QPushButton *clearOutput = new QPushButton("Clear output folder", card);
    clearOutput->setObjectName("Danger");
    clearOutput->setCursor(Qt::PointingHandCursor);
    connect(clearOutput, &QPushButton::clicked, this, &SettingsPage::clearOutput);
    actions->addWidget(clearOutput);

    QPushButton *clearLog = new QPushButton("Clear log", card);
    clearLog->setCursor(Qt::PointingHandCursor);
    connect(clearLog, &QPushButton::clicked, this, [this]() {
        Log::instance().clear();
        Logging::info("Log cleared");
    });
    actions->addWidget(clearLog);

    actions->addStretch(1);

    QPushButton *reset = new QPushButton("Reset all settings", card);
    reset->setObjectName("Ghost");
    reset->setCursor(Qt::PointingHandCursor);
    connect(reset, &QPushButton::clicked, this, &SettingsPage::resetAll);
    actions->addWidget(reset);

    card->body()->addLayout(actions);
    return card;
}

QWidget *SettingsPage::buildServerCard()
{
    Card *card = new Card("Backend server", this);

    QVBoxLayout *body = card->body();

    QLabel *commandLabel = new QLabel("Command", card);
    commandLabel->setObjectName("Hint");
    body->addWidget(commandLabel);
    m_command = new QLineEdit(card);
    m_command->setPlaceholderText("python3");
    body->addWidget(m_command);
    connect(m_command, &QLineEdit::textChanged, this, &SettingsPage::markDirty);

    QLabel *argsLabel = new QLabel("Arguments", card);
    argsLabel->setObjectName("Hint");
    body->addWidget(argsLabel);
    m_arguments = new QLineEdit(card);
    m_arguments->setPlaceholderText("run_server.py --port 8000");
    body->addWidget(m_arguments);
    connect(m_arguments, &QLineEdit::textChanged, this, &SettingsPage::markDirty);

    QWidget *workdirRow = dirRow("Working directory", m_workdir = new QLineEdit(card),
                                 "Where the server process is started.");
    body->addWidget(workdirRow);

    QHBoxLayout *portRow = new QHBoxLayout();
    portRow->setSpacing(8);
    QLabel *portLabel = new QLabel("Port", card);
    portLabel->setObjectName("Dim");
    portLabel->setFixedWidth(140);
    portRow->addWidget(portLabel);
    m_port = new QSpinBox(card);
    m_port->setRange(1, 65535);
    m_port->setFixedWidth(110);
    portRow->addWidget(m_port);
    portRow->addStretch(1);
    body->addLayout(portRow);
    connect(m_port, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &SettingsPage::markDirty);

    body->addWidget(Widgets::separator(card));

    m_autoStart = new ToggleSwitch(card);
    body->addWidget(switchRow("Start on launch",
                              "Start the backend as soon as the window opens.",
                              m_autoStart));

    m_stopOnExit = new ToggleSwitch(card);
    body->addWidget(switchRow("Stop on exit",
                              "Shut the backend down when the window closes.",
                              m_stopOnExit));

    QLabel *note = new QLabel("These fields are only a pad for your adapter: the bundled build runs a "
                              "simulated backend and never spawns a process.",
                              card);
    note->setObjectName("Hint");
    note->setWordWrap(true);
    body->addWidget(note);

    return card;
}

QWidget *SettingsPage::buildDefaultsCard()
{
    Card *card = new Card("Generation defaults", this);

    QGridLayout *grid = new QGridLayout();
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(2, 1);

    m_width = new QSpinBox(card);
    m_width->setRange(256, 2048);
    m_width->setSingleStep(64);
    m_width->setFixedWidth(110);
    m_height = new QSpinBox(card);
    m_height->setRange(256, 2048);
    m_height->setSingleStep(64);
    m_height->setFixedWidth(110);

    QWidget *sizeRow = new QWidget(card);
    QHBoxLayout *sizeLayout = new QHBoxLayout(sizeRow);
    sizeLayout->setContentsMargins(0, 0, 0, 0);
    sizeLayout->setSpacing(8);
    sizeLayout->addWidget(m_width);
    QLabel *times = new QLabel(QString::fromUtf8("\xC3\x97"), sizeRow);
    times->setStyleSheet(QString("color: %1;").arg(Theme::TextFaint.name()));
    sizeLayout->addWidget(times);
    sizeLayout->addWidget(m_height);
    sizeLayout->addStretch(1);

    grid->addWidget(fieldBox(card, "Size", sizeRow), 0, 0, 1, 2);

    m_steps = new QSpinBox(card);
    m_steps->setRange(4, 80);
    grid->addWidget(fieldBox(card, "Steps", m_steps), 0, 2);

    m_cfg = new QDoubleSpinBox(card);
    m_cfg->setRange(0.0, 25.0);
    m_cfg->setSingleStep(0.5);
    m_cfg->setDecimals(1);
    grid->addWidget(fieldBox(card, "Guidance (CFG)", m_cfg), 1, 0);

    m_batch = new QSpinBox(card);
    m_batch->setRange(1, 8);
    grid->addWidget(fieldBox(card, "Batch", m_batch), 1, 1);

    m_sampler = new ThemeCombo(card);
    m_sampler->addItems(QStringList() << "Euler" << "Euler a" << "DPM++ 2M"
                                      << "DPM++ SDE" << "UniPC" << "DDIM");
    grid->addWidget(fieldBox(card, "Sampler", m_sampler), 1, 2);

    card->body()->addLayout(grid);

    connect(m_width, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &SettingsPage::markDirty);
    connect(m_height, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &SettingsPage::markDirty);
    connect(m_steps, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &SettingsPage::markDirty);
    connect(m_batch, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &SettingsPage::markDirty);
    connect(m_cfg, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
            this, &SettingsPage::markDirty);
    connect(m_sampler, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, &SettingsPage::markDirty);

    return card;
}

QWidget *SettingsPage::buildAppearanceCard()
{
    Card *card = new Card("Appearance", this);

    QLabel *label = new QLabel("Accent colour", card);
    label->setObjectName("Hint");
    card->body()->addWidget(label);

    QWidget *row = new QWidget(card);
    QHBoxLayout *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    m_accents = new QButtonGroup(this);
    m_accents->setExclusive(true);

    const QStringList choices = AppSettings::accentChoices();
    for (int i = 0; i < choices.size(); ++i) {
        const QColor color(choices.at(i));
        QToolButton *b = new QToolButton(row);
        b->setObjectName("Swatch");
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setFixedSize(32, 32);
        b->setIcon(QIcon(swatchPixmap(color, 20)));
        b->setIconSize(QSize(20, 20));
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(choices.at(i));
        m_accents->addButton(b, i);
        layout->addWidget(b);
        connect(b, &QToolButton::clicked, this, &SettingsPage::markDirty);
    }

    layout->addStretch(1);
    card->body()->addWidget(row);
    return card;
}

QWidget *SettingsPage::buildAboutCard()
{
    Card *card = new Card("About", this);

    QLabel *text = new QLabel(
        QString("<div style='line-height:150%'>"
                "<b>Qwen Image Harness</b> %1<br>"
                "Frontend shell for Qwen-Image-2.1.<br><br>"
                "Qt %2  &middot;  %3<br>"
                "The UI talks to an abstract backend; the bundled build uses a simulator."
                "</div>")
            .arg(QString::fromLatin1(APP_VERSION), QString::fromLatin1(qVersion()),
                 QSysInfo::prettyProductName()),
        card);
    text->setObjectName("Dim");
    text->setWordWrap(true);
    text->setTextFormat(Qt::RichText);
    card->body()->addWidget(text);

    return card;
}

void SettingsPage::loadFromSettings()
{
    m_loading = true;
    const AppSettings &s = AppSettings::instance();

    m_outputDir->setText(QDir::toNativeSeparators(s.outputDir()));
    m_outputDir->setCursorPosition(0);
    m_logDir->setText(QDir::toNativeSeparators(s.logDir()));
    m_logDir->setCursorPosition(0);
    m_maskDir->setText(QDir::toNativeSeparators(s.maskDir()));
    m_maskDir->setCursorPosition(0);

    m_autoSave->setChecked(s.data().autoSave);
    m_sidecar->setChecked(s.data().sidecar);
    m_retention->setChecked(s.data().retentionEnabled);
    m_retentionDays->setValue(s.data().retentionDays);
    m_retentionDays->setEnabled(s.data().retentionEnabled);

    m_command->setText(s.server().command);
    m_arguments->setText(s.server().arguments);
    m_workdir->setText(QDir::toNativeSeparators(s.server().workingDir));
    m_port->setValue(s.server().port);
    m_autoStart->setChecked(s.server().autoStart);
    m_stopOnExit->setChecked(s.server().stopOnExit);

    m_width->setValue(s.defaults().size.width());
    m_height->setValue(s.defaults().size.height());
    m_steps->setValue(s.defaults().steps);
    m_cfg->setValue(s.defaults().cfg);
    m_batch->setValue(s.defaults().batch);
    const int samplerIndex = m_sampler->findText(s.defaults().sampler);
    if (samplerIndex >= 0)
        m_sampler->setCurrentIndex(samplerIndex);

    if (m_accents) {
        QAbstractButton *b = m_accents->button(s.accent());
        if (b)
            b->setChecked(true);
    }

    m_loading = false;
    m_dirtyLabel->clear();
    if (m_saveButton)
        m_saveButton->setEnabled(false);
    if (m_revertButton)
        m_revertButton->setEnabled(false);
}

void SettingsPage::applyToSettings()
{
    AppSettings &s = AppSettings::instance();

    s.setOutputDir(QDir::fromNativeSeparators(m_outputDir->text().trimmed()));
    s.setLogDir(QDir::fromNativeSeparators(m_logDir->text().trimmed()));
    s.setMaskDir(QDir::fromNativeSeparators(m_maskDir->text().trimmed()));

    DataOptions data;
    data.autoSave = m_autoSave->isChecked();
    data.sidecar = m_sidecar->isChecked();
    data.retentionEnabled = m_retention->isChecked();
    data.retentionDays = m_retentionDays->value();
    s.setData(data);

    ServerOptions server;
    server.command = m_command->text().trimmed();
    server.arguments = m_arguments->text().trimmed();
    server.workingDir = QDir::fromNativeSeparators(m_workdir->text().trimmed());
    server.port = m_port->value();
    server.autoStart = m_autoStart->isChecked();
    server.stopOnExit = m_stopOnExit->isChecked();
    s.setServer(server);

    GenerationDefaults gen;
    gen.size = QSize(m_width->value(), m_height->value());
    gen.steps = m_steps->value();
    gen.cfg = m_cfg->value();
    gen.batch = m_batch->value();
    gen.sampler = m_sampler->currentText();
    gen.fixedSeed = s.defaults().fixedSeed;
    gen.seed = s.defaults().seed;
    s.setDefaults(gen);

    if (m_accents && m_accents->checkedId() >= 0)
        s.setAccent(m_accents->checkedId());
}

void SettingsPage::save()
{
    applyToSettings();
    AppSettings &s = AppSettings::instance();

    QString error;
    if (!s.ensureDirectories(&error)) {
        QMessageBox::warning(this, "Settings", error);
        return;
    }

    s.save();
    Log::instance().setLogFile(s.logFilePath());
    Theme::setAccent(AppSettings::accentColor(s.accent()));

    const int removed = s.applyRetention();
    if (removed > 0)
        Logging::info(QString("Retention removed %1 old image(s)").arg(removed));

    m_dirtyLabel->clear();
    Logging::success("Settings saved");
    emit saved();
}

void SettingsPage::revert()
{
    loadFromSettings();
    Logging::info("Settings changes reverted");
}

void SettingsPage::markDirty()
{
    if (m_loading)
        return;
    m_dirtyLabel->setText("Unsaved changes");
    if (m_saveButton)
        m_saveButton->setEnabled(true);
    if (m_revertButton)
        m_revertButton->setEnabled(true);
}

void SettingsPage::reload()
{
    loadFromSettings();
}

void SettingsPage::clearOutput()
{
    const QString dir = AppSettings::instance().outputDir();
    QDir d(dir);
    const QStringList filters = QStringList() << "*.png" << "*.jpg" << "*.jpeg"
                                              << "*.webp" << "*.bmp";
    const QFileInfoList files = d.entryInfoList(filters, QDir::Files);

    if (files.isEmpty()) {
        Logging::info("Output folder is already empty.");
        return;
    }

    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, "Clear output folder",
        QString("Delete %1 generated file(s) from\n%2?\n\nThis cannot be undone.")
            .arg(files.size())
            .arg(QDir::toNativeSeparators(dir)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (answer != QMessageBox::Yes)
        return;

    int removed = 0;
    for (int i = 0; i < files.size(); ++i) {
        const QFileInfo &fi = files.at(i);
        if (!QFile::remove(fi.absoluteFilePath()))
            continue;
        ++removed;
        const QString sidecar = fi.absolutePath() + "/" + fi.completeBaseName() + ".json";
        if (QFileInfo(sidecar).exists())
            QFile::remove(sidecar);
    }

    Logging::success(QString("Removed %1 file(s) from the output folder").arg(removed));
    emit outputCleared();
}

void SettingsPage::resetAll()
{
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, "Reset settings",
        "Restore every setting to its default value?",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (answer != QMessageBox::Yes)
        return;

    AppSettings::instance().resetToDefaults();
    Theme::setAccent(AppSettings::accentColor(AppSettings::instance().accent()));
    loadFromSettings();
    Logging::success("Settings reset to defaults");
    emit saved();
}
