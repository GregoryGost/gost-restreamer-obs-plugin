#pragma once

#include <QDir>
#include <QTemporaryDir>

// RAII-каталог под закреплённым CMake GOST_TEST_WORKSPACE_DIR. Суффикс XXXXXX даёт каждому экземпляру
// отдельный путь; уничтожение QTemporaryDir удаляет его содержимое. Системный TEMP не используется.
// Вызывающий тест обязан проверить isValid до файловых действий; методы возвращают пути без создания файлов.
class WorkspaceTemp final {
public:
	explicit WorkspaceTemp(const QString &name) : directory_(makeTemplate(name)) {}
	bool isValid() const { return directory_.isValid(); }
	QString path() const { return directory_.path(); }
	QString filePath(const QString &name) const { return directory_.filePath(name); }

private:
	static QString makeTemplate(const QString &name)
	{
		const QString root = QStringLiteral(GOST_TEST_WORKSPACE_DIR);
		QDir().mkpath(root);
		return QDir(root).filePath(name + QStringLiteral("-XXXXXX"));
	}
	QTemporaryDir directory_;
};
