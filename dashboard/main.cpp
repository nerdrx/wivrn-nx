#include "version.h"
#include <KAboutData>
#include <KIconTheme>
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <QApplication>
#include <QCoroQml>
#include <QCoroQmlTask>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QUrl>
#include <QtQml>

#include <QQuickWindow>
#include <QStringLiteral>

#ifdef WIVRN_HAVE_KCOLORSCHEME
#include <KColorScheme>
#include <KSharedConfig>
#include <QSettings>
#endif

int main(int argc, char * argv[])
{
	KIconTheme::initTheme();
	QApplication app(argc, argv);
	KLocalizedString::setApplicationDomain("wivrn-dashboard");
	app.setOrganizationName(QStringLiteral("wivrn"));

	KAboutData aboutData(
	        QStringLiteral("wivrn-dashboard"),
	        i18nc("@title", "WiVRn"),
	        wivrn::display_version(),
	        i18n("WiVRn server"),
	        KAboutLicense::GPL_V3,
	        i18n("(c) 2022-2026 WiVRn development team"));

	aboutData.setDesktopFileName(QStringLiteral("io.github.wivrn.wivrn"));
	aboutData.setOrganizationDomain("wivrn.github.io");

	aboutData.setHomepage("https://github.com/WiVRn/WiVRn");
	aboutData.setBugAddress("https://github.com/WiVRn/WiVRn/issues");
	aboutData.setProgramLogo(QIcon(":/qml/wivrn.svg"));

	aboutData.addComponent(
	        i18n("Monado"),
	        i18n("OpenXR runtime"),
	        "",
	        "https://monado.dev/",
	        KAboutLicense::BSL_V1);

	// Set aboutData as information about the app
	KAboutData::setApplicationData(aboutData);

#ifdef WIVRN_HAVE_KCOLORSCHEME
	// NX design language: apply the deep-space color scheme app-locally, the
	// same way KColorSchemeManager does, so the system scheme is untouched.
	// The "NX look" toggle in the dashboard settings turns it off.
	if (QSettings().value("nx_theme", true).toBool())
	{
		const auto scheme = QStringLiteral(":/nx.colors");
		app.setProperty("KDE_COLOR_SCHEME_PATH", scheme);
		app.setPalette(KColorScheme::createApplicationPalette(KSharedConfig::openConfig(scheme)));
	}
#endif

	QCoro::Qml::registerTypes();

	// Work around QTBUG-45105, QTBUG-46074, QTBUG-51112: flicker when resizing
	QQuickWindow::setSceneGraphBackend(QString::fromLatin1("software"));

	QApplication::setStyle(QStringLiteral("breeze"));
	if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE"))
	{
		QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
	}

	QQmlApplicationEngine engine;

	KLocalization::setupLocalizedContext(&engine);
	engine.loadFromModule("io.github.wivrn.wivrn", "Main");

	if (engine.rootObjects().isEmpty())
	{
		return -1;
	}

	return app.exec();
}
