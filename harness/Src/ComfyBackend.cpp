#include "ComfyBackend.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>
#include <QUrlQuery>

#include "Log.h"
#include "Settings.h"

namespace {

QJsonObject obj(std::initializer_list<QPair<QString, QJsonValue>> items)
{
    QJsonObject o;
    for (const auto &kv : items)
        o.insert(kv.first, kv.second);
    return o;
}

QJsonArray link(const QString &nodeId, int slot)
{
    QJsonArray a;
    a.append(nodeId);
    a.append(slot);
    return a;
}

// The turbo checkpoint is distilled to 8 steps with a baked schedule.
const char *kTurboSigmas = "1.0, 0.978453, 0.95418, 0.926626, 0.89508, "
                           "0.845148, 0.704534, 0.414568, 0";

const char *kTurboModel = "qwen-image-2.1-turbo-Q4_K_M.gguf";
const char *kBaseModel = "qwen_image_2.1-Q4_K_M.gguf";
const char *kStockTe = "qwen3vl_8b_w4a8.safetensors";
const char *kHereticTe = "qwen3vl_8b_w4a8_heretic.safetensors";
const char *kVae = "qwen_image_2.1_vae_bf16.safetensors";


} // namespace
namespace {

// QProcess::splitCommand arrived in Qt 5.15. Keep the same behaviour on the
// Qt 5.12/5.14 kits the Windows builds use: split on whitespace outside quotes,
// honour single/double quotes and backslash escapes inside double quotes.
QStringList splitCommandLine(const QString &command)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    return QProcess::splitCommand(command);
#else
    QStringList out;
    QString current;
    bool inSingle = false;
    bool inDouble = false;
    bool hasToken = false;

    for (int i = 0; i < command.size(); ++i) {
        const QChar c = command.at(i);
        if (c == '\\' && inDouble && i + 1 < command.size()) {
            current += command.at(++i);
            hasToken = true;
        } else if (c == '"' && !inSingle) {
            inDouble = !inDouble;
            hasToken = true;
        } else if (c == '\'' && !inDouble) {
            inSingle = !inSingle;
            hasToken = true;
        } else if (c.isSpace() && !inSingle && !inDouble) {
            if (hasToken) {
                out << current;
                current.clear();
                hasToken = false;
            }
        } else {
            current += c;
            hasToken = true;
        }
    }
    if (hasToken)
        out << current;
    return out;
#endif
}

// Bounding box of everything above `threshold`, or a null rect for an empty mask.
QRect maskBounds(const QImage &gray, int threshold = 127)
{
    int minX = gray.width(), minY = gray.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < gray.height(); ++y) {
        const uchar *line = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x) {
            if (line[x] > threshold) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    if (maxX < 0)
        return QRect();
    return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

// Separable box blur, two passes: cheap gaussian-ish feather for the paste edge.
QImage blurMask(const QImage &gray, int radius)
{
    if (radius <= 0)
        return gray;
    QImage img = gray.convertToFormat(QImage::Format_Grayscale8);
    const int taps = 2 * radius + 1;
    for (int pass = 0; pass < 2; ++pass) {
        QImage tmp(img.size(), QImage::Format_Grayscale8);
        for (int y = 0; y < img.height(); ++y) {
            const uchar *src = img.constScanLine(y);
            uchar *dst = tmp.scanLine(y);
            long sum = 0;
            const int n = img.width();
            for (int x = -radius; x <= radius; ++x)
                sum += src[qBound(0, x, n - 1)];
            for (int x = 0; x < n; ++x) {
                dst[x] = uchar(sum / taps);
                sum -= src[qBound(0, x - radius, n - 1)];
                sum += src[qBound(0, x + radius + 1, n - 1)];
            }
        }
        QImage out(tmp.size(), QImage::Format_Grayscale8);
        const int h = tmp.height();
        for (int x = 0; x < tmp.width(); ++x) {
            long sum = 0;
            for (int y = -radius; y <= radius; ++y)
                sum += tmp.constScanLine(qBound(0, y, h - 1))[x];
            for (int y = 0; y < h; ++y) {
                out.scanLine(y)[x] = uchar(sum / taps);
                sum -= tmp.constScanLine(qBound(0, y - radius, h - 1))[x];
                sum += tmp.constScanLine(qBound(0, y + radius + 1, h - 1))[x];
            }
        }
        img = out;
    }
    return img;
}

} // namespace

ComfyBackend::ComfyBackend(QObject *parent)
    : Backend(parent)
    , m_net(new QNetworkAccessManager(this))
    , m_proc(nullptr)
    , m_health(new QTimer(this))
    , m_poll(new QTimer(this))
    , m_port(8188)
    , m_healthTicks(0)
    , m_owned(false)
    , m_step(0)
    , m_total(0)
{
    m_health->setInterval(500);
    connect(m_health, &QTimer::timeout, this, &ComfyBackend::pollServer);

    m_poll->setInterval(700);
    connect(m_poll, &QTimer::timeout, this, &ComfyBackend::pollJobs);

    loadServerOptions();
}

ComfyBackend::~ComfyBackend()
{
    if (m_proc) {
        m_proc->disconnect(this);
        if (m_proc->state() != QProcess::NotRunning) {
            m_proc->terminate();
            if (!m_proc->waitForFinished(8000))
                m_proc->kill();
        }
    }
}

void ComfyBackend::loadServerOptions()
{
    m_port = AppSettings::instance().server().port > 0 ? AppSettings::instance().server().port : 8188;
}

QString ComfyBackend::baseUrl() const
{
    return QString("http://127.0.0.1:%1").arg(m_port);
}

void ComfyBackend::log(const QString &text)
{
    Logging::backend(text);
}

void ComfyBackend::emitState(State state)
{
    setState(state);
}

// ---------------------------------------------------------------- server ----

void ComfyBackend::startServer()
{
    if (state() == Running) {
        log("Server already running.");
        return;
    }

    loadServerOptions();
    emitState(Starting);

    QJsonObject probe;
    QString error;
    if (httpGet("/system_stats", &probe, &error, 1500)) {
        m_owned = false;
        m_health->stop();
        emitState(Running);
        log(QString("Attached to a ComfyUI already listening on port %1 (not started by this app).").arg(m_port));
        return;
    }

    const ServerOptions &opts = AppSettings::instance().server();
    QString program = opts.command.trimmed();
    QStringList args;

    if (program.isEmpty()) {
        program = defaultComfyPython();
        args = splitCommandLine(QString("main.py --listen 127.0.0.1 --port %1 "
                                        "--reserve-vram 0.8 --fp16-unet").arg(m_port));
    } else {
        args = splitCommandLine(opts.arguments);
    }

    QString workDir = opts.workingDir.trimmed();
    if (workDir.isEmpty())
        workDir = defaultComfyDir();

    if (!QFileInfo::exists(program)) {
        emitState(Failed);
        const QString msg = QString("Server command not found: %1").arg(program);
        log(msg);
        emit jobFailed(QString(), msg);
        return;
    }

    m_proc = new QProcess(this);
    m_proc->setWorkingDirectory(workDir);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    m_proc->setProgram(program);
    m_proc->setArguments(args);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("PYTORCH_CUDA_ALLOC_CONF", "expandable_segments:True");
    m_proc->setProcessEnvironment(env);

    connect(m_proc, &QProcess::readyRead, this, &ComfyBackend::onProcessOutput);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &ComfyBackend::onProcessFinished);

    log(QString("Starting: %1 %2").arg(program, args.join(' ')));
    m_proc->start();

    if (!m_proc->waitForStarted(5000)) {
        emitState(Failed);
        const QString msg = QString("Could not start the server: %1").arg(m_proc->errorString());
        log(msg);
        emit jobFailed(QString(), msg);
        return;
    }

    m_owned = true;
    m_healthTicks = 0;
    m_health->start();
    log("Waiting for ComfyUI to answer on /system_stats ...");
}

void ComfyBackend::stopServer()
{
    m_health->stop();
    emitState(Stopping);

    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        log("Stopping ComfyUI ...");
        m_proc->terminate();
        if (!m_proc->waitForFinished(15000)) {
            log("ComfyUI did not exit in time, killing it.");
            m_proc->kill();
            m_proc->waitForFinished(5000);
        }
    } else {
        log("No ComfyUI process owned by this app; any external instance is left alone.");
    }

    emitState(Stopped);
}

void ComfyBackend::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status)
    onProcessOutput();
    if (m_proc) {
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    if (state() != Stopping && state() != Stopped) {
        const QString msg = QString("ComfyUI exited (code %1)").arg(exitCode);
        log(msg);
        emitState(Failed);
        for (Job *job : m_jobs)
            failJob(job, msg);
    }
    m_owned = false;
}

void ComfyBackend::pollServer()
{
    QJsonObject stats;
    QString error;
    if (httpGet("/system_stats", &stats, &error, 1200)) {
        m_health->stop();
        emitState(Running);
        log("ComfyUI is up.");
        return;
    }
    if (++m_healthTicks > 480) { // ~4 minutes
        m_health->stop();
        emitState(Failed);
        const QString msg = "ComfyUI did not become ready within 4 minutes - see the Terminal page.";
        log(msg);
        emit jobFailed(QString(), msg);
    }
}

void ComfyBackend::onProcessOutput()
{
    if (!m_proc)
        return;

    const QString text = QString::fromLocal8Bit(m_proc->readAll());
    static const QRegularExpression ansi("\\x1b\\[[0-9;]*[A-Za-z]");
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    const QStringList lines = text.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts);
#else
    const QStringList lines = text.split(QRegularExpression("[\r\n]+"), QString::SkipEmptyParts);
#endif

    static const QRegularExpression stepRe("(\\d+)/(\\d+)\\s*\\[");
    for (const QString &raw : lines) {
        const QString line = QString(raw).remove(ansi).trimmed();
        if (line.isEmpty())
            continue;
        log(line);

        QRegularExpressionMatch m = stepRe.match(line);
        if (m.hasMatch()) {
            const int done = m.captured(1).toInt();
            const int total = m.captured(2).toInt();
            if (total > 0) {
                m_step = done;
                m_total = total;
                if (!m_activeJob.isEmpty() && m_jobs.contains(m_activeJob)) {
                    Job *job = m_jobs.value(m_activeJob);
                    const int pct = 5 + (90 * done) / total;
                    emit progress(job->id, pct,
                                  QString("Sampling %1/%2").arg(done).arg(total));
                }
            }
        }
    }
}

// ---------------------------------------------------------------- http ------

bool ComfyBackend::httpGet(const QString &path, QJsonObject *json, QString *error, int timeoutMs)
{
    QNetworkRequest req{QUrl(baseUrl() + path)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_net->get(req);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    bool ok = false;
    if (reply->isFinished() && reply->error() == QNetworkReply::NoError) {
        const QByteArray body = reply->readAll();
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error == QJsonParseError::NoError && doc.isObject()) {
            if (json)
                *json = doc.object();
            ok = true;
        } else if (error) {
            *error = QString("Malformed JSON from %1").arg(path);
        }
    } else if (error) {
        *error = reply->isFinished() ? reply->errorString()
                                     : QString("Timeout talking to %1").arg(path);
    }

    reply->deleteLater();
    return ok;
}

bool ComfyBackend::httpPostJson(const QString &path, const QJsonObject &body, QJsonObject *json,
                                QString *error, int timeoutMs)
{
    QNetworkRequest req{QUrl(baseUrl() + path)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    bool ok = false;
    const QByteArray raw = reply->readAll();
    if (reply->isFinished() && reply->error() == QNetworkReply::NoError) {
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
        if (pe.error == QJsonParseError::NoError && doc.isObject()) {
            if (json)
                *json = doc.object();
            ok = true;
        } else {
            ok = true; // 200 with an empty body is fine for some endpoints
        }
    } else if (error) {
        QString detail = reply->isFinished() ? reply->errorString()
                                             : QString("Timeout talking to %1").arg(path);
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (doc.isObject() && doc.object().contains("error")) {
            const QJsonObject e = doc.object().value("error").toObject();
            detail = e.value("message").toString(detail);
        }
        *error = detail;
    }

    reply->deleteLater();
    return ok;
}

bool ComfyBackend::httpPostEmpty(const QString &path, QString *error)
{
    QNetworkRequest req{QUrl(baseUrl() + path)};
    QNetworkReply *reply = m_net->post(req, QByteArray());

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(8000);
    loop.exec();

    const bool ok = reply->isFinished() && reply->error() == QNetworkReply::NoError;
    if (!ok && error)
        *error = reply->errorString();
    reply->deleteLater();
    return ok;
}

bool ComfyBackend::httpUpload(const QString &filePath, const QString &field,
                              QString *storedName, QString *error)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QString("Cannot read %1").arg(QDir::toNativeSeparators(filePath));
        return false;
    }
    const QByteArray data = file.readAll();
    file.close();

    QHttpMultiPart *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QString("form-data; name=\"%1\"; filename=\"%2\"")
                           .arg(field, QFileInfo(filePath).fileName()));
    filePart.setBody(data);
    multi->append(filePart);

    QHttpPart overwrite;
    overwrite.setHeader(QNetworkRequest::ContentDispositionHeader,
                        "form-data; name=\"overwrite\"");
    overwrite.setBody("true");
    multi->append(overwrite);

    QHttpPart type;
    type.setHeader(QNetworkRequest::ContentDispositionHeader, "form-data; name=\"type\"");
    type.setBody("input");
    multi->append(type);

    QNetworkRequest req{QUrl(baseUrl() + "/upload/image")};
    QNetworkReply *reply = m_net->post(req, multi);
    multi->setParent(reply);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(120000);
    loop.exec();

    bool ok = false;
    if (reply->isFinished() && reply->error() == QNetworkReply::NoError) {
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject o = doc.object();
        const QString name = o.value("name").toString();
        const QString sub = o.value("subfolder").toString();
        if (!name.isEmpty()) {
            *storedName = sub.isEmpty() ? name : (sub + "/" + name);
            ok = true;
        }
    }
    if (!ok && error)
        *error = reply->isFinished() ? reply->errorString() : QString("Upload timed out");

    reply->deleteLater();
    return ok;
}

bool ComfyBackend::httpDownload(const QString &query, const QString &destPath, QString *error)
{
    QNetworkRequest req{QUrl(baseUrl() + "/view?" + query)};
    QNetworkReply *reply = m_net->get(req);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(120000);
    loop.exec();

    bool ok = false;
    if (reply->isFinished() && reply->error() == QNetworkReply::NoError) {
        QDir().mkpath(QFileInfo(destPath).absolutePath());
        QFile out(destPath);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(reply->readAll());
            out.close();
            ok = true;
        } else if (error) {
            *error = QString("Cannot write %1").arg(QDir::toNativeSeparators(destPath));
        }
    } else if (error) {
        *error = reply->isFinished() ? reply->errorString() : QString("Download timed out");
    }

    reply->deleteLater();
    return ok;
}

// ---------------------------------------------------------------- jobs ------

QString ComfyBackend::resolveModel(int steps) const
{
    const QString forced = qEnvironmentVariable("QWEN_MODEL").trimmed().toLower();
    if (forced == "turbo")
        return QString::fromLatin1(kTurboModel);
    if (forced == "base")
        return QString::fromLatin1(kBaseModel);
    return steps <= 12 ? QString::fromLatin1(kTurboModel) : QString::fromLatin1(kBaseModel);
}

QString ComfyBackend::resolveTextEncoder() const
{
    const QString forced = qEnvironmentVariable("QWEN_TE").trimmed().toLower();
    return forced == "heretic" ? QString::fromLatin1(kHereticTe) : QString::fromLatin1(kStockTe);
}

QString ComfyBackend::mapSampler(const QString &uiName) const
{
    const QString n = uiName.trimmed().toLower();
    if (n.startsWith("euler a"))
        return "euler_ancestral";
    if (n.contains("dpm++ 2m"))
        return "dpmpp_2m";
    if (n.contains("dpm++ sde"))
        return "dpmpp_sde";
    if (n.contains("unipc"))
        return "uni_pc";
    return "euler";
}

QJsonObject ComfyBackend::buildGraph(const Job &job) const
{
    const GenerationRequest &r = job.req;
    const bool turbo = job.modelFile.contains("turbo");
    const int side = qMax(r.size.width(), r.size.height());
    const bool masked = r.mode == PageMode::MaskInfill && !job.maskName.isEmpty();

    QJsonObject g;
    g.insert("1", obj({{"class_type", "UnetLoaderGGUF"},
                       {"inputs", obj({{"unet_name", job.modelFile}})}}));
    g.insert("2", obj({{"class_type", "CLIPLoader"},
                       {"inputs", obj({{"clip_name", job.teFile},
                                       {"type", "qwen_image"},
                                       {"device", "default"}})}}));
    g.insert("3", obj({{"class_type", "VAELoader"},
                       {"inputs", obj({{"vae_name", QString::fromLatin1(kVae)}})}}));

    // image_1 is the primary image, references fill image_2..image_16
    QJsonObject teInputs{
        {"clip", link("2", 0)},
        {"prompt", r.prompt},
        {"negative_prompt", r.negativePrompt},
        {"vae", link("3", 0)},
        {"resolution", masked ? qMax(job.workSize.width(), job.workSize.height()) : side},
    };
    for (int i = 0; i < job.uploaded.size() && i < 16; ++i) {
        const QString nodeId = QString("2%1").arg(i + 1);
        g.insert(nodeId, obj({{"class_type", "LoadImage"},
                              {"inputs", obj({{"image", job.uploaded.at(i)}})}}));
        teInputs.insert(QString("images.image_%1").arg(i + 1), link(nodeId, 0));
    }
    g.insert("4", obj({{"class_type", "TextEncodeQwenImage21"}, {"inputs", teInputs}}));

    // latent: reference-derived, or a noise mask over the crop. The crop itself
    // (and its mask) already carries the surrounding structure, so only the
    // painted pixels are re-sampled and everything else is preserved by the
    // sampler rather than pasted back afterwards.
    QJsonValue latent = link("4", 2);
    if (masked) {
        g.insert("40", obj({{"class_type", "LoadImage"},
                            {"inputs", obj({{"image", job.maskName}})}}));
        g.insert("41", obj({{"class_type", "ImageToMask"},
                            {"inputs", obj({{"image", link("40", 0)},
                                            {"channel", "red"}})}}));
        g.insert("42", obj({{"class_type", "VAEEncode"},
                            {"inputs", obj({{"pixels", link("21", 0)},
                                            {"vae", link("3", 0)}})}}));
        g.insert("43", obj({{"class_type", "SetLatentNoiseMask"},
                            {"inputs", obj({{"samples", link("42", 0)},
                                            {"mask", link("41", 0)}})}}));
        latent = link("43", 0);
    }

    if (r.batch > 1) {
        g.insert("45", obj({{"class_type", "RepeatLatentBatch"},
                            {"inputs", obj({{"samples", latent},
                                            {"amount", r.batch}})}}));
        latent = link("45", 0);
    }

    if (turbo) {
        g.insert("5", obj({{"class_type", "RandomNoise"},
                           {"inputs", obj({{"noise_seed", job.seed}})}}));
        g.insert("6", obj({{"class_type", "KSamplerSelect"},
                           {"inputs", obj({{"sampler_name", "euler"}})}}));
        g.insert("7", obj({{"class_type", "ManualSigmas"},
                           {"inputs", obj({{"sigmas", QString::fromLatin1(kTurboSigmas)}})}}));
        g.insert("8", obj({{"class_type", "BasicGuider"},
                           {"inputs", obj({{"model", link("1", 0)},
                                           {"conditioning", link("4", 0)}})}}));
        g.insert("9", obj({{"class_type", "SamplerCustomAdvanced"},
                           {"inputs", obj({{"noise", link("5", 0)},
                                           {"guider", link("8", 0)},
                                           {"sampler", link("6", 0)},
                                           {"sigmas", link("7", 0)},
                                           {"latent_image", latent}})}}));
    } else {
        g.insert("9", obj({{"class_type", "KSampler"},
                           {"inputs", obj({{"model", link("1", 0)},
                                           {"positive", link("4", 0)},
                                           {"negative", link("4", 1)},
                                           {"latent_image", latent},
                                           {"seed", job.seed},
                                           {"steps", job.steps},
                                           {"cfg", r.cfg},
                                           {"sampler_name", mapSampler(r.sampler)},
                                           {"scheduler", "simple"},
                                           {"denoise", 1.0}})}}));
    }

    g.insert("10", obj({{"class_type", "VAEDecode"},
                        {"inputs", obj({{"samples", link("9", 0)},
                                        {"vae", link("3", 0)}})}}));

    QJsonValue finalImage = link("10", 0);

    g.insert("11", obj({{"class_type", "SaveImage"},
                        {"inputs", obj({{"images", finalImage},
                                        {"filename_prefix", QString("studio/%1").arg(pageModeId(r.mode))}})}}));
    return g;
}

QString ComfyBackend::submit(const GenerationRequest &request)
{
    if (state() != Running) {
        emit jobFailed(QString(), "ComfyUI is not running - press Start server");
        return QString();
    }

    Job *job = new Job;
    job->id = QUuid::createUuid().toString().remove('{').remove('}').left(8);
    job->req = request;
    job->steps = request.steps > 0 ? request.steps : 30;
    job->seed = request.seed >= 0 ? request.seed
                                  : QRandomGenerator::global()->bounded(1 << 20);
    job->modelFile = resolveModel(job->steps);
    job->teFile = resolveTextEncoder();
    job->clock.start();
    m_jobs.insert(job->id, job);

    log(QString("Job %1: %2 | %3 steps, cfg %4, %5x%6, seed %7 | %8")
            .arg(job->id, pageModeName(request.mode))
            .arg(job->steps)
            .arg(request.cfg)
            .arg(request.size.width())
            .arg(request.size.height())
            .arg(job->seed)
            .arg(job->modelFile));

    const QString id = job->id;
    // Uploads hit a loopback server, but still do them after returning so the
    // caller sees an id immediately.
    QTimer::singleShot(0, this, [this, id]() {
        Job *j = m_jobs.value(id);
        if (!j)
            return;
        beginUploads(j);
        if (m_jobs.contains(id))
            queueJob(j);
    });

    return id;
}

bool ComfyBackend::prepareMaskedCrop(Job *job, QString *error)
{
    const QString primary = job->req.images.value(0);
    job->originalPath = primary;

    QImage original(primary);
    QImage mask(job->req.maskPath);
    if (original.isNull() || mask.isNull()) {
        *error = QString("Cannot read %1 or the mask").arg(QFileInfo(primary).fileName());
        return false;
    }
    if (mask.size() != original.size())
        mask = mask.scaled(original.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    const QImage gray = mask.convertToFormat(QImage::Format_Grayscale8);
    const QRect bounds = maskBounds(gray);
    if (bounds.isNull()) {
        *error = "the mask is empty";
        return false;
    }

    // Context margin: the model has to see the structure around the hole, or it
    // invents geometry that no longer lines up with the untouched pixels.
    const int pad = qMax(64, int(qMax(bounds.width(), bounds.height()) * 0.45));
    QRect crop = bounds.adjusted(-pad, -pad, pad, pad).intersected(original.rect());

    // Snap to the 16 px VAE grid so nothing is resampled twice afterwards.
    int x0 = (crop.left() / 16) * 16;
    int y0 = (crop.top() / 16) * 16;
    int x1 = qMin(original.width(), ((crop.right() + 16) / 16) * 16);
    int y1 = qMin(original.height(), ((crop.bottom() + 16) / 16) * 16);
    x0 = qMax(0, x0);
    y0 = qMax(0, y0);
    x1 = qMax(x0 + 64, x1 - ((x1 - x0) % 16));
    y1 = qMax(y0 + 64, y1 - ((y1 - y0) % 16));
    const QRect rect(x0, y0, x1 - x0, y1 - y0);

    // Work size: the model's comfortable budget. Small crops are upscaled so the
    // masked detail is rendered at ~1024 px instead of a thumbnail's worth.
    const double scale = qBound(0.5, 1024.0 / qMax(rect.width(), rect.height()), 3.0);
    const QSize work(qMax(64, int(std::lround(rect.width() * scale / 16.0) * 16)),
                     qMax(64, int(std::lround(rect.height() * scale / 16.0) * 16)));

    const QImage cropImage = original.copy(rect)
                                 .convertToFormat(QImage::Format_RGB888)
                                 .scaled(work, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const QImage cropMask = gray.copy(rect)
                                .scaled(work, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    QString dir = QDir::tempPath() + "/qwenstudio-inpaint";
    QDir().mkpath(dir);
    job->cropImagePath = dir + QString("/crop_%1.png").arg(job->id);
    job->cropMaskPath = dir + QString("/mask_%1.png").arg(job->id);
    if (!cropImage.save(job->cropImagePath, "PNG") || !cropMask.save(job->cropMaskPath, "PNG")) {
        *error = "cannot write the crop files";
        return false;
    }

    job->cropRect = rect;
    job->workSize = work;
    job->featherMask = gray.copy(rect);   // original scale, for the local blend

    log(QString("Infill: mask %1x%2 at %3,%4 -> crop %5x%6 -> model sees %7x%8")
            .arg(bounds.width()).arg(bounds.height())
            .arg(bounds.x()).arg(bounds.y())
            .arg(rect.width()).arg(rect.height())
            .arg(work.width()).arg(work.height()));
    return true;
}

void ComfyBackend::beginUploads(Job *job)
{
    QStringList sources = job->req.images;
    QString maskSource = job->req.maskPath;

    if (job->req.mode == PageMode::MaskInfill && !maskSource.isEmpty()
        && QFileInfo::exists(maskSource) && !sources.isEmpty()) {
        QString error;
        if (!prepareMaskedCrop(job, &error)) {
            failJob(job, QString("Mask infill setup failed: %1").arg(error));
            return;
        }
        sources[0] = job->cropImagePath;
        maskSource = job->cropMaskPath;
    }

    for (const QString &path : sources) {
        QString stored;
        QString error;
        if (!httpUpload(path, "image", &stored, &error)) {
            failJob(job, QString("Upload failed for %1: %2")
                              .arg(QFileInfo(path).fileName(), error));
            return;
        }
        job->uploaded.append(stored);
    }

    if (job->req.mode == PageMode::MaskInfill && !maskSource.isEmpty()
        && QFileInfo::exists(maskSource)) {
        QString stored;
        QString error;
        if (!httpUpload(maskSource, "image", &stored, &error)) {
            failJob(job, QString("Mask upload failed: %1").arg(error));
            return;
        }
        job->maskName = stored;
    }
}

void ComfyBackend::queueJob(Job *job)
{
    const QJsonObject graph = buildGraph(*job);
    QJsonObject body{
        {"prompt", graph},
        {"client_id", QUuid::createUuid().toString().remove('{').remove('}')},
    };

    QJsonObject reply;
    QString error;
    if (!httpPostJson("/prompt", body, &reply, &error, 60000)) {
        failJob(job, QString("ComfyUI rejected the graph: %1").arg(error));
        return;
    }

    const QString promptId = reply.value("prompt_id").toString();
    if (promptId.isEmpty()) {
        failJob(job, "ComfyUI did not return a prompt id");
        return;
    }

    job->promptId = promptId;
    m_byPrompt.insert(promptId, job->id);
    m_activeJob = job->id;
    emit progress(job->id, 2, "Queued");
    log(QString("Job %1 queued as prompt %2").arg(job->id, promptId));
    m_poll->start();
}

void ComfyBackend::pollJobs()
{
    if (m_jobs.isEmpty()) {
        m_poll->stop();
        return;
    }

    const QStringList ids = m_jobs.keys();
    for (const QString &id : ids) {
        Job *job = m_jobs.value(id);
        if (!job || job->promptId.isEmpty() || job->cancelled)
            continue;

        QJsonObject history;
        QString error;
        if (!httpGet("/history/" + job->promptId, &history, &error, 8000))
            continue;

        if (!history.contains(job->promptId)) {
            const int elapsed = int(job->clock.elapsed() / 1000);
            if (job->id == m_activeJob && m_total == 0) {
                emit progress(job->id, qMin(10 + elapsed / 4, 60),
                              QString("Sampling (%1s)").arg(elapsed));
            }
            continue;
        }

        completeJob(job, history.value(job->promptId).toObject());
        return; // job map changed
    }
}

bool ComfyBackend::blendMaskedResult(Job *job, const QString &downloadedPath, QString *error)
{
    const QRect rect = job->cropRect;
    QImage patch(downloadedPath);
    QImage original(job->originalPath);
    if (patch.isNull() || original.isNull() || !rect.isValid()) {
        *error = "cannot read the result or the original image";
        return false;
    }

    if (patch.size() != rect.size())
        patch = patch.scaled(rect.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    patch = patch.convertToFormat(QImage::Format_RGB888);
    QImage out = original.convertToFormat(QImage::Format_RGB888);

    // A few pixels of feather, and only at the painted edge: enough to avoid a
    // hard cut, small enough that untouched geometry is not dragged around.
    const int feather = qBound(3, qMax(rect.width(), rect.height()) / 90, 12);
    const QImage alpha = blurMask(job->featherMask, feather);

    for (int y = 0; y < rect.height(); ++y) {
        uchar *dst = out.scanLine(rect.y() + y);
        const uchar *src = patch.constScanLine(y);
        const uchar *a = alpha.constScanLine(y);
        for (int x = 0; x < rect.width(); ++x) {
            const int w = a[x];
            if (w == 0)
                continue;
            uchar *d = dst + (rect.x() + x) * 3;
            const uchar *s2 = src + x * 3;
            if (w == 255) {
                d[0] = s2[0];
                d[1] = s2[1];
                d[2] = s2[2];
                continue;
            }
            for (int c = 0; c < 3; ++c)
                d[c] = uchar((d[c] * (255 - w) + s2[c] * w) / 255);
        }
    }

    if (!out.save(downloadedPath, "PNG")) {
        *error = "cannot write the blended image";
        return false;
    }
    log(QString("Infill: blended %1x%2 patch at %3,%4 into the original (feather %5 px)")
            .arg(rect.width()).arg(rect.height()).arg(rect.x()).arg(rect.y()).arg(feather));
    return true;
}

void ComfyBackend::completeJob(Job *job, const QJsonObject &entry)
{
    const QString status = entry.value("status").toObject().value("status_str").toString();
    if (status == "error") {
        QString message = "ComfyUI reported an execution error";
        const QJsonArray messages = entry.value("status").toObject().value("messages").toArray();
        for (const QJsonValue &m : messages) {
            const QJsonArray pair = m.toArray();
            if (pair.size() == 2 && pair.at(0).toString() == "execution_error") {
                const QJsonObject info = pair.at(1).toObject();
                message = info.value("exception_message").toString(message).trimmed();
                break;
            }
        }
        failJob(job, message);
        return;
    }

    QVector<QJsonObject> images;
    const QJsonObject outputs = entry.value("outputs").toObject();
    for (auto it = outputs.begin(); it != outputs.end(); ++it) {
        const QJsonArray list = it.value().toObject().value("images").toArray();
        for (const QJsonValue &v : list)
            images.append(v.toObject());
    }
    if (images.isEmpty()) {
        failJob(job, "ComfyUI finished without producing an image");
        return;
    }

    const QString outDir = AppSettings::instance().outputDir();
    QDir().mkpath(outDir);

    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
    GenerationResult result;
    result.jobId = job->id;
    result.mode = job->req.mode;

    for (int i = 0; i < images.size(); ++i) {
        const QJsonObject img = images.at(i);
        QUrlQuery q;
        q.addQueryItem("filename", img.value("filename").toString());
        q.addQueryItem("subfolder", img.value("subfolder").toString());
        q.addQueryItem("type", img.value("type").toString("output"));

        QString base = QString("studio_%1_%2").arg(stamp).arg(i + 1, 2, 10, QChar('0'));
        QString path = QDir(outDir).filePath(base + ".png");
        int n = 1;
        while (QFileInfo::exists(path))
            path = QDir(outDir).filePath(QString("%1-%2.png").arg(base).arg(++n));

        QString error;
        if (!httpDownload(q.toString(QUrl::FullyEncoded), path, &error)) {
            failJob(job, QString("Could not save the result: %1").arg(error));
            return;
        }

        if (job->req.mode == PageMode::MaskInfill && !job->originalPath.isEmpty()
            && job->cropRect.isValid()) {
            QString blendError;
            if (!blendMaskedResult(job, path, &blendError)) {
                failJob(job, QString("Could not blend the repaired crop: %1").arg(blendError));
                return;
            }
        }

        writeSidecar(*job, path);
        GeneratedImage gi;
        gi.path = path;
        result.images.append(gi);
        log(QString("Saved %1").arg(QDir::toNativeSeparators(path)));
    }

    emit progress(job->id, 100, "Done");
    m_jobs.remove(job->id);
    m_byPrompt.remove(job->promptId);
    if (m_activeJob == job->id)
        m_activeJob.clear();
    m_step = m_total = 0;
    delete job;

    emit jobFinished(result);
}

void ComfyBackend::writeSidecar(const Job &job, const QString &imagePath) const
{
    if (!AppSettings::instance().data().sidecar)
        return;

    const QFileInfo fi(imagePath);
    QFile f(QDir(fi.absolutePath()).filePath(fi.completeBaseName() + ".json"));
    if (!f.open(QIODevice::WriteOnly))
        return;

    QJsonObject meta{
        {"mode", pageModeId(job.req.mode)},
        {"prompt", job.req.prompt},
        {"negative_prompt", job.req.negativePrompt},
        {"width", job.req.size.width()},
        {"height", job.req.size.height()},
        {"steps", job.steps},
        {"cfg", job.req.cfg},
        {"sampler", job.req.sampler},
        {"seed", job.seed},
        {"model", job.modelFile},
        {"text_encoder", job.teFile},
        {"mask", job.req.maskPath},
        {"created", QDateTime::currentDateTime().toString(Qt::ISODate)},
    };
    f.write(QJsonDocument(meta).toJson(QJsonDocument::Indented));
    f.close();
}

void ComfyBackend::cancel(const QString &jobId)
{
    Job *job = m_jobs.value(jobId);
    if (!job)
        return;

    job->cancelled = true;
    QString error;
    httpPostEmpty("/interrupt", &error);

    if (!job->promptId.isEmpty()) {
        QJsonObject body{{"delete", QJsonArray{job->promptId}}};
        QJsonObject ignored;
        httpPostJson("/queue", body, &ignored, &error, 5000);
        m_byPrompt.remove(job->promptId);
    }

    log(QString("Job %1 cancelled").arg(jobId));

    GenerationResult result;
    result.jobId = jobId;
    result.mode = job->req.mode;
    result.cancelled = true;

    m_jobs.remove(jobId);
    if (m_activeJob == jobId)
        m_activeJob.clear();
    m_step = m_total = 0;
    delete job;

    emit jobFinished(result);
}

void ComfyBackend::failJob(Job *job, const QString &message)
{
    if (!job)
        return;
    const QString id = job->id;
    log(QString("Job %1 failed: %2").arg(id, message));
    m_jobs.remove(id);
    if (!job->promptId.isEmpty())
        m_byPrompt.remove(job->promptId);
    if (m_activeJob == id)
        m_activeJob.clear();
    delete job;
    emit jobFailed(id, message);
}
