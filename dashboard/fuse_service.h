#pragma once

#include <QObject>
#include <QVariantMap>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QTimer>
#include <QJsonObject>
#include <QMap>
#include <functional>
#include <QtQmlIntegration/qqmlintegration.h>

// Development simulator transport, deliberately separate from VR tracking.
class FuseService : public QObject
{
	Q_OBJECT
	QML_ELEMENT
	QML_SINGLETON
	Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
	Q_PROPERTY(QVariantList devices READ devices NOTIFY changed)
	Q_PROPERTY(QVariantMap tracking READ tracking NOTIFY changed)
	Q_PROPERTY(QVariantMap calibration READ calibration NOTIFY changed)
	Q_PROPERTY(bool connected READ connected NOTIFY changed)
	Q_PROPERTY(bool busy READ busy NOTIFY changed)
	Q_PROPERTY(bool running READ running NOTIFY changed)
	Q_PROPERTY(bool workerAvailable READ workerAvailable NOTIFY changed)
	Q_PROPERTY(QString error READ error NOTIFY changed)
	Q_PROPERTY(QString cameraError READ cameraError NOTIFY changed)
	Q_PROPERTY(QString serviceUrl READ serviceUrl CONSTANT)

public:
	explicit FuseService(QObject * parent = nullptr);
	~FuseService() override;
	QVariantMap state() const { return m_state; }
	QVariantList devices() const { return m_devices; }
	QVariantMap tracking() const { return m_tracking; }
	QVariantMap calibration() const { return m_calibration; }
	bool connected() const { return m_connected; }
	bool busy() const { return m_busy; }
	bool running() const { return m_process.state() != QProcess::NotRunning; }
	bool workerAvailable() const;
	QString error() const { return m_error; }
	QString cameraError() const { return m_cameraError; }
	QString serviceUrl() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_port); }
	Q_INVOKABLE void connectService();
	Q_INVOKABLE void startWorker();
	Q_INVOKABLE void stopWorker();
	Q_INVOKABLE void setControl(QString name, bool value);
	Q_INVOKABLE void setCamera(QString id, bool enabled);
	Q_INVOKABLE void setEstimation(QString id, bool enabled);
	Q_INVOKABLE void calibrationCommand(QString action, QString id, int columns, int rows, double squareMM);

signals:
	void changed();

private:
	using Completion = std::function<void(QJsonObject, QString, bool)>;
	void request(const QString & path, const QJsonObject & body, Completion done);
	void poll();
	void finish();
	void fail(const QString & message);
	void launch();
	QNetworkAccessManager m_network;
	QProcess m_process;
	QTimer m_poll;
	QTimer m_kill;
	const QString m_worker;
	const QString m_python;
	const quint16 m_port;
	QVariantMap m_state;
	QVariantList m_devices;
	QVariantMap m_tracking;
	QVariantMap m_calibration;
	QJsonObject m_calibrationCommand;
	QMap<QString, bool> m_controls;
	QMap<QString, bool> m_cameraControls;
	QMap<QString, bool> m_estimationControls;
	QString m_error;
	QString m_cameraError;
	bool m_connected = false;
	bool m_busy = false;
	bool m_launchRequested = false;
	quint64 m_generation = 0;
};
