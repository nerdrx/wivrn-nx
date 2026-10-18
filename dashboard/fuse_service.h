#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QMap>
#include <QNetworkAccessManager>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>
#include <functional>

// Development simulator transport, deliberately separate from VR tracking.
class FuseService : public QObject
{
	Q_OBJECT
	QML_ELEMENT
	QML_SINGLETON
	Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)
	Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
	Q_PROPERTY(QVariantMap tracking READ tracking NOTIFY trackingChanged)
	Q_PROPERTY(QVariantMap calibration READ calibration NOTIFY calibrationChanged)
	Q_PROPERTY(QVariantMap alignment READ alignment NOTIFY alignmentChanged)
	Q_PROPERTY(QVariantMap shadow READ shadow NOTIFY shadowChanged)
	Q_PROPERTY(QVariantMap assistance READ assistance NOTIFY assistanceChanged)
	Q_PROPERTY(bool connected READ connected NOTIFY changed)
	Q_PROPERTY(bool busy READ busy NOTIFY changed)
	Q_PROPERTY(bool running READ running NOTIFY changed)
	Q_PROPERTY(bool workerAvailable READ workerAvailable NOTIFY changed)
	Q_PROPERTY(QString error READ error NOTIFY changed)
	Q_PROPERTY(QString cameraError READ cameraError NOTIFY changed)
	Q_PROPERTY(QVariantList cameraChoices READ cameraChoices NOTIFY cameraChoicesChanged)
	Q_PROPERTY(QString serviceUrl READ serviceUrl CONSTANT)

public:
	explicit FuseService(QObject * parent = nullptr);
	~FuseService() override;
	QVariantMap state() const
	{
		return m_state;
	}
	QVariantList cameraChoices() const
	{
		return m_cameraChoices;
	}
	QVariantList devices() const
	{
		return m_devices;
	}
	QVariantMap tracking() const
	{
		return m_tracking;
	}
	QVariantMap calibration() const
	{
		return m_calibration;
	}
	QVariantMap alignment() const
	{
		return m_alignment;
	}
	QVariantMap shadow() const
	{
		return m_shadow;
	}
	QVariantMap assistance() const
	{
		return m_assistance;
	}
	bool connected() const
	{
		return m_connected;
	}
	bool busy() const
	{
		return m_busy;
	}
	bool running() const
	{
		return m_process.state() != QProcess::NotRunning;
	}
	bool workerAvailable() const;
	QString error() const
	{
		return m_error;
	}
	QString cameraError() const
	{
		return m_cameraError;
	}
	QString serviceUrl() const
	{
		return QStringLiteral("http://127.0.0.1:%1").arg(m_port);
	}
	Q_INVOKABLE void connectService();
	Q_INVOKABLE void startWorker();
	Q_INVOKABLE void stopWorker();
	Q_INVOKABLE void setControl(QString name, bool value);
	Q_INVOKABLE void setCamera(QString id, bool enabled);
	Q_INVOKABLE void setEstimation(QString id, bool enabled);
	Q_INVOKABLE void calibrationCommand(QString action, QString id, int columns, int rows, double squareMM);
	Q_INVOKABLE void importLensProfile(QUrl url);
	Q_INVOKABLE void alignmentCommand(const QVariantMap & command);
	Q_INVOKABLE void setShadow(QString id, bool enabled);
	Q_INVOKABLE void setAssistance(bool enabled);
	Q_INVOKABLE void setAssistanceMode(QString mode);
	Q_INVOKABLE void shadowRecordingCommand(QString action, QString id = {});
	Q_INVOKABLE void setCameraSettings(QString id, int width, int height, int fps);

signals:
	void changed();
	void stateChanged();
	void devicesChanged();
	void trackingChanged();
	void calibrationChanged();
	void alignmentChanged();
	void shadowChanged();
	void assistanceChanged();
	void cameraChoicesChanged();

private:
	template <class T>
	void update(T & target, T value, void (FuseService::*signal)())
	{
		if (target == value)
			return;
		target = std::move(value);
		emit(this->*signal)();
	}
	using Completion = std::function<void(QJsonObject, QString, bool)>;
	void request(const QString & path, const QJsonObject & body, Completion done);
	void poll();
	void finish();
	void fail(const QString & message);
	void launch();
	QNetworkAccessManager m_network;
	QProcess m_process;
	QTimer m_poll;
	QElapsedTimer m_diagnostics;
	QVariantList m_cameraChoices;
	QTimer m_kill;
	const QString m_worker;
	const QString m_python;
	const quint16 m_port;
	QVariantMap m_state;
	QVariantList m_devices;
	QVariantMap m_tracking;
	QVariantMap m_calibration;
	QVariantMap m_alignment;
	QVariantMap m_shadow;
	QVariantMap m_assistance;
	QJsonObject m_calibrationCommand;
	QJsonObject m_lensImport;
	QJsonObject m_alignmentCommand;
	QJsonObject m_shadowCommand;
	QJsonObject m_assistanceCommand;
	QJsonObject m_shadowRecordingCommand;
	QMap<QString, bool> m_controls;
	QMap<QString, bool> m_cameraControls;
	QMap<QString, bool> m_estimationControls;
	QJsonObject m_cameraSettings;
	QString m_error;
	QString m_cameraError;
	bool m_connected = false;
	bool m_busy = false;
	bool m_launchRequested = false;
	quint64 m_generation = 0;
};
