#ifndef SETTINGS_H
#define SETTINGS_H

#include <QColor>
#include <QObject>
#include <QSettings>
#include <QSize>
#include <QString>

struct GenerationDefaults {
    QSize size;
    int steps;
    double cfg;
    int batch;
    QString sampler;
    bool fixedSeed;
    qint64 seed;

    GenerationDefaults()
        : size(1024, 1024)
        , steps(30)
        , cfg(4.0)
        , batch(1)
        , sampler("Euler")
        , fixedSeed(false)
        , seed(0)
    {
    }
};

struct DataOptions {
    bool autoSave;
    bool sidecar;
    bool retentionEnabled;
    int retentionDays;

    DataOptions()
        : autoSave(true)
        , sidecar(false)
        , retentionEnabled(false)
        , retentionDays(30)
    {
    }
};

struct ServerOptions {
    QString command;
    QString arguments;
    QString workingDir;
    int port;
    bool autoStart;
    bool stopOnExit;

    ServerOptions()
        : port(8000)
        , autoStart(false)
        , stopOnExit(true)
    {
    }
};

//! Platform default ComfyUI checkout: ~/git/ComfyUI on Unix, %USERPROFILE%/ComfyUI on Windows.
QString defaultComfyDir();

//! Interpreter inside that checkout's venv (bin/python on Unix, Scripts/python.exe on Windows).
QString defaultComfyPython();

/*!
 * Persistent application configuration. Values are cached in memory, edited
 * through setters, and written by save() so a settings page can offer
 * Apply / Revert semantics.
 */
class AppSettings : public QObject
{
    Q_OBJECT

public:
    static AppSettings &instance();

    void load();
    void save();
    void resetToDefaults();

    QString outputDir() const { return m_outputDir; }
    QString logDir() const { return m_logDir; }
    QString maskDir() const { return m_maskDir; }
    QString logFilePath() const;

    const DataOptions &data() const { return m_data; }
    const ServerOptions &server() const { return m_server; }
    const GenerationDefaults &defaults() const { return m_gen; }
    QString lastPage() const { return m_lastPage; }
    int accent() const { return m_accent; }

    void setOutputDir(const QString &v) { m_outputDir = v; }
    void setLogDir(const QString &v) { m_logDir = v; }
    void setMaskDir(const QString &v) { m_maskDir = v; }
    void setData(const DataOptions &v) { m_data = v; }
    void setServer(const ServerOptions &v) { m_server = v; }
    void setDefaults(const GenerationDefaults &v) { m_gen = v; }
    void setLastPage(const QString &v) { m_lastPage = v; }
    void setAccent(int v) { m_accent = v; }

    //! Creates the configured directories when they do not exist yet.
    bool ensureDirectories(QString *error = nullptr);

    //! Deletes images older than the retention window. Returns removed count.
    int applyRetention();

    static QStringList accentChoices();
    static QColor accentColor(int index);

signals:
    void changed();

private:
    explicit AppSettings(QObject *parent = nullptr);

    QSettings m_qs;
    QString m_outputDir;
    QString m_logDir;
    QString m_maskDir;
    DataOptions m_data;
    ServerOptions m_server;
    GenerationDefaults m_gen;
    QString m_lastPage;
    int m_accent;
};

#endif // SETTINGS_H
