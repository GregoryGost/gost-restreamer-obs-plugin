#pragma once

#include "text-lookup.hpp"
#include <QAbstractButton>
#include <QMessageBox>

namespace gost {
// Переводит стандартные ответы по языку OBS, сохраняя их идентификаторы и выбранную кнопку по умолчанию.
inline void localizeConfirmation(QMessageBox &dialog, const TextLookup &text)
{
	// yes и no указывают на существующие стандартные кнопки; новые ответы не добавляются.
	if (auto *yes = dialog.button(QMessageBox::Yes))
		yes->setText(text("Dialog.Yes"));
	if (auto *no = dialog.button(QMessageBox::No))
		no->setText(text("Dialog.No"));
}
} // namespace gost
