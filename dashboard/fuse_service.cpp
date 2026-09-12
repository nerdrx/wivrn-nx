#include "fuse_service.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QStandardPaths>

static quint16 servicePort()
{
	bool ok = false;
	const int port = qEnvironmentVariableIntValue("NX_FUSE_PORT", &ok);
	return ok && port > 0 && port <= 65535 ? quint16(port) : 8787;
}

static QString workerPython(const QString & worker)
{
	const QFileInfo local(QFileInfo(worker).absolutePath() + QStringLiteral("/.venv/bin/python3"));
	return QFileInfo(worker).isAbsolute() && local.isExecutable() ? local.absoluteFilePath()
	                                                             : QStandardPaths::findExecutable("python3");
}

FuseService::FuseService(QObject * parent) :
        QObject(parent),
        m_worker(qEnvironmentVariable("NX_FUSE_WORKER")),
        m_python(workerPython(m_worker)),
        m_port(servicePort())
{
	m_network.setProxy(QNetworkProxy::NoProxy);
	m_poll.setInterval(200); // At most five state/camera cycles per second.
	connect(&m_poll, &QTimer::timeout, this, &FuseService::poll);
	m_kill.setSingleShot(true);
	connect(&m_kill, &QTimer::timeout, this, [this] { m_process.kill(); });
	connect(&m_process, &QProcess::stateChanged, this, [this] { emit changed(); });
	connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
		fail(QStringLiteral("Worker: ") + m_process.errorString());
	});
	connect(&m_process, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
		m_kill.stop();
		fail(QStringLiteral("Owned worker stopped."));
	});
	// Do not retain unbounded worker logs in QProcess buffers.
	connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] { m_process.readAllStandardOutput(); });
	connect(&m_process, &QProcess::readyReadStandardError, this, [this] { m_process.readAllStandardError(); });
}

FuseService::~FuseService()
{
	// QProcess only represents the worker started here; external services are never stopped.
	if (running())
		m_process.kill();
}

bool FuseService::workerAvailable() const
{
	const QFileInfo file(m_worker);
	return !m_python.isEmpty() && file.isAbsolute() && file.isFile() && file.isReadable();
}

void FuseService::fail(const QString & message)
{
	m_connected = false;
	m_state.clear();
	m_devices.clear();
	m_controls.clear();
	m_cameraControls.clear();
	m_estimationControls.clear();
	m_error = message;
	emit changed();
}

void FuseService::request(const QString & path, const QJsonObject & body, Completion done)
{
	QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_port) + path));
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	req.setTransferTimeout(1500);
	req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	auto * reply = body.isEmpty() ? m_network.get(req) : m_network.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
	reply->setReadBufferSize(128 * 1024 + 1);
	// Wall deadline covers slow trickles as well as transfer inactivity.
	auto * deadline = new QTimer(reply);
	deadline->setSingleShot(true);
	connect(deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
	deadline->start(2000);
	connect(reply, &QNetworkReply::readyRead, reply, [reply] {
		if (reply->bytesAvailable() > 128 * 1024)
			reply->abort();
	});
	const auto generation = m_generation;
	connect(reply, &QNetworkReply::finished, this, [this, reply, generation, done = std::move(done)] {
		reply->deleteLater();
		if (generation != m_generation)
			return;
		const bool refused = reply->error() == QNetworkReply::ConnectionRefusedError;
		if (reply->error() != QNetworkReply::NoError)
		{
			QString message = reply->errorString();
			if (reply->bytesAvailable() <= 128 * 1024)
			{
				const auto detail = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("error"));
				if (detail.isString())
					message = detail.toString().left(512);
			}
			done({}, QStringLiteral("NX Fuse: ") + message, refused);
			return;
		}
		if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 || reply->bytesAvailable() > 128 * 1024)
		{
			done({}, QStringLiteral("Unexpected simulation response."), false);
			return;
		}
		QJsonParseError parseError;
		const auto doc = QJsonDocument::fromJson(reply->readAll(), &parseError);
		if (parseError.error != QJsonParseError::NoError || !doc.isObject())
			done({}, QStringLiteral("Invalid simulation JSON."), false);
		else
			done(doc.object(), {}, false);
	});
}

void FuseService::connectService()
{
	if (!m_poll.isActive())
	{
		m_poll.start();
		poll();
	}
}

void FuseService::startWorker()
{
	if (running() || m_connected)
	{
		connectService();
		return;
	}
	// Always probe first: an already-running service belongs to somebody else.
	m_launchRequested = true;
	connectService();
}

void FuseService::launch()
{
	if (!workerAvailable())
	{
		fail(QStringLiteral("Set NX_FUSE_WORKER to the absolute app.py path; python3 is required."));
		return;
	}
	m_process.setProgram(m_python);
	m_process.setArguments({m_worker, QStringLiteral("--port"), QString::number(m_port)});
	m_process.setWorkingDirectory(QFileInfo(m_worker).absolutePath());
	m_process.start();
}

void FuseService::stopWorker()
{
	++m_generation;
	m_poll.stop();
	m_launchRequested = false;
	m_busy = false;
	fail(running() ? QStringLiteral("Stopping owned worker.") : QStringLiteral("Disconnected; external worker left running."));
	if (running())
	{
		m_process.terminate();
		m_kill.start(1500);
	}
}

void FuseService::setControl(QString name, bool value)
{
	if (!m_connected || (name != QStringLiteral("enabled") && name != QStringLiteral("occluded") && name != QStringLiteral("camera_only")))
		return;
	// Three bounded keys, last requested value wins. Poll drains them serially.
	m_controls.insert(name, value);
}

void FuseService::setCamera(QString id, bool enabled)
{
	if (!m_connected || id.isEmpty() || id.size() > 256)
		return;
	for (const auto & device : m_devices)
	{
		if (device.toMap().value(QStringLiteral("id")).toString() == id)
		{
			if (m_cameraControls.size() < 16 || m_cameraControls.contains(id))
				m_cameraControls.insert(id, enabled);
			return;
		}
	}
}

void FuseService::finish()
{
	m_busy = false;
	emit changed();
}

void FuseService::setEstimation(QString id, bool enabled)
{
	if (!m_connected || id.isEmpty() || id.size() > 256)
		return;
	for (const auto & device : m_devices)
	{
		if (device.toMap().value(QStringLiteral("id")).toString() == id)
		{
			if (m_estimationControls.size() < 16 || m_estimationControls.contains(id))
				m_estimationControls.insert(id, enabled);
			return;
		}
	}
}

void FuseService::poll()
{
	if (m_busy)
		return;
	m_busy = true;
	emit changed();
	if (m_connected && !m_estimationControls.isEmpty())
	{
		const auto it = m_estimationControls.cbegin();
		const QJsonObject body{{QStringLiteral("id"), it.key()}, {QStringLiteral("enabled"), it.value()}};
		m_estimationControls.erase(m_estimationControls.begin());
		request(QStringLiteral("/api/camera/estimation"), body, [this](QJsonObject response, QString error, bool) {
			m_cameraError = error.isEmpty() && response.value(QStringLiteral("ok")) == QJsonValue(true)
			                      ? QString() : (error.isEmpty() ? QStringLiteral("Estimation request rejected.") : error);
			finish();
		});
		return;
	}
	if (m_connected && !m_cameraControls.isEmpty())
	{
		const auto it = m_cameraControls.cbegin();
		const QJsonObject body{{QStringLiteral("id"), it.key()}, {QStringLiteral("enabled"), it.value()}};
		m_cameraControls.erase(m_cameraControls.begin());
		request(QStringLiteral("/api/camera/control"), body, [this](QJsonObject response, QString error, bool) {
			m_cameraError = error.isEmpty() && response.value(QStringLiteral("ok")) == QJsonValue(true)
			                      ? QString() : (error.isEmpty() ? QStringLiteral("Camera request rejected.") : error);
			finish();
		});
		return;
	}
	if (m_connected && !m_controls.isEmpty())
	{
		QJsonObject body;
		for (auto it = m_controls.cbegin(); it != m_controls.cend(); ++it)
			body.insert(it.key(), it.value());
		m_controls.clear();
		request(QStringLiteral("/api/control"), body, [this](QJsonObject response, QString error, bool) {
			if (!error.isEmpty() || response.value(QStringLiteral("ok")) != QJsonValue(true))
				fail(error.isEmpty() ? QStringLiteral("Simulation rejected controls.") : error);
			finish();
		});
		return;
	}
	request(QStringLiteral("/api/state"), {}, [this](QJsonObject response, QString error, bool refused) {
		const bool launchRequested = m_launchRequested;
		m_launchRequested = false;
		if (!error.isEmpty())
		{
			fail(error);
			if (launchRequested && refused && !running())
				launch();
			finish();
			return;
		}
		if (response.value(QStringLiteral("mode")) != QJsonValue(QStringLiteral("simulation")) || !response.value(QStringLiteral("joints")).isObject())
		{
			fail(QStringLiteral("Port 8787 is not a compatible NX Fuse simulator."));
			finish();
			return;
		}
		m_state = response.toVariantMap();
		request(QStringLiteral("/api/cameras"), {}, [this](QJsonObject cameras, QString error, bool) {
			if (!error.isEmpty() || !cameras.value(QStringLiteral("devices")).isArray())
				fail(error.isEmpty() ? QStringLiteral("Invalid camera inventory.") : error);
			else
			{
				m_devices = cameras.value(QStringLiteral("devices")).toArray().toVariantList();
				m_connected = true;
				m_error.clear();
			}
			finish();
		});
	});
}
