#pragma once

#include "lan.hpp"

#include <QDialog>
#include <QPointer>
#include <QWidget>

#include <atomic>
#include <future>
#include <memory>

class QLabel;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace lucida {

class Controller;

/* The other Spectra PCs on the network, in the Chat Log dock: who is there,
 * what they are playing, pairing, their clips and their log */
class PeersPanel : public QWidget {
	Q_OBJECT

public:
	explicit PeersPanel(Controller *controller, QWidget *parent = nullptr);

signals:
	/* show the paired PCs' lines in the log browser */
	void browseLog();

private:
	QPointer<Controller> controller;
	QLabel *title;
	QLabel *hint; /* what to check while nobody has been found */
	QTreeWidget *list;
	QPushButton *pairButton, *clipsButton, *logButton, *addButton, *storedButton;

	void Refresh();
	void UpdateButtons();
	QTreeWidgetItem *Selected() const;
	std::optional<lan::Peer> Current() const;
	void Activate();
	void Pair();
	void OpenClips();
	void Forget();
	void ContextMenu(const QPoint &pos);
	/* Adds a PC broadcasts do not reach, by its address */
	void AddByAddress();
	void OpenStored();
};

/* On a storage PC: the PCs whose recordings and logs are kept here */
class StoredDialog : public QDialog {
	Q_OBJECT

public:
	StoredDialog(Controller *controller, QWidget *parent = nullptr);

private:
	QPointer<Controller> controller;
	QTreeWidget *list;
	QLabel *status;

	void Reload();
	QString SelectedFolder(bool log) const;
};

/* Someone on another PC asks to pair: shows the code, answers through the
 * promise (and closes if they give up) */
void AskToPair(QWidget *parent, const lan::PairPrompt &prompt, std::shared_ptr<std::promise<bool>> answer);

/* Pairing with another PC from here: shows the code to compare, then waits
 * for the other PC's answer */
class PairDialog : public QDialog {
	Q_OBJECT

public:
	PairDialog(std::shared_ptr<lan::Service> service, const lan::Peer &peer, QWidget *parent = nullptr);
	~PairDialog() override;

	void reject() override;

private:
	lan::Peer peer;
	QLabel *message, *code;
	QPushButton *matchButton, *cancelButton;
	std::shared_ptr<std::promise<bool>> decision;
	bool finished = false;

	void ShowCode(const QString &code, std::shared_ptr<std::promise<bool>> decision);
	void Decide(bool match);
	void Done(const lan::Result &result);
};

/* A paired PC's clips folder: download clips to this PC */
class PeerClipsDialog : public QDialog {
	Q_OBJECT

public:
	PeerClipsDialog(std::shared_ptr<lan::Service> service, const lan::Peer &peer, QWidget *parent = nullptr);
	~PeerClipsDialog() override;

	void reject() override;

private:
	std::shared_ptr<lan::Service> service;
	lan::Peer peer;
	QTreeWidget *list;
	QLabel *status;
	QProgressBar *progress;
	QPushButton *refreshButton, *downloadButton, *openButton, *folderButton, *cancelButton;
	std::shared_ptr<std::atomic<bool>> cancel;
	bool busy = false;

	QString Folder() const;
	QString Downloaded(const QString &name, qint64 size) const;
	void Reload();
	void Download();
	void UpdateButtons();
	void OpenSelected();
};

} // namespace lucida
