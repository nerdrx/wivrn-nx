/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVideoFrame>
#include <QVideoSink>
#include <memory>
#include <qqmlintegration.h>

/*!
 * Consumer of the desktop mirror published by the server.
 *
 * The server publishes one PipeWire Video/Source node named "wivrn-headset-view"
 * while a session runs and the "mirror" configuration key is enabled. This class
 * watches the registry for that node and, while started, connects an input
 * stream to it and hands the frames to a QVideoSink taken from a QML
 * VideoOutput.
 *
 * The stream only exists between start() and stop(), so that the server sees no
 * consumer at all (and skips the capture entirely) while the page is hidden.
 */
class MirrorView : public QObject
{
	Q_OBJECT
	QML_NAMED_ELEMENT(MirrorView)

	//! Sink the frames are pushed to, normally bound to VideoOutput.videoSink.
	Q_PROPERTY(QVideoSink * videoSink READ videoSink WRITE setVideoSink NOTIFY videoSinkChanged)
	//! True while frames are actually flowing.
	Q_PROPERTY(bool active READ active NOTIFY activeChanged)
	//! True while the server publishes its mirror node.
	Q_PROPERTY(bool available READ available NOTIFY availableChanged)
	//! False if the dashboard was built without PipeWire or the daemon cannot be reached.
	Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
	//! Empty unless setting the PipeWire side up failed.
	Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)

public:
	explicit MirrorView(QObject * parent = nullptr);
	~MirrorView() override;

	QVideoSink * videoSink() const
	{
		return m_sink;
	}
	void setVideoSink(QVideoSink * sink);

	bool active() const
	{
		return m_active;
	}
	bool available() const
	{
		return m_available;
	}
	bool supported() const
	{
		return m_supported;
	}
	QString errorString() const
	{
		return m_error;
	}

	//! Ask for the stream, it is created as soon as the node exists. Idempotent.
	Q_INVOKABLE void start();
	//! Drop the stream, the server then has no consumer at all. Idempotent.
	Q_INVOKABLE void stop();

Q_SIGNALS:
	void videoSinkChanged();
	void activeChanged();
	void availableChanged();
	void supportedChanged();
	void errorStringChanged();

private:
	struct impl;
	friend struct impl;

	// All of these run on the thread MirrorView lives in, never on the PipeWire one
	void set_active(bool);
	void set_available(bool);
	void present_frame(const QVideoFrame &);
	void reconcile();

	std::unique_ptr<impl> d;
	QPointer<QVideoSink> m_sink;
	QVideoFrame m_last_frame;
	QString m_error;
	bool m_active = false;
	bool m_available = false;
	bool m_supported = false;
	bool m_wanted = false;
};
