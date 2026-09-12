#include "fuse_service.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <functional>
#include <stdexcept>

static void require(bool condition, const char * message)
{
	if (!condition)
		throw std::runtime_error(message);
}

static void until(const std::function<bool()> & predicate, const char * message, int timeout = 5000)
{
	QElapsedTimer elapsed;
	elapsed.start();
	while (!predicate() && elapsed.elapsed() < timeout)
	{
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		QThread::msleep(5);
	}
	require(predicate(), message);
}

static void set_scenario(const QString & scenario)
{
	QNetworkAccessManager manager;
	QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:") + qEnvironmentVariable("NX_FUSE_PORT")
	                             + QStringLiteral("/__scenario?name=") + scenario));
	QNetworkReply * reply = manager.get(request);
	QEventLoop loop;
	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	loop.exec();
	require(reply->error() == QNetworkReply::NoError, "Fake service scenario update failed");
	reply->deleteLater();
}

static void expect_failure(const QString & scenario, const char * message)
{
	set_scenario(scenario);
	FuseService service;
	service.connectService();
	until([&] { return !service.error().isEmpty(); }, message, 6000);
	service.stopWorker();
	require(!service.connected() && !service.busy(), "Failed transport did not reset state");
}

int main(int argc, char ** argv)
{
	QCoreApplication app(argc, argv);
	try
	{
		require(!qEnvironmentVariable("NX_FUSE_PORT").isEmpty(), "NX_FUSE_PORT must select fake service");
		expect_failure(QStringLiteral("malformed"), "Malformed JSON was accepted");
		expect_failure(QStringLiteral("wrong-mode"), "Wrong service mode was accepted");
		expect_failure(QStringLiteral("oversize"), "Oversize response was accepted");
		expect_failure(QStringLiteral("stalled"), "Stalled response did not time out");

		set_scenario(QStringLiteral("valid"));
		FuseService service;
		service.connectService();
		until([&] { return service.connected(); }, "Reconnect after transport failures timed out", 7000);
		require(service.error().isEmpty(), "Reconnect retained transport error");
		require(service.state().value(QStringLiteral("mode")).toString() == QStringLiteral("simulation"), "Recovery returned wrong mode");
		require(!service.busy(), "Recovered service remained busy");
		service.stopWorker();
		return 0;
	}
	catch (const std::exception & error)
	{
		qCritical("FAIL: %s", error.what());
		return 1;
	}
}
