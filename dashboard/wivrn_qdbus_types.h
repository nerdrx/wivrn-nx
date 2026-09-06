#pragma once

#include <QDBusArgument>
#include <QObject>
#include <QQmlEngine>
#include <QSize>

// The Server interface carries StreamEyeSize as a (uu) struct, mapped to QSize by the
// QtTypeName annotation in the interface XML. Qt has no D-Bus marshalling for QSize of its own,
// so qdbusxml2cpp's generated property needs these two.
QDBusArgument & operator<<(QDBusArgument & arg, const QSize & size);
const QDBusArgument & operator>>(const QDBusArgument & arg, QSize & size);
Q_DECLARE_METATYPE(QSize)

// Same as XrFovf
struct field_of_view
{
	Q_GADGET
	Q_PROPERTY(float angleLeft READ getAngleLeft WRITE setAngleLeft)
	Q_PROPERTY(float angleRight READ getAngleRight WRITE setAngleRight)
	Q_PROPERTY(float angleUp READ getAngleUp WRITE setAngleUp)
	Q_PROPERTY(float angleDown READ getAngleDown WRITE setAngleDown)
	QML_VALUE_TYPE(field_of_view)
public:
	float angleLeft;
	float angleRight;
	float angleUp;
	float angleDown;

	float getAngleLeft() const
	{
		return angleLeft;
	}

	float getAngleRight() const
	{
		return angleRight;
	}

	float getAngleUp() const
	{
		return angleUp;
	}

	float getAngleDown() const
	{
		return angleDown;
	}

	void setAngleLeft(float value)
	{
		angleLeft = value;
	}

	void setAngleRight(float value)
	{
		angleRight = value;
	}

	void setAngleUp(float value)
	{
		angleUp = value;
	}

	void setAngleDown(float value)
	{
		angleDown = value;
	}
};
