#include "Settings.h"

#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace {

// The app used to be called QwenImageStudio. Its settings live in
// $XDG_CONFIG_HOME/Qwen/QwenImageStudio.ini; carry them over once so an existing
// install keeps its paths and server settings.
void migrateLegacySettings()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/Qwen";
    const QString legacy = dir + "/QwenImageStudio.ini";
    const QString current = dir + "/QwenImageHarness.ini";
    if (!QFile::exists(current) && QFile::exists(legacy))
        QFile::copy(legacy, current);
}

} // namespace

AppSettings::AppSettings(QObject *parent)
    : QObject(parent)
    , m_qs(QSettings::IniFormat, QSettings::UserScope, "Qwen", "QwenImageHarness")
    , m_accent(0)
{
    migrateLegacySettings();
    load();
}

AppSettings &AppSettings::instance()
{
    static AppSettings inst;
    return inst;
}

static QString picturesRoot()
{
    QString p = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (p.isEmpty())
        p = QDir::homePath() + "/Pictures";
    return p;
}

static QString appDataRoot()
{
    QString p = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (p.isEmpty())
        p = QDir::homePath() + "/.local/share/QwenImageHarness";
    return p;
}

QString defaultComfyDir()
{
#ifdef Q_OS_WIN
    return QDir::fromNativeSeparators(QDir::homePath() + "/ComfyUI");
#else
    return QDir::homePath() + "/git/ComfyUI";
#endif
}

QString defaultComfyPython()
{
#ifdef Q_OS_WIN
    return defaultComfyDir() + "/venv/Scripts/python.exe";
#else
    return defaultComfyDir() + "/venv/bin/python";
#endif
}

QStringList AppSettings::accentChoices()
{
    return QStringList() << "#8B7BFF" << "#5AA9FF" << "#42D6B0" << "#FF8A5B" << "#FF6FA5";
}

QColor AppSettings::accentColor(int index)
{
    const QStringList list = accentChoices();
    if (index < 0 || index >= list.size())
        index = 0;
    return QColor(list.at(index));
}

void AppSettings::load()
{
    const QString defaultOutput = QDir::fromNativeSeparators(picturesRoot()) + "/QwenImage";
    const QString defaultLog = QDir::fromNativeSeparators(appDataRoot()) + "/logs";
    const QString defaultMask = QDir::fromNativeSeparators(appDataRoot()) + "/masks";

    m_outputDir = m_qs.value("paths/output", defaultOutput).toString();
    m_logDir = m_qs.value("paths/log", defaultLog).toString();
    m_maskDir = m_qs.value("paths/mask", defaultMask).toString();

    m_data.autoSave = m_qs.value("data/autoSave", true).toBool();
    m_data.sidecar = m_qs.value("data/sidecar", false).toBool();
    m_data.retentionEnabled = m_qs.value("data/retentionEnabled", false).toBool();
    m_data.retentionDays = m_qs.value("data/retentionDays", 30).toInt();

    m_server.command = m_qs.value("server/command", defaultComfyPython()).toString();
    m_server.arguments = m_qs.value("server/arguments",
                                    QString("main.py --listen 127.0.0.1 --port 8188 "
                                            "--reserve-vram 0.8 --fp16-unet")).toString();
    m_server.workingDir = m_qs.value("server/workingDir", defaultComfyDir()).toString();
    m_server.port = m_qs.value("server/port", 8188).toInt();
    m_server.autoStart = m_qs.value("server/autoStart", false).toBool();
    m_server.stopOnExit = m_qs.value("server/stopOnExit", true).toBool();

    m_gen.size = m_qs.value("gen/size", QSize(1024, 1024)).toSize();
    m_gen.steps = m_qs.value("gen/steps", 30).toInt();
    m_gen.cfg = m_qs.value("gen/cfg", 4.0).toDouble();
    m_gen.batch = m_qs.value("gen/batch", 1).toInt();
    m_gen.sampler = m_qs.value("gen/sampler", QString("Euler")).toString();
    m_gen.fixedSeed = m_qs.value("gen/fixedSeed", false).toBool();
    m_gen.seed = m_qs.value("gen/seed", qint64(0)).toLongLong();

    m_lastPage = m_qs.value("ui/lastPage", QString("generate")).toString();
    m_accent = m_qs.value("ui/accent", 0).toInt();
}

void AppSettings::save()
{
    m_qs.setValue("paths/output", m_outputDir);
    m_qs.setValue("paths/log", m_logDir);
    m_qs.setValue("paths/mask", m_maskDir);

    m_qs.setValue("data/autoSave", m_data.autoSave);
    m_qs.setValue("data/sidecar", m_data.sidecar);
    m_qs.setValue("data/retentionEnabled", m_data.retentionEnabled);
    m_qs.setValue("data/retentionDays", m_data.retentionDays);

    m_qs.setValue("server/command", m_server.command);
    m_qs.setValue("server/arguments", m_server.arguments);
    m_qs.setValue("server/workingDir", m_server.workingDir);
    m_qs.setValue("server/port", m_server.port);
    m_qs.setValue("server/autoStart", m_server.autoStart);
    m_qs.setValue("server/stopOnExit", m_server.stopOnExit);

    m_qs.setValue("gen/size", m_gen.size);
    m_qs.setValue("gen/steps", m_gen.steps);
    m_qs.setValue("gen/cfg", m_gen.cfg);
    m_qs.setValue("gen/batch", m_gen.batch);
    m_qs.setValue("gen/sampler", m_gen.sampler);
    m_qs.setValue("gen/fixedSeed", m_gen.fixedSeed);
    m_qs.setValue("gen/seed", m_gen.seed);

    m_qs.setValue("ui/lastPage", m_lastPage);
    m_qs.setValue("ui/accent", m_accent);
    m_qs.sync();

    emit changed();
}

void AppSettings::resetToDefaults()
{
    m_qs.clear();
    load();
    emit changed();
}

QString AppSettings::logFilePath() const
{
    return QDir::fromNativeSeparators(m_logDir) + "/studio.log";
}

bool AppSettings::ensureDirectories(QString *error)
{
    const QStringList dirs = QStringList() << m_outputDir << m_logDir << m_maskDir;
    for (int i = 0; i < dirs.size(); ++i) {
        const QString d = dirs.at(i);
        if (d.isEmpty())
            continue;
        QDir dir(d);
        if (!dir.exists() && !QDir().mkpath(dir.absolutePath())) {
            if (error)
                *error = QString("Cannot create directory: %1").arg(QDir::toNativeSeparators(dir.absolutePath()));
            return false;
        }
    }
    if (error)
        error->clear();
    return true;
}

int AppSettings::applyRetention()
{
    if (!m_data.retentionEnabled || m_data.retentionDays <= 0 || m_outputDir.isEmpty())
        return 0;

    QDir dir(m_outputDir);
    if (!dir.exists())
        return 0;

    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-m_data.retentionDays);
    const QStringList filters = QStringList() << "*.png" << "*.jpg" << "*.jpeg" << "*.webp" << "*.bmp";
    const QFileInfoList files = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);

    int removed = 0;
    for (int i = 0; i < files.size(); ++i) {
        const QFileInfo &fi = files.at(i);
        if (fi.lastModified() >= cutoff)
            continue;
        if (QFile::remove(fi.absoluteFilePath())) {
            ++removed;
            const QString sidecar = fi.absolutePath() + "/" + fi.completeBaseName() + ".json";
            if (QFileInfo(sidecar).exists())
                QFile::remove(sidecar);
        }
    }
    return removed;
}
