// Standalone integration check; run with run_fuse_service_test.py.
// Requires an existing NX Fuse simulator on 127.0.0.1:8787. No worker is owned.
#include "fuse_service.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <functional>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char * message)
{
	if (!condition)
		throw std::runtime_error(message);
}

static void until(const std::function<bool()> & predicate, const char * message)
{
	QElapsedTimer elapsed;
	elapsed.start();
	while (!predicate() && elapsed.elapsed() < 5000)
	{
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		QThread::msleep(5);
	}
	require(predicate(), message);
}

int main(int argc, char ** argv)
{
	QCoreApplication app(argc, argv);
	try
	{
		{
			FuseService service;
			require(!service.connected() && !service.busy() && !service.running(), "Constructor must remain idle");
			service.connectService();
			until([&] { return service.connected(); }, "Attach timed out");
			require(service.state().value("mode").toString() == "simulation", "Wrong mode");
			require(!service.state().value("joints").toMap().isEmpty(), "Missing synthetic joints");
			require(service.error().isEmpty(), "Unexpected attach error");
			require(!service.running(), "Attached service must not become owned");
			service.startWorker();
			require(!service.running(), "Start on attached service must not create worker");

			for (const QString name : {QStringLiteral("enabled"), QStringLiteral("camera_only"), QStringLiteral("occluded")})
			{
				service.setControl(name, true);
				until([&] { return service.connected() && service.state().value(name).toBool(); }, "Control update timed out");
			}
			// Queued repeated keys must converge to the last requested value.
			service.setControl("enabled", false);
			service.setControl("enabled", true);
			service.setControl("enabled", false);
			service.setControl("camera_only", false);
			service.setControl("occluded", false);
			until([&] {
				const auto state = service.state();
				return service.connected() && !state.value("enabled").toBool()
				       && !state.value("camera_only").toBool() && !state.value("occluded").toBool();
			}, "Restoring disabled simulation controls timed out");
			service.stopWorker();
			require(!service.connected() && !service.running() && !service.busy(), "Disconnect failed");
			require(service.state().isEmpty() && service.devices().isEmpty(), "Disconnected data must clear");
		}
		// First service has been destroyed. Reattachment proves it did not kill the external worker.
		FuseService check;
		check.connectService();
		until([&] { return check.connected(); }, "External worker did not survive disconnect/destruction");
		require(!check.running(), "Second attachment must not own external worker");
		check.stopWorker();
		std::cout << "PASS: attach, controls, coalescing, reset, disconnect, external worker survival\n";
		return 0;
	}
	catch (const std::exception & error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
