#include "lucida-peers.hpp"
#include "lucida-controller.hpp"

#include <obs-module.h>

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <set>
#include <thread>

namespace lucida {

namespace {

constexpr int kRoleId = Qt::UserRole;
constexpr int kRoleName = Qt::UserRole + 1;
constexpr int kRoleSize = Qt::UserRole + 2;
constexpr int kDownloadMs = 30000; /* per chunk */

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Runs fn on the UI thread if the widget is still there */
template<typename W, typename F> void OnUi(QPointer<W> widget, F fn)
{
	QMetaObject::invokeMethod(
		qApp,
		[widget, fn]() {
			if (widget) {
				fn(widget.data());
			}
		},
		Qt::QueuedConnection);
}

QString PeerStatus(const lan::Peer &p)
{
	if (!p.paired) {
		return T("Lucida.Peers.NotPaired");
	}
	if (p.reachable && !p.pairedThere) {
		return T("Lucida.Peers.PairAgain");
	}
	if (!p.reachable) {
		return p.error.isEmpty() ? T("Lucida.Peers.Paired") : T("Lucida.Peers.Unreachable").arg(p.error);
	}
	QString text = p.game.isEmpty() ? T("Lucida.Peers.Online") : T("Lucida.Peers.Playing").arg(p.game);
	if (p.recording) {
		text += T("Lucida.Peers.Recording");
	}
	if (p.storage) {
		text += T("Lucida.Peers.Storage");
	}
	return text;
}

/* A peer's name as a folder name */
QString SafeName(const QString &name)
{
	QString out;
	for (const QChar c : name) {
		out += QStringLiteral("<>:\"/\\|?*").contains(c) || c.unicode() < 32 ? QChar('_') : c;
	}
	out = out.trimmed();
	while (out.endsWith('.')) {
		out.chop(1);
	}
	return out.isEmpty() ? QStringLiteral("PC") : out;
}

} // namespace

/* --- the dock's list ---------------------------------------------------------------- */

PeersPanel::PeersPanel(Controller *controller_, QWidget *parent) : QWidget(parent), controller(controller_)
{
	title = new QLabel();
	title->setWordWrap(true);
	hint = new QLabel();
	hint->setWordWrap(true);
	hint->setTextInteractionFlags(Qt::TextSelectableByMouse);
	hint->hide();
	list = new QTreeWidget();
	list->setColumnCount(2);
	list->setHeaderLabels({T("Lucida.Peers.Col.Name"), T("Lucida.Peers.Col.Status")});
	list->setRootIsDecorated(false);
	list->setUniformRowHeights(true);
	list->setMaximumHeight(110);
	list->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	list->header()->setStretchLastSection(true);
	list->setContextMenuPolicy(Qt::CustomContextMenu);

	pairButton = new QPushButton(T("Lucida.Peers.Pair"));
	pairButton->setToolTip(T("Lucida.Peers.Pair.Tip"));
	clipsButton = new QPushButton(T("Lucida.Peers.Clips"));
	clipsButton->setToolTip(T("Lucida.Peers.Clips.Tip"));
	logButton = new QPushButton(T("Lucida.Peers.Log"));
	logButton->setToolTip(T("Lucida.Peers.Log.Tip"));
	addButton = new QPushButton(T("Lucida.Peers.Add"));
	addButton->setToolTip(T("Lucida.Peers.Add.Tip"));
	storedButton = new QPushButton(T("Lucida.Peers.Stored"));
	storedButton->setToolTip(T("Lucida.Peers.Stored.Tip"));

	QHBoxLayout *buttons = new QHBoxLayout();
	buttons->addWidget(pairButton);
	buttons->addWidget(clipsButton);
	buttons->addWidget(logButton);
	buttons->addWidget(addButton);
	buttons->addWidget(storedButton);
	buttons->addStretch(1);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(title);
	layout->addWidget(list);
	layout->addWidget(hint);
	layout->addLayout(buttons);

	connect(addButton, &QPushButton::clicked, this, &PeersPanel::AddByAddress);
	connect(storedButton, &QPushButton::clicked, this, &PeersPanel::OpenStored);
	connect(pairButton, &QPushButton::clicked, this, &PeersPanel::Pair);
	connect(clipsButton, &QPushButton::clicked, this, &PeersPanel::OpenClips);
	connect(logButton, &QPushButton::clicked, this, &PeersPanel::browseLog);
	connect(list, &QTreeWidget::itemDoubleClicked, this, &PeersPanel::Activate);
	connect(list, &QTreeWidget::itemSelectionChanged, this, &PeersPanel::UpdateButtons);
	connect(list, &QTreeWidget::customContextMenuRequested, this, &PeersPanel::ContextMenu);
	if (controller) {
		connect(controller, &Controller::lanPeersChanged, this, &PeersPanel::Refresh);
		connect(controller, &Controller::lanStatusChanged, this, &PeersPanel::Refresh);
		connect(controller, &Controller::storageStatusChanged, this, &PeersPanel::UpdateButtons);
	}
	Refresh();
}

void PeersPanel::Refresh()
{
	std::shared_ptr<lan::Service> svc = controller ? controller->Lan() : nullptr;
	setVisible(svc != nullptr);
	if (!svc) {
		return;
	}
	const QString keep = Selected() ? Selected()->data(0, kRoleId).toString() : QString();
	const bool blocked = list->blockSignals(true);
	list->clear();
	std::set<QString> online;
	for (const lan::Peer &p : svc->Peers()) {
		online.insert(p.id);
		QTreeWidgetItem *item =
			new QTreeWidgetItem({p.name.isEmpty() ? p.Fingerprint() : p.name, PeerStatus(p)});
		item->setData(0, kRoleId, p.id);
		item->setToolTip(0, T("Lucida.Peers.Tip").arg(p.Fingerprint(), p.address.toString()).arg(p.port));
		item->setToolTip(1, item->text(1));
		list->addTopLevelItem(item);
	}
	for (const lan::PairedPeer &p : svc->Paired()) {
		if (online.count(p.id)) {
			continue;
		}
		QTreeWidgetItem *item = new QTreeWidgetItem({p.name, T("Lucida.Peers.Offline")});
		item->setData(0, kRoleId, p.id);
		for (int c = 0; c < 2; c++) {
			item->setForeground(c, list->palette().color(QPalette::Disabled, QPalette::Text));
		}
		list->addTopLevelItem(item);
	}
	for (int i = 0; i < list->topLevelItemCount(); i++) {
		if (list->topLevelItem(i)->data(0, kRoleId).toString() == keep) {
			list->setCurrentItem(list->topLevelItem(i));
		}
	}
	list->blockSignals(blocked);
	title->setText(online.empty() ? T("Lucida.Peers.NoneYet").arg(svc->Name())
				      : T("Lucida.Peers.Title").arg(svc->Name()));
	/* nobody found: say what to check, and where this PC is for adding it by hand */
	if (online.empty()) {
		const QStringList addresses = lan::Service::LocalAddresses();
		hint->setText(T("Lucida.Peers.Hint")
				      .arg(addresses.isEmpty() ? T("Lucida.Peers.Hint.NoAddress")
							       : addresses.join(QStringLiteral(", ")))
				      .arg(svc->Port()));
	}
	hint->setVisible(online.empty());
	UpdateButtons();
}

void PeersPanel::UpdateButtons()
{
	std::shared_ptr<lan::Service> svc = controller ? controller->Lan() : nullptr;
	bool anyPaired = false;
	for (const lan::Peer &p : svc ? svc->Peers() : std::vector<lan::Peer>()) {
		anyPaired = anyPaired || (p.paired && p.pairedThere);
	}
	std::optional<lan::Peer> current = Current();
	const bool needsPairing = current && (!current->paired || (current->reachable && !current->pairedThere));
	pairButton->setEnabled(needsPairing);
	pairButton->setText(current && current->paired ? T("Lucida.Peers.PairAgainButton") : T("Lucida.Peers.Pair"));
	clipsButton->setEnabled(current && current->paired && current->pairedThere);
	logButton->setEnabled(anyPaired);
	storedButton->setVisible(controller && controller->StorageNode() != nullptr);
}

QTreeWidgetItem *PeersPanel::Selected() const
{
	const QList<QTreeWidgetItem *> selected = list->selectedItems();
	return selected.isEmpty() ? nullptr : selected.first();
}

std::optional<lan::Peer> PeersPanel::Current() const
{
	std::shared_ptr<lan::Service> svc = controller ? controller->Lan() : nullptr;
	QTreeWidgetItem *item = Selected();
	if (!svc || !item) {
		return std::nullopt;
	}
	return svc->FindPeer(item->data(0, kRoleId).toString());
}

void PeersPanel::Activate()
{
	std::optional<lan::Peer> p = Current();
	if (!p) {
		return;
	}
	if (!p->paired || !p->pairedThere) {
		Pair();
	} else {
		OpenClips();
	}
}

void PeersPanel::Pair()
{
	std::shared_ptr<lan::Service> svc = controller ? controller->Lan() : nullptr;
	std::optional<lan::Peer> p = Current();
	if (svc && p) {
		(new PairDialog(svc, *p, window()))->show();
	}
}

void PeersPanel::OpenClips()
{
	std::shared_ptr<lan::Service> svc = controller ? controller->Lan() : nullptr;
	std::optional<lan::Peer> p = Current();
	if (svc && p && p->paired) {
		(new PeerClipsDialog(svc, *p, window()))->show();
	}
}

void PeersPanel::Forget()
{
	std::shared_ptr<lan::Service> svc = controller ? controller->Lan() : nullptr;
	QTreeWidgetItem *item = Selected();
	if (!svc || !item) {
		return;
	}
	const QString id = item->data(0, kRoleId).toString();
	if (!svc->IsPaired(id)) {
		return;
	}
	if (QMessageBox::question(this, T("Lucida.Peers.Forget"),
				  T("Lucida.Peers.Forget.Confirm").arg(item->text(0))) == QMessageBox::Yes) {
		svc->Forget(id);
	}
}

void PeersPanel::ContextMenu(const QPoint &pos)
{
	QTreeWidgetItem *item = list->itemAt(pos);
	std::shared_ptr<lan::Service> svc = controller ? controller->Lan() : nullptr;
	if (!item || !svc) {
		return;
	}
	list->setCurrentItem(item);
	QMenu menu(this);
	if (pairButton->isEnabled()) {
		menu.addAction(pairButton->text(), this, &PeersPanel::Pair);
	}
	if (clipsButton->isEnabled()) {
		menu.addAction(clipsButton->text(), this, &PeersPanel::OpenClips);
	}
	if (svc->IsPaired(item->data(0, kRoleId).toString())) {
		menu.addAction(T("Lucida.Peers.Forget"), this, &PeersPanel::Forget);
	}
	if (!menu.isEmpty()) {
		menu.exec(list->viewport()->mapToGlobal(pos));
	}
}

void PeersPanel::AddByAddress()
{
	std::shared_ptr<lan::Service> svc = controller ? controller->Lan() : nullptr;
	if (!svc) {
		return;
	}
	bool ok = false;
	const QString text = QInputDialog::getText(this, T("Lucida.Peers.Add.Title"), T("Lucida.Peers.Add.Prompt"),
						   QLineEdit::Normal, QString(), &ok)
				     .trimmed();
	if (!ok || text.isEmpty()) {
		return;
	}
	/* "host" or "host:port" */
	QString host = text;
	quint16 port = lan::kDefaultPort;
	const qsizetype colon = text.lastIndexOf(':');
	if (colon > 0 && text.count(':') == 1) {
		bool numeric = false;
		const int p = text.mid(colon + 1).toInt(&numeric);
		if (numeric && p > 0 && p <= 65535) {
			host = text.left(colon);
			port = (quint16)p;
		}
	}
	addButton->setEnabled(false);
	title->setText(T("Lucida.Peers.Add.Trying").arg(text));
	QPointer<PeersPanel> self(this);
	std::thread([svc, host, port, text, self] {
		const lan::Result r = svc->AddAddress(host, port);
		OnUi(self, [r, text](PeersPanel *panel) {
			panel->addButton->setEnabled(true);
			panel->Refresh();
			if (r.ok) {
				/* select it, ready to pair */
				for (int i = 0; i < panel->list->topLevelItemCount(); i++) {
					QTreeWidgetItem *item = panel->list->topLevelItem(i);
					if (item->data(0, kRoleId).toString() == r.body.value("id").toString()) {
						panel->list->setCurrentItem(item);
					}
				}
			} else {
				QMessageBox::warning(panel, T("Lucida.Peers.Add.Title"),
						     T("Lucida.Peers.Add.Failed").arg(text, r.error));
			}
		});
	}).detach();
}

void PeersPanel::OpenStored()
{
	if (controller && controller->StorageNode()) {
		(new StoredDialog(controller, window()))->show();
	}
}

/* --- what is stored here ------------------------------------------------------------------ */

StoredDialog::StoredDialog(Controller *controller_, QWidget *parent) : QDialog(parent), controller(controller_)
{
	setWindowTitle(T("Lucida.Stored.Title"));
	setAttribute(Qt::WA_DeleteOnClose);
	resize(680, 320);
	list = new QTreeWidget();
	list->setColumnCount(4);
	list->setHeaderLabels({T("Lucida.Stored.Col.Pc"), T("Lucida.Stored.Col.Segments"), T("Lucida.Stored.Col.Size"),
			       T("Lucida.Stored.Col.Newest")});
	list->setRootIsDecorated(false);
	list->setUniformRowHeights(true);
	list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	for (int c = 1; c < 4; c++) {
		list->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
	}
	list->header()->setStretchLastSection(false);
	status = new QLabel();
	status->setWordWrap(true);

	QPushButton *log = new QPushButton(T("Lucida.Stored.OpenLog"));
	log->setToolTip(T("Lucida.Stored.OpenLog.Tip"));
	QPushButton *folder = new QPushButton(T("Lucida.Stored.OpenFolder"));
	QPushButton *close = new QPushButton(T("Lucida.Pair.Close"));
	connect(log, &QPushButton::clicked, this, [this] {
		const QString db = SelectedFolder(true);
		if (db.isEmpty()) {
			return;
		}
		const QString viewer =
			QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("lucida-viewer.exe"));
		if (!QProcess::startDetached(viewer, {QDir::toNativeSeparators(db)})) {
			status->setText(T("Lucida.Stored.NoViewer").arg(QDir::toNativeSeparators(viewer)));
		}
	});
	connect(folder, &QPushButton::clicked, this, [this] {
		const QString dir = SelectedFolder(false);
		if (!dir.isEmpty()) {
			QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
		}
	});
	connect(close, &QPushButton::clicked, this, &QDialog::reject);
	connect(list, &QTreeWidget::itemSelectionChanged, this, [this, log, folder] {
		log->setEnabled(!list->selectedItems().isEmpty() && QFileInfo::exists(SelectedFolder(true)));
		folder->setEnabled(!list->selectedItems().isEmpty());
	});
	log->setEnabled(false);
	folder->setEnabled(false);
	if (controller) {
		connect(controller, &Controller::storedChanged, this, &StoredDialog::Reload);
	}

	QHBoxLayout *buttons = new QHBoxLayout();
	buttons->addWidget(log);
	buttons->addWidget(folder);
	buttons->addStretch(1);
	buttons->addWidget(close);
	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(list, 1);
	layout->addWidget(status);
	layout->addLayout(buttons);
	Reload();
}

void StoredDialog::Reload()
{
	std::shared_ptr<lan::StorageNode> node = controller ? controller->StorageNode() : nullptr;
	if (!node) {
		status->setText(T("Lucida.Stored.Off"));
		list->clear();
		return;
	}
	const QString keep = list->currentItem() ? list->currentItem()->data(0, kRoleId).toString() : QString();
	const QSignalBlocker block(list);
	list->clear();
	const QLocale locale;
	for (const lan::StoredSource &s : node->Sources()) {
		QTreeWidgetItem *item = new QTreeWidgetItem(
			{s.name, QString::number(s.segments), locale.formattedDataSize(s.bytes),
			 s.newest ? QDateTime::fromSecsSinceEpoch(s.newest).toString(QStringLiteral("yyyy-MM-dd HH:mm"))
				  : QString()});
		item->setData(0, kRoleId, s.id);
		item->setData(0, kRoleName, s.folder);
		item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
		item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
		list->addTopLevelItem(item);
		if (s.id == keep) {
			list->setCurrentItem(item);
		}
	}
	status->setText(T("Lucida.Stored.Status")
				.arg(QDir::toNativeSeparators(node->Folder()),
				     locale.formattedDataSize((qint64)node->UsedBytes()),
				     locale.formattedDataSize((qint64)node->Quota())));
}

QString StoredDialog::SelectedFolder(bool log) const
{
	QTreeWidgetItem *item = list->currentItem();
	if (!item) {
		return QString();
	}
	lan::StoredSource s;
	s.folder = item->data(0, kRoleName).toString();
	return log ? s.LogPath() : s.folder;
}

/* --- being asked to pair ---------------------------------------------------------------- */

void AskToPair(QWidget *parent, const lan::PairPrompt &prompt, std::shared_ptr<std::promise<bool>> answer)
{
	auto box = new QMessageBox(
		QMessageBox::Question, T("Lucida.Pair.Asked.Title"),
		T("Lucida.Pair.Asked").arg(prompt.name.toHtmlEscaped(), prompt.fingerprint, prompt.code),
		QMessageBox::NoButton, parent);
	box->setTextFormat(Qt::RichText);
	QPushButton *yes = box->addButton(T("Lucida.Pair.Asked.Yes"), QMessageBox::AcceptRole);
	box->addButton(T("Lucida.Pair.Asked.No"), QMessageBox::RejectRole);
	box->setDefaultButton(static_cast<QPushButton *>(nullptr));
	box->setAttribute(Qt::WA_DeleteOnClose);
	box->setWindowModality(Qt::NonModal);

	auto answered = std::make_shared<bool>(false);
	auto finish = [answer, answered](bool value) {
		if (!*answered) {
			*answered = true;
			answer->set_value(value);
		}
	};
	QObject::connect(box, &QMessageBox::finished, box,
			 [box, yes, finish](int) { finish(box->clickedButton() == yes); });
	QObject::connect(box, &QObject::destroyed, [finish] { finish(false); });

	/* the other side gave up, or nobody answered in time */
	QTimer *poll = new QTimer(box);
	const qint64 until = QDateTime::currentMSecsSinceEpoch() + lan::kPairTimeoutMs;
	QObject::connect(poll, &QTimer::timeout, box, [box, prompt, until, finish] {
		if (prompt.cancelled->load() || QDateTime::currentMSecsSinceEpoch() > until) {
			finish(false);
			box->close();
		}
	});
	poll->start(500);
	box->show();
	box->raise();
	box->activateWindow();
}

/* --- asking to pair ------------------------------------------------------------------- */

PairDialog::PairDialog(std::shared_ptr<lan::Service> service, const lan::Peer &peer_, QWidget *parent)
	: QDialog(parent),
	  peer(peer_)
{
	setWindowTitle(T("Lucida.Pair.Title").arg(peer.name));
	setAttribute(Qt::WA_DeleteOnClose);
	message = new QLabel(T("Lucida.Pair.Connecting").arg(peer.name));
	message->setWordWrap(true);
	message->setMinimumWidth(420);
	code = new QLabel();
	/* a style sheet: OBS's theme would override a font set on the label */
	code->setStyleSheet(QStringLiteral("font-size: 20pt; font-weight: bold;"));
	code->setAlignment(Qt::AlignCenter);
	code->setTextInteractionFlags(Qt::TextSelectableByMouse);
	code->hide();
	matchButton = new QPushButton(T("Lucida.Pair.Match"));
	matchButton->hide();
	cancelButton = new QPushButton(T("Lucida.Pair.Cancel"));
	connect(matchButton, &QPushButton::clicked, this, [this] { Decide(true); });
	connect(cancelButton, &QPushButton::clicked, this, &PairDialog::reject);

	QHBoxLayout *buttons = new QHBoxLayout();
	buttons->addStretch(1);
	buttons->addWidget(matchButton);
	buttons->addWidget(cancelButton);
	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(message);
	layout->addWidget(code);
	layout->addLayout(buttons);

	QPointer<PairDialog> self(this);
	const lan::Peer target = peer;
	std::thread([service, target, self] {
		lan::Result r = service->Pair(target, [self](const QString &code) {
			auto decision = std::make_shared<std::promise<bool>>();
			std::future<bool> answer = decision->get_future();
			QMetaObject::invokeMethod(
				qApp,
				[self, code, decision] {
					if (self && !self->finished) {
						self->ShowCode(code, decision);
					} else {
						decision->set_value(false);
					}
				},
				Qt::QueuedConnection);
			return answer.get();
		});
		OnUi(self, [r](PairDialog *d) { d->Done(r); });
	}).detach();
}

PairDialog::~PairDialog()
{
	if (decision) {
		decision->set_value(false);
	}
}

void PairDialog::ShowCode(const QString &text, std::shared_ptr<std::promise<bool>> decision_)
{
	decision = std::move(decision_);
	code->setText(text);
	code->show();
	message->setText(T("Lucida.Pair.Compare").arg(peer.name));
	matchButton->show();
	matchButton->setDefault(true);
	adjustSize();
}

void PairDialog::Decide(bool match)
{
	if (decision) {
		decision->set_value(match);
		decision.reset();
	}
	matchButton->hide();
	if (match) {
		message->setText(T("Lucida.Pair.Waiting").arg(peer.name));
		cancelButton->setText(T("Lucida.Pair.Close"));
	}
}

void PairDialog::reject()
{
	Decide(false);
	finished = true;
	QDialog::reject();
}

void PairDialog::Done(const lan::Result &result)
{
	finished = true;
	if (decision) {
		decision->set_value(false);
		decision.reset();
	}
	matchButton->hide();
	code->setVisible(result.ok);
	message->setText(result.ok ? T("Lucida.Pair.Done").arg(peer.name)
				   : T("Lucida.Pair.Failed").arg(peer.name, result.error));
	cancelButton->setText(T("Lucida.Pair.Close"));
	adjustSize();
}

/* --- a peer's clips --------------------------------------------------------------------- */

PeerClipsDialog::PeerClipsDialog(std::shared_ptr<lan::Service> service_, const lan::Peer &peer_, QWidget *parent)
	: QDialog(parent),
	  service(std::move(service_)),
	  peer(peer_)
{
	setWindowTitle(T("Lucida.Clips.Title").arg(peer.name));
	setAttribute(Qt::WA_DeleteOnClose);
	resize(760, 480);

	list = new QTreeWidget();
	list->setColumnCount(4);
	list->setHeaderLabels({T("Lucida.Clips.Col.Name"), T("Lucida.Clips.Col.Size"), T("Lucida.Clips.Col.Date"),
			       T("Lucida.Clips.Col.Here")});
	list->setRootIsDecorated(false);
	list->setUniformRowHeights(true);
	list->setSelectionMode(QAbstractItemView::ExtendedSelection);
	list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	for (int c = 1; c < 4; c++) {
		list->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
	}
	list->header()->setStretchLastSection(false);

	status = new QLabel();
	status->setWordWrap(true);
	progress = new QProgressBar();
	progress->setRange(0, 1000);
	progress->hide();

	refreshButton = new QPushButton(T("Lucida.Clips.Refresh"));
	downloadButton = new QPushButton(T("Lucida.Clips.Download"));
	downloadButton->setToolTip(T("Lucida.Clips.Download.Tip").arg(QDir::toNativeSeparators(Folder())));
	openButton = new QPushButton(T("Lucida.Clips.Open"));
	folderButton = new QPushButton(T("Lucida.Clips.Folder"));
	cancelButton = new QPushButton(T("Lucida.Clips.Stop"));
	cancelButton->hide();
	QPushButton *close = new QPushButton(T("Lucida.Pair.Close"));

	connect(refreshButton, &QPushButton::clicked, this, &PeerClipsDialog::Reload);
	connect(downloadButton, &QPushButton::clicked, this, &PeerClipsDialog::Download);
	connect(openButton, &QPushButton::clicked, this, &PeerClipsDialog::OpenSelected);
	connect(folderButton, &QPushButton::clicked, this, [this] {
		QDir().mkpath(Folder());
		QDesktopServices::openUrl(QUrl::fromLocalFile(Folder()));
	});
	connect(cancelButton, &QPushButton::clicked, this, [this] {
		if (cancel) {
			*cancel = true;
		}
	});
	connect(close, &QPushButton::clicked, this, &PeerClipsDialog::reject);
	connect(list, &QTreeWidget::itemSelectionChanged, this, &PeerClipsDialog::UpdateButtons);
	connect(list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
		if (!Downloaded(item->data(0, kRoleName).toString(), item->data(0, kRoleSize).toLongLong()).isEmpty()) {
			OpenSelected();
		} else {
			Download();
		}
	});

	QHBoxLayout *buttons = new QHBoxLayout();
	buttons->addWidget(refreshButton);
	buttons->addStretch(1);
	buttons->addWidget(downloadButton);
	buttons->addWidget(cancelButton);
	buttons->addWidget(openButton);
	buttons->addWidget(folderButton);
	buttons->addWidget(close);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(list, 1);
	layout->addWidget(progress);
	layout->addWidget(status);
	layout->addLayout(buttons);
	Reload();
}

PeerClipsDialog::~PeerClipsDialog()
{
	if (cancel) {
		*cancel = true;
	}
}

void PeerClipsDialog::reject()
{
	if (cancel) {
		*cancel = true; /* what was downloaded so far is kept and resumed next time */
	}
	QDialog::reject();
}

QString PeerClipsDialog::Folder() const
{
	return QDir(ClipsDirectory()).filePath(T("Lucida.Clips.Subfolder").arg(SafeName(peer.name)));
}

QString PeerClipsDialog::Downloaded(const QString &name, qint64 size) const
{
	const QFileInfo f(QDir(Folder()).filePath(name));
	return f.exists() && f.size() == size ? f.absoluteFilePath() : QString();
}

void PeerClipsDialog::UpdateButtons()
{
	const QList<QTreeWidgetItem *> selected = list->selectedItems();
	bool anyHere = false;
	for (QTreeWidgetItem *item : selected) {
		anyHere = anyHere ||
			  !Downloaded(item->data(0, kRoleName).toString(), item->data(0, kRoleSize).toLongLong())
				   .isEmpty();
	}
	refreshButton->setEnabled(!busy);
	downloadButton->setEnabled(!busy && !selected.isEmpty());
	downloadButton->setVisible(!busy);
	cancelButton->setVisible(busy);
	openButton->setEnabled(anyHere);
}

void PeerClipsDialog::Reload()
{
	status->setText(T("Lucida.Clips.Loading").arg(peer.name));
	list->clear();
	UpdateButtons();
	QPointer<PeerClipsDialog> self(this);
	std::shared_ptr<lan::Service> svc = service;
	const lan::Peer target = peer;
	std::thread([svc, target, self] {
		const lan::Result r = svc->Request(target, QJsonObject{{"op", "clips"}});
		OnUi(self, [r](PeerClipsDialog *d) {
			if (!r.ok) {
				d->status->setText(T("Lucida.Clips.Failed").arg(r.error));
				return;
			}
			const QLocale locale;
			for (const QJsonValue &v : r.body.value("clips").toArray()) {
				const QJsonObject c = v.toObject();
				const QString name = c.value("name").toString();
				const qint64 size = c.value("size").toInteger();
				QTreeWidgetItem *item = new QTreeWidgetItem(
					{name, locale.formattedDataSize(size),
					 QDateTime::fromSecsSinceEpoch(c.value("modified").toInteger())
						 .toString(QStringLiteral("yyyy-MM-dd HH:mm")),
					 d->Downloaded(name, size).isEmpty() ? QString() : T("Lucida.Clips.HereYes")});
				item->setData(0, kRoleName, name);
				item->setData(0, kRoleSize, size);
				item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
				d->list->addTopLevelItem(item);
			}
			d->status->setText(
				d->list->topLevelItemCount()
					? T("Lucida.Clips.Count").arg(d->list->topLevelItemCount()).arg(d->peer.name)
					: T("Lucida.Clips.None").arg(d->peer.name));
			d->UpdateButtons();
		});
	}).detach();
}

void PeerClipsDialog::Download()
{
	if (busy) {
		return;
	}
	std::vector<std::pair<QString, qint64>> wanted;
	for (QTreeWidgetItem *item : list->selectedItems()) {
		const QString name = item->data(0, kRoleName).toString();
		const qint64 size = item->data(0, kRoleSize).toLongLong();
		if (Downloaded(name, size).isEmpty()) {
			wanted.emplace_back(name, size);
		}
	}
	if (wanted.empty()) {
		status->setText(T("Lucida.Clips.AlreadyHere"));
		return;
	}
	const QString folder = Folder();
	if (!QDir().mkpath(folder)) {
		status->setText(T("Lucida.Clips.Failed").arg(QDir::toNativeSeparators(folder)));
		return;
	}
	busy = true;
	cancel = std::make_shared<std::atomic<bool>>(false);
	progress->setValue(0);
	progress->show();
	UpdateButtons();

	QPointer<PeerClipsDialog> self(this);
	std::shared_ptr<lan::Service> svc = service;
	std::shared_ptr<std::atomic<bool>> stop = cancel;
	const lan::Peer target = peer;
	std::thread([svc, target, self, stop, folder, wanted] {
		QString error;
		int done = 0;
		for (const auto &[name, size] : wanted) {
			const QString dest = QDir(folder).filePath(name);
			QFile part(dest + QStringLiteral(".part"));
			if (!part.open(QIODevice::ReadWrite)) {
				error = part.errorString();
				break;
			}
			/* a part from an earlier try is resumed */
			qint64 offset = part.size();
			if (offset > size) {
				part.resize(0);
				offset = 0;
			}
			part.seek(offset);
			qint64 got = offset;
			qint64 lastPost = 0;
			const lan::Result r = svc->Request(
				target, QJsonObject{{"op", "clip"}, {"name", name}, {"offset", offset}},
				[&](const QByteArray &chunk) {
					if (part.write(chunk) != chunk.size()) {
						return false;
					}
					got += chunk.size();
					const qint64 now = QDateTime::currentMSecsSinceEpoch();
					if (now - lastPost > 150) {
						lastPost = now;
						const int permille = size > 0 ? (int)(got * 1000 / size) : 0;
						const QLocale locale;
						const QString text = T("Lucida.Clips.Downloading")
									     .arg(name, locale.formattedDataSize(got),
										  locale.formattedDataSize(size));
						OnUi(self, [permille, text](PeerClipsDialog *d) {
							d->progress->setValue(permille);
							d->status->setText(text);
						});
					}
					return !stop->load();
				},
				kDownloadMs);
			part.close();
			if (stop->load()) {
				error = T("Lucida.Clips.Stopped");
				break;
			}
			if (!r.ok || QFileInfo(part.fileName()).size() != r.body.value("size").toInteger()) {
				error = r.ok ? T("Lucida.Clips.Incomplete") : r.error;
				break;
			}
			/* never over a different file of the same name */
			QString final = dest;
			for (int i = 2; QFileInfo::exists(final); i++) {
				const QFileInfo f(dest);
				final = f.dir().filePath(
					QStringLiteral("%1 (%2).%3").arg(f.completeBaseName()).arg(i).arg(f.suffix()));
			}
			if (!QFile::rename(part.fileName(), final)) {
				error = T("Lucida.Clips.Rename").arg(QDir::toNativeSeparators(final));
				break;
			}
			done++;
		}
		OnUi(self, [error, done](PeerClipsDialog *d) {
			d->busy = false;
			d->progress->hide();
			d->status->setText(
				error.isEmpty()
					? T("Lucida.Clips.Done").arg(done).arg(QDir::toNativeSeparators(d->Folder()))
					: T("Lucida.Clips.Failed").arg(error));
			/* mark what is here now */
			for (int i = 0; i < d->list->topLevelItemCount(); i++) {
				QTreeWidgetItem *item = d->list->topLevelItem(i);
				const bool here = !d->Downloaded(item->data(0, kRoleName).toString(),
								 item->data(0, kRoleSize).toLongLong())
							   .isEmpty();
				item->setText(3, here ? T("Lucida.Clips.HereYes") : QString());
			}
			d->UpdateButtons();
		});
	}).detach();
}

void PeerClipsDialog::OpenSelected()
{
	for (QTreeWidgetItem *item : list->selectedItems()) {
		const QString path =
			Downloaded(item->data(0, kRoleName).toString(), item->data(0, kRoleSize).toLongLong());
		if (!path.isEmpty()) {
			QDesktopServices::openUrl(QUrl::fromLocalFile(path));
			return;
		}
	}
}

} // namespace lucida
