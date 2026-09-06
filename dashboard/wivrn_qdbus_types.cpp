#include "wivrn_qdbus_types.h"

QDBusArgument & operator<<(QDBusArgument & arg, const QSize & size)
{
	arg.beginStructure();
	arg << uint32_t(size.width()) << uint32_t(size.height());
	arg.endStructure();
	return arg;
}

const QDBusArgument & operator>>(const QDBusArgument & arg, QSize & size)
{
	uint32_t width = 0;
	uint32_t height = 0;
	arg.beginStructure();
	arg >> width >> height;
	arg.endStructure();
	size = QSize(int(width), int(height));
	return arg;
}

#include "moc_wivrn_qdbus_types.cpp"
