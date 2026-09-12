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
#include <iostream>

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
		service.setShadow(QStringLiteral("cam-test"), true);
		until([&] { return service.shadow().value(QStringLiteral("enabled")).toBool(); }, "Shadow POST did not complete", 7000);
		require(service.cameraError().isEmpty(), "Successful shadow {ok:true} response reported an error");
		service.setAssistance(true);
		until([&] { return service.assistance().value(QStringLiteral("enabled")).toBool(); }, "Assistance POST did not complete", 7000);
        require(service.cameraError().isEmpty(), "Successful assistance {ok:true} response reported an error");
        service.setEstimation(QStringLiteral("cam-test"), true);
        service.alignmentCommand({{"action", "auto_start"}, {"id", "cam-test"},
                                  {"anchors", QVariantList{QStringLiteral("head")}},
                                  {"offsets", QVariantMap{{"head", QVariantList{0, 0, 0}}}}});
        until([&] {
            return service.alignment().value("automatic").toMap().value("running").toBool()
                   && service.alignment().value("estimation_received").toBool();
        }, "Queued estimation and automatic alignment must both complete", 7000);
		service.setCameraSettings(QStringLiteral("cam-test"), 1280, 720, 30);
		until([&] {
			for (const auto & value : service.devices())
				if (value.toMap().value(QStringLiteral("id")).toString() == QStringLiteral("cam-test"))
					return value.toMap().value(QStringLiteral("settings")).toMap().value(QStringLiteral("width")).toInt() == 1280;
			return false;
		}, "Camera settings POST/GET reflection did not complete", 7000);
		set_scenario(QStringLiteral("shadow-reject"));
		service.setShadow(QStringLiteral("cam-test"), false);
		until([&] { return !service.cameraError().isEmpty(); }, "Rejected shadow response was not surfaced", 7000);
		set_scenario(QStringLiteral("malformed"));
		until([&] { return !service.error().isEmpty(); }, "Disconnect after camera error timed out", 7000);
		require(service.cameraError().isEmpty(), "Disconnect retained stale camera error");
		set_scenario(QStringLiteral("valid"));
		service.connectService();
		until([&] { return service.connected(); }, "Reconnect after camera error timed out", 7000);

		int stateChanged = 0, devicesChanged = 0, cameraChoicesChanged = 0, calibrationChanged = 0;
		QObject::connect(&service, &FuseService::stateChanged, [&] { ++stateChanged; });
		QObject::connect(&service, &FuseService::devicesChanged, [&] { ++devicesChanged; });
		QObject::connect(&service, &FuseService::cameraChoicesChanged, [&] { ++cameraChoicesChanged; });
		QObject::connect(&service, &FuseService::calibrationChanged, [&] { ++calibrationChanged; });
		int completedPolls = 0;
		QObject::connect(&service, &FuseService::changed, [&] { if (!service.busy()) ++completedPolls; });
		until([&] { return completedPolls >= 5; }, "Repeated snapshot polls did not complete", 7000);
		require(stateChanged == 0 && devicesChanged == 0 && cameraChoicesChanged == 0 && calibrationChanged == 0,
		        "Identical snapshots emitted change signals");

		set_scenario(QStringLiteral("changing-devices"));
		completedPolls = 0;
		devicesChanged = cameraChoicesChanged = 0;
		until([&] { return completedPolls >= 5; }, "Changing snapshot polls did not complete", 7000);
		require(devicesChanged > 0 && cameraChoicesChanged == 0,
		        "Device detail changes did not isolate camera choices signal");
		service.stopWorker();
		return 0;
	}
	catch (const std::exception & error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
