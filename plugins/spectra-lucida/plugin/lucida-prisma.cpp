#include "lucida-prisma.hpp"
#include "lucida-controller.hpp"
#include "lucida-host.hpp"

#include <obs-module.h>
#include <util/bmem.h>

#include <chrono>

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSysInfo>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

namespace lucida {

namespace {

QString ModuleConfigPath(const char *name)
{
	char *path = obs_module_config_path(name);
	const QString out = QString::fromUtf8(path ? path : "");
	bfree(path);
	return out;
}

/* Spectra's in-game notification overlay (the frontend's spectra_notify proc) */
void Toast(const char *kind, const QString &title, const QString &text, const QString &key)
{
	const QByteArray t = title.toUtf8(), x = text.toUtf8(), k = key.toUtf8();
	calldata_t cd = {0};
	calldata_set_string(&cd, "kind", kind);
	calldata_set_string(&cd, "title", t.constData());
	calldata_set_string(&cd, "text", x.constData());
	calldata_set_string(&cd, "key", k.constData());
	proc_handler_call(obs_get_proc_handler(), "spectra_notify", &cd);
	calldata_free(&cd);
}

QJsonObject ToJson(const VideoExport &v)
{
	QJsonObject o;
	o["kind"] = v.kind;
	o["key"] = v.key;
	o["file"] = v.file;
	o["info"] = v.info;
	return o;
}

VideoExport FromJson(const QJsonObject &o)
{
	VideoExport v;
	v.kind = o.value("kind").toString();
	v.key = o.value("key").toString();
	v.file = o.value("file").toString();
	v.info = o.value("info").toObject();
	return v;
}

void ProcReady(void *, calldata_t *cd)
{
	calldata_set_bool(cd, "ready", PrismaExports::Get().Ready());
}

void ProcExport(void *, calldata_t *cd)
{
	auto str = [cd](const char *name) {
		const char *v = calldata_string(cd, name);
		return QString::fromUtf8(v ? v : "");
	};
	VideoExport v;
	v.kind = str("kind");
	v.file = str("path");
	if ((v.kind != "clips" && v.kind != "loops") || v.file.isEmpty()) {
		return;
	}
	/* only what Prisma takes: mp4, mkv, mov, webm, ts, flv; 1 byte to 64 GiB */
	const qint64 size = QFileInfo(v.file).size();
	if (VideoContentType(v.file).isEmpty() || size < 1 || size > 64ll * 1024 * 1024 * 1024) {
		blog(LOG_WARNING, "[Lucida] Not exporting %s to Prisma: not a video it takes",
		     v.file.toUtf8().constData());
		return;
	}
	v.key = ExportKey(v.file);
	auto set = [&v](const char *key, const QString &value) {
		if (!value.isEmpty()) {
			v.info[key] = value;
		}
	};
	set("title", str("title"));
	set("game", str("game"));
	set("started", str("started"));
	const double duration = calldata_float(cd, "duration");
	if (duration > 0.0) {
		v.info["duration"] = duration;
	}
	const QStringList tags = str("tags").split(',', Qt::SkipEmptyParts);
	if (!tags.isEmpty()) {
		QJsonArray array;
		for (const QString &t : tags) {
			array.append(t.trimmed());
		}
		v.info["tags"] = array;
	}
	PrismaExports::Get().Enqueue(v);
}

} // namespace

/* ------------------------------------------------------------------------- */
/* Exports */

PrismaExports &PrismaExports::Get()
{
	static PrismaExports exports;
	return exports;
}

void PrismaExports::Start(Controller *c)
{
	if (started) {
		return;
	}
	started = true;
	controller = c;
	proc_handler_t *procs = obs_get_proc_handler();
	proc_handler_add(procs, "void spectra_prisma_ready(out bool ready)", ProcReady, nullptr);
	proc_handler_add(procs,
			 "void spectra_prisma_export(in string kind, in string path, in string title, in string game, "
			 "in string started, in float duration, in string tags)",
			 ProcExport, nullptr);
	Load();
	Refresh();
	worker = std::thread([this]() { Run(); });
}

void PrismaExports::Stop()
{
	stopping = true;
	wake.notify_all();
	if (worker.joinable()) {
		worker.join();
	}
}

std::shared_ptr<PrismaClient> PrismaExports::Client()
{
	std::lock_guard<std::mutex> lock(mutex);
	return client;
}

void PrismaExports::Refresh()
{
	/* the controller lives on the UI thread, so this runs there */
	std::shared_ptr<PrismaClient> current = controller ? controller->CloudClient() : nullptr;
	std::lock_guard<std::mutex> lock(mutex);
	client = current;
}

bool PrismaExports::Ready()
{
	Refresh();
	return Client() != nullptr;
}

void PrismaExports::Enqueue(const VideoExport &video)
{
	Refresh();
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (const VideoExport &v : queue) {
			if (v.kind == video.kind && v.key == video.key) {
				return; /* already on its way */
			}
		}
		queue.push_back(video);
	}
	Save();
	blog(LOG_INFO, "[Lucida] Prisma export queued: %s/%s", video.kind.toUtf8().constData(),
	     video.key.toUtf8().constData());
	Toast("upload", Text("Lucida.Prisma.Queued"), QFileInfo(video.file).fileName(), "prisma-export");
	wake.notify_all();
}

void PrismaExports::Save()
{
	QJsonArray array;
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (const VideoExport &v : queue) {
			array.append(ToJson(v));
		}
	}
	const QString path = ModuleConfigPath("prisma-exports.json");
	QDir().mkpath(QFileInfo(path).absolutePath());
	QSaveFile file(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
		file.commit();
	}
}

void PrismaExports::Load()
{
	QFile file(ModuleConfigPath("prisma-exports.json"));
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	std::lock_guard<std::mutex> lock(mutex);
	for (const QJsonValue &v : QJsonDocument::fromJson(file.readAll()).array()) {
		VideoExport video = FromJson(v.toObject());
		if (!video.file.isEmpty() && QFileInfo::exists(video.file)) {
			queue.push_back(video);
		}
	}
	if (!queue.empty()) {
		blog(LOG_INFO, "[Lucida] Resuming %d Prisma export(s)", (int)queue.size());
	}
}

void PrismaExports::Run()
{
	int failures = 0;
	while (!stopping) {
		VideoExport next;
		{
			std::unique_lock<std::mutex> lock(mutex);
			/* after failures, wait a while before trying again */
			const auto delay =
				std::chrono::seconds(failures == 0 ? 0 : std::min(600, 30 << std::min(failures, 4)));
			if (queue.empty()) {
				wake.wait(lock, [this]() { return stopping.load() || !queue.empty(); });
			} else if (failures > 0) {
				wake.wait_for(lock, delay, [this]() { return stopping.load(); });
			}
			if (stopping || queue.empty()) {
				continue;
			}
			next = queue.front();
		}

		std::shared_ptr<PrismaClient> client = Client();
		if (!client) {
			/* not connected: keep the queue for when it is */
			failures = std::max(failures, 3);
			std::unique_lock<std::mutex> lock(mutex);
			wake.wait_for(lock, std::chrono::seconds(60), [this]() { return stopping.load(); });
			continue;
		}
		if (!QFileInfo::exists(next.file)) {
			blog(LOG_WARNING, "[Lucida] Prisma export dropped, the file is gone: %s",
			     next.file.toUtf8().constData());
		} else {
			const QString name = QFileInfo(next.file).fileName();
			blog(LOG_INFO, "[Lucida] Exporting %s to Prisma as %s/%s", name.toUtf8().constData(),
			     next.kind.toUtf8().constData(), next.key.toUtf8().constData());
			int lastPercent = -1;
			const PrismaResult r = ExportVideo(
				*client, next,
				[&](qint64 done, qint64 total) {
					const int percent = total > 0 ? (int)(done * 100 / total) : 0;
					if (percent / 25 != lastPercent / 25) {
						lastPercent = percent;
						blog(LOG_INFO, "[Lucida] Prisma export of %s: %d%%",
						     name.toUtf8().constData(), percent);
					}
				},
				[this]() { return stopping.load(); });
			if (stopping) {
				break; /* still queued: resumes next time */
			}
			if (!r.Ok()) {
				failures++;
				const bool permanent = r.status < 0 || (r.status >= 400 && r.status < 500 && r.status != 401 &&
									r.status != 408 && r.status != 409 && r.status != 429);
				blog(LOG_WARNING, "[Lucida] Prisma export of %s failed: %s%s",
				     name.toUtf8().constData(), r.Describe().toUtf8().constData(),
				     permanent ? " (dropped)" : " (will retry)");
				Toast("error", Text("Lucida.Prisma.Failed"), r.Describe(), "prisma-export");
				if (!permanent) {
					continue;
				}
			} else {
				failures = 0;
				Toast("upload", Text("Lucida.Prisma.Exported"), name, "prisma-export");
			}
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (!queue.empty() && queue.front().key == next.key && queue.front().kind == next.kind) {
				queue.pop_front();
			}
		}
		Save();
	}
}

/* ------------------------------------------------------------------------- */
/* Connecting */

PrismaConnectDialog::PrismaConnectDialog(const QString &currentUrl, QWidget *parent) : QDialog(parent)
{
	setWindowTitle(Text("Lucida.Prisma.Connect.Title"));
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	setMinimumWidth(480);

	url = new QLineEdit(currentUrl);
	url->setPlaceholderText(QStringLiteral("https://prisma.example.com"));
	passphrase = new QLineEdit();
	passphrase->setEchoMode(QLineEdit::Password);
	passphrase->setPlaceholderText(Text("Lucida.Prisma.Connect.PassphraseHint"));
	status = new QLabel();
	status->setWordWrap(true);
	code = new QLabel();
	QFont big = code->font();
	big.setPointSizeF(big.pointSizeF() * 2.2);
	big.setBold(true);
	code->setFont(big);
	code->setAlignment(Qt::AlignCenter);
	code->setTextInteractionFlags(Qt::TextSelectableByMouse);
	code->hide();
	openLink = new QPushButton(Text("Lucida.Prisma.Connect.OpenLink"));
	openLink->hide();
	connect(openLink, &QPushButton::clicked, this, [this]() { QDesktopServices::openUrl(QUrl(link)); });

	auto *note = new QLabel(Text("Lucida.Prisma.Connect.Note"));
	note->setWordWrap(true);
	auto *form = new QFormLayout();
	form->addRow(Text("Lucida.Prisma.Connect.Url"), url);
	form->addRow(Text("Lucida.Prisma.Connect.Passphrase"), passphrase);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
	connectButton = buttons->addButton(Text("Lucida.Prisma.Connect.Connect"), QDialogButtonBox::AcceptRole);
	connect(connectButton, &QPushButton::clicked, this, &PrismaConnectDialog::Connect);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(note);
	layout->addLayout(form);
	layout->addWidget(status);
	layout->addWidget(code);
	layout->addWidget(openLink, 0, Qt::AlignHCenter);
	layout->addWidget(buttons);
}

PrismaConnectDialog::~PrismaConnectDialog()
{
	cancelled = true;
	if (worker.joinable()) {
		worker.join();
	}
}

void PrismaConnectDialog::Busy(bool busy)
{
	connectButton->setEnabled(!busy);
	url->setEnabled(!busy);
	passphrase->setEnabled(!busy);
}

void PrismaConnectDialog::Connect()
{
	const QString base = url->text().trimmed();
	if (!base.startsWith(QLatin1String("https://")) && !base.startsWith(QLatin1String("http://"))) {
		status->setText(Text("Lucida.Prisma.Connect.BadUrl"));
		return;
	}
	if (worker.joinable()) {
		worker.join();
	}
	const QString secret = passphrase->text();
	/* never kept: the passphrase only travels in this one request */
	passphrase->clear();
	Busy(true);
	code->hide();
	openLink->hide();
	status->setText(Text("Lucida.Prisma.Connect.Connecting"));
	cancelled = false;

	PairingInfo info;
	info.machine = QSysInfo::machineHostName();
	info.name = QStringLiteral("Spectra on %1").arg(info.machine);
	info.app = QStringLiteral("spectra");
	info.version = QString::fromUtf8(obs_get_version_string());
	info.scopes = {"spectra:read", "spectra:write", "lucida:read", "lucida:write"};

	QPointer<PrismaConnectDialog> self(this);
	worker = std::thread([this, self, base, secret, info]() {
		const std::array<uint8_t, 32> seed = NewEd25519Seed();
		auto post = [self](const PairingResult &r, bool final) {
			QMetaObject::invokeMethod(
				qApp,
				[self, r, final]() {
					if (!self) {
						return;
					}
					if (final) {
						self->Finish(r);
					} else {
						self->ShowCode(r);
					}
				},
				Qt::QueuedConnection);
		};

		PairingResult r;
		bool useCode = secret.trimmed().isEmpty();
		if (!useCode) {
			r = PairWithPassphrase(base, secret, info, seed);
			if (r.outcome == PairingResult::Outcome::NoPassphrase) {
				useCode = true; /* none is set: an admin approves a code instead */
			} else {
				post(r, true);
				return;
			}
		}

		r = StartCodePairing(base, info, seed);
		if (r.outcome != PairingResult::Outcome::Pending) {
			post(r, true);
			return;
		}
		post(r, false);
		const QString deviceCode = r.deviceCode;
		const int interval = r.interval;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(r.expiresIn);
		while (!cancelled && std::chrono::steady_clock::now() < deadline) {
			for (int i = 0; i < interval * 10 && !cancelled; i++) {
				QThread::msleep(100);
			}
			if (cancelled) {
				return;
			}
			PairingResult poll = PollCodePairing(base, deviceCode, seed);
			if (poll.outcome != PairingResult::Outcome::Pending) {
				post(poll, true);
				return;
			}
		}
		if (!cancelled) {
			PairingResult expired;
			expired.outcome = PairingResult::Outcome::Expired;
			post(expired, true);
		}
	});
}

void PrismaConnectDialog::ShowCode(const PairingResult &r)
{
	link = r.link;
	code->setText(r.userCode);
	code->show();
	openLink->setVisible(!link.isEmpty());
	status->setText(Text("Lucida.Prisma.Connect.Code"));
	if (!link.isEmpty()) {
		QDesktopServices::openUrl(QUrl(link));
	}
}

void PrismaConnectDialog::Finish(const PairingResult &r)
{
	Busy(false);
	code->hide();
	openLink->hide();
	using O = PairingResult::Outcome;
	switch (r.outcome) {
	case O::Paired: {
		const QString path = ModuleConfigPath("prisma-spectra.json");
		QDir().mkpath(QFileInfo(path).absolutePath());
		if (QFileInfo::exists(path)) {
			/* an earlier pairing's key: kept aside, never overwritten */
			const QString old = path + QStringLiteral(".old-") +
					    QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
			QFile::rename(path, old);
		}
		QSaveFile file(path);
		if (!file.open(QIODevice::WriteOnly) || file.write(r.credentialsJson) < 0 || !file.commit()) {
			status->setText(Text("Lucida.Prisma.Connect.WriteFailed").arg(QDir::toNativeSeparators(path)));
			return;
		}
		written = path;
		blog(LOG_INFO, "[Lucida] Paired with Prisma (tenant %s, scopes %s)", r.tenant.toUtf8().constData(),
		     r.scopes.join(' ').toUtf8().constData());
		accept();
		return;
	}
	case O::WrongPassphrase:
		status->setText(Text("Lucida.Prisma.Connect.WrongPassphrase"));
		passphrase->setFocus();
		return;
	case O::Locked:
		status->setText(Text("Lucida.Prisma.Connect.Locked").arg(std::max(1, (r.retryAfter + 59) / 60)));
		return;
	case O::Denied:
		status->setText(Text("Lucida.Prisma.Connect.Denied"));
		return;
	case O::Expired:
		status->setText(Text("Lucida.Prisma.Connect.Expired"));
		return;
	default:
		status->setText(Text("Lucida.Prisma.Connect.Failed").arg(r.error));
		return;
	}
}

} // namespace lucida
