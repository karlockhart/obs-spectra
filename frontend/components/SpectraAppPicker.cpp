#include "SpectraAppPicker.hpp"

#include <OBSApp.hpp>

#include <qt-wrappers.hpp>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <util/windows/window-helpers.h>
#endif

#include "moc_SpectraAppPicker.cpp"

namespace {

#ifdef _WIN32
struct AppsContext {
	DWORD selfPid;
	/* lowercase exe -> {exe, window title} */
	QMap<QString, QPair<QString, QString>> apps;
};

BOOL CALLBACK EnumAppWindowProc(HWND hwnd, LPARAM param)
{
	AppsContext *ctx = reinterpret_cast<AppsContext *>(param);

	if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) {
		return TRUE;
	}
	if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) {
		return TRUE;
	}

	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == ctx->selfPid) {
		return TRUE;
	}

	struct dstr title = {0};
	ms_get_window_title(&title, hwnd);
	QString titleStr = QString::fromUtf8(title.array ? title.array : "");
	dstr_free(&title);
	if (titleStr.isEmpty()) {
		return TRUE;
	}

	struct dstr exe = {0};
	if (!ms_get_window_exe(&exe, hwnd)) {
		return TRUE;
	}
	QString exeName = QString::fromUtf8(exe.array);
	dstr_free(&exe);

	if (exeName.compare("explorer.exe", Qt::CaseInsensitive) == 0) {
		return TRUE;
	}
	if (!ctx->apps.contains(exeName.toLower())) {
		ctx->apps.insert(exeName.toLower(), {exeName, titleStr});
	}
	return TRUE;
}
#endif

} // namespace

SpectraRunningAppsCombo::SpectraRunningAppsCombo(QWidget *parent) : QComboBox(parent)
{
	setEditable(true);
	setInsertPolicy(QComboBox::NoInsert);
	setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	setMinimumContentsLength(16);
	/* Completing would paste window titles into typed patterns */
	setCompleter(nullptr);
	lineEdit()->setPlaceholderText(QTStr("Spectra.Loop.Apps.Placeholder"));
}

void SpectraRunningAppsCombo::showPopup()
{
	Refresh();
	QComboBox::showPopup();
}

void SpectraRunningAppsCombo::Refresh()
{
	QString text = currentText();
	clear();

#ifdef _WIN32
	AppsContext ctx = {GetCurrentProcessId(), {}};
	EnumWindows(EnumAppWindowProc, reinterpret_cast<LPARAM>(&ctx));
	for (const auto &app : ctx.apps) {
		QString title = app.second;
		if (title.length() > 50) {
			title = title.left(47) + "...";
		}
		addItem(QStringLiteral("%1  %2  %3").arg(app.first, QChar(0x2014), title), app.first);
	}
#endif

	setCurrentIndex(-1);
	setEditText(text);
}

SpectraAppPicker::SpectraAppPicker(bool fullscreenOption, QWidget *parent) : QWidget(parent)
{
	list = new QListWidget();
	list->setSelectionMode(QAbstractItemView::ExtendedSelection);
	list->setMaximumHeight(110);

	entry = new SpectraRunningAppsCombo();
	entry->setToolTip(QTStr("Spectra.Loop.Settings.ProcessesTip"));
	/* Choosing a running app from the dropdown adds it right away */
	connect(entry, &QComboBox::activated, this, [this](int index) {
		AddPattern(entry->itemData(index).toString());
		entry->setEditText(QString());
	});
	/* Enter adds the typed pattern instead of closing the dialog */
	entry->lineEdit()->installEventFilter(this);

	add = new QPushButton(QTStr("Add"));
	add->setAutoDefault(false);
	connect(add, &QPushButton::clicked, this, &SpectraAppPicker::AddEntry);

	remove = new QPushButton(QTStr("Remove"));
	remove->setAutoDefault(false);
	remove->setEnabled(false);
	connect(remove, &QPushButton::clicked, this, &SpectraAppPicker::RemoveSelected);
	connect(list, &QListWidget::itemSelectionChanged, this,
		[this]() { remove->setEnabled(!list->selectedItems().isEmpty()); });

	anyFullscreen = new QCheckBox(QTStr("Spectra.Loop.Apps.AnyFullscreen"));
	anyFullscreen->setToolTip(QTStr("Spectra.Loop.Apps.AnyFullscreenTip"));
	anyFullscreen->setVisible(fullscreenOption);

	auto *entryRow = new QHBoxLayout();
	entryRow->setContentsMargins(0, 0, 0, 0);
	entryRow->addWidget(entry, 1);
	entryRow->addWidget(add);
	entryRow->addWidget(remove);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(list);
	layout->addLayout(entryRow);
	layout->addWidget(anyFullscreen);
}

void SpectraAppPicker::SetEntryHint(const QString &placeholder, const QString &tip)
{
	entry->lineEdit()->setPlaceholderText(placeholder);
	entry->setToolTip(tip);
}

bool SpectraAppPicker::eventFilter(QObject *obj, QEvent *event)
{
	if (obj == entry->lineEdit() && event->type() == QEvent::KeyPress) {
		int key = static_cast<QKeyEvent *>(event)->key();
		if (key == Qt::Key_Return || key == Qt::Key_Enter) {
			AddEntry();
			return true;
		}
	}
	return QWidget::eventFilter(obj, event);
}

void SpectraAppPicker::AddEntry()
{
	/* The typed text may hold several comma-separated patterns */
	for (const QString &pattern : entry->currentText().split(QRegularExpression("[,;]"))) {
		AddPattern(pattern);
	}
	entry->setEditText(QString());
}

void SpectraAppPicker::AddPattern(const QString &pattern_)
{
	QString pattern = pattern_.trimmed();
	if (pattern.isEmpty()) {
		return;
	}
	for (int i = 0; i < list->count(); i++) {
		if (list->item(i)->text().compare(pattern, Qt::CaseInsensitive) == 0) {
			list->setCurrentRow(i);
			return;
		}
	}
	list->addItem(pattern);
}

void SpectraAppPicker::RemoveSelected()
{
	qDeleteAll(list->selectedItems());
}

void SpectraAppPicker::SetPatterns(const QString &patterns)
{
	list->clear();
	for (const QString &pattern : patterns.split(QRegularExpression("[,;]"))) {
		AddPattern(pattern);
	}
}

QString SpectraAppPicker::Patterns() const
{
	QStringList patterns;
	for (int i = 0; i < list->count(); i++) {
		patterns << list->item(i)->text();
	}
	/* Text typed but not added yet still counts */
	for (const QString &pattern : entry->currentText().split(QRegularExpression("[,;]"))) {
		if (!pattern.trimmed().isEmpty() && !patterns.contains(pattern.trimmed(), Qt::CaseInsensitive)) {
			patterns << pattern.trimmed();
		}
	}
	return patterns.join(", ");
}

void SpectraAppPicker::SetAnyFullscreen(bool enabled)
{
	anyFullscreen->setChecked(enabled);
}

bool SpectraAppPicker::AnyFullscreen() const
{
	return anyFullscreen->isChecked();
}
