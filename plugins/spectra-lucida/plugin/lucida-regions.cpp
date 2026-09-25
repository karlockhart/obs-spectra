#include "lucida-regions.hpp"
#include "carnivore.hpp"
#include "lucida-controller.hpp"
#include "lucida-host.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <thread>
#include <utility>

namespace lucida {

namespace {

constexpr int kShots = 200;
constexpr int kColName = 0;
constexpr int kColMode = 1;

const QColor kChatColour(60, 200, 90);
const QColor kHudColour(60, 180, 230);
const QColor kReadColour(255, 190, 40);
const QColor kOtherColour(170, 170, 170);
const QColor kIgnoreColour(230, 60, 60);
const QColor kTestColour(80, 140, 255);

const std::pair<RegionMode, const char *> kModes[] = {
	{RegionMode::Read, "Lucida.Regions.Mode.Read"},
	{RegionMode::Other, "Lucida.Regions.Mode.Other"},
	{RegionMode::Ignore, "Lucida.Regions.Mode.Ignore"},
};

QString T(const char *key)
{
	return Text(key);
}

QColor ModeColour(RegionMode mode)
{
	return mode == RegionMode::Ignore ? kIgnoreColour : mode == RegionMode::Other ? kOtherColour : kReadColour;
}

/* The settings Lucida uses for games without a profile, as a profile */
LucidaProfile FromSettings(const Settings &s)
{
	LucidaProfile p;
	p.carnivore = s.recorder.carnivore;
	p.chatRegion = s.recorder.chatRegion;
	p.hudRegion = s.recorder.hudRegion;
	p.readHud = s.recorder.readHud;
	return p;
}

QString UniqueName(const QString &base, const std::vector<RegionRule> &rules)
{
	auto taken = [&](const QString &n) {
		return n == QLatin1String("chat") || n == kOtherRegion ||
		       std::any_of(rules.begin(), rules.end(), [&](const RegionRule &r) { return r.name == n; });
	};
	QString name = base;
	for (int n = 2; taken(name); n++) {
		name = QStringLiteral("%1 %2").arg(base).arg(n);
	}
	return name;
}

} // namespace

RegionEditor::RegionEditor(Controller *controller_, QWidget *parent) : QDialog(parent), controller(controller_)
{
	setWindowTitle(T("Lucida.Regions.Title"));
	setWindowFlag(Qt::WindowMaximizeButtonHint);
	resize(1280, 800);

	/* profiles */
	profileList = new QListWidget();
	QPushButton *add = new QPushButton(T("Lucida.Regions.New"));
	QPushButton *duplicate = new QPushButton(T("Lucida.Regions.Duplicate"));
	QPushButton *remove = new QPushButton(T("Lucida.Regions.Remove"));
	for (QPushButton *b : {add, duplicate, remove}) {
		b->setAutoDefault(false);
	}
	QHBoxLayout *profileButtons = new QHBoxLayout();
	profileButtons->addWidget(add);
	profileButtons->addWidget(duplicate);
	profileButtons->addWidget(remove);
	QLabel *profilesNote = new QLabel(T("Lucida.Regions.Profiles.Note"));
	profilesNote->setWordWrap(true);
	QVBoxLayout *left = new QVBoxLayout();
	left->addWidget(new QLabel(T("Lucida.Regions.Profiles")));
	left->addWidget(profileList, 1);
	left->addLayout(profileButtons);
	left->addWidget(profilesNote);
	QWidget *leftPanel = new QWidget();
	leftPanel->setLayout(left);

	/* the profile */
	name = new QLineEdit();
	executable = new QLineEdit();
	executable->setToolTip(T("Lucida.Regions.Executable.Tip"));
	executableCheck = new QLabel();
	carnivore = new QCheckBox(T("Lucida.Regions.Carnivore"));
	readHud = new QCheckBox(T("Lucida.Regions.ReadHud"));
	onlyDrawn = new QCheckBox(T("Lucida.Regions.OnlyDrawn"));
	onlyDrawn->setToolTip(T("Lucida.Regions.OnlyDrawn.Tip"));
	QFormLayout *form = new QFormLayout();
	form->addRow(T("Lucida.Regions.Name"), name);
	form->addRow(T("Lucida.Regions.Executable"), executable);
	form->addRow(QString(), executableCheck);
	form->addRow(carnivore);
	form->addRow(readHud);
	form->addRow(onlyDrawn);

	regionTable = new QTableWidget(0, 2);
	regionTable->setHorizontalHeaderLabels({T("Lucida.Regions.Col.Name"), T("Lucida.Regions.Col.Mode")});
	regionTable->horizontalHeader()->setSectionResizeMode(kColName, QHeaderView::Stretch);
	regionTable->horizontalHeader()->setSectionResizeMode(kColMode, QHeaderView::ResizeToContents);
	regionTable->verticalHeader()->hide();
	regionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	regionTable->setSelectionMode(QAbstractItemView::ExtendedSelection);

	auto button = [this](const char *key, auto slot) {
		QPushButton *b = new QPushButton(T(key));
		const QString tip = T((QByteArray(key) + ".Tip").constData());
		if (!tip.endsWith(".Tip")) {
			b->setToolTip(tip);
		}
		b->setAutoDefault(false);
		connect(b, &QPushButton::clicked, this, slot);
		return b;
	};
	QPushButton *draw = button("Lucida.Regions.Draw", [this] { StartDrawing(Drawing::NewRegion); });
	redrawButton = button("Lucida.Regions.Redraw", [this] { StartDrawing(Drawing::Redraw); });
	deleteButton = button("Lucida.Regions.Delete", [this] { DeleteRegions(); });
	mergeButton = button("Lucida.Regions.Merge", [this] { MergeRegions(); });
	QPushButton *chat = button("Lucida.Regions.ChatBox", [this] { StartDrawing(Drawing::Chat); });
	QPushButton *hud = button("Lucida.Regions.HudClock", [this] { StartDrawing(Drawing::Hud); });
	QPushButton *import = button("Lucida.Regions.Import", [this] { ImportLearned(); });
	QGridLayout *regionButtons = new QGridLayout();
	regionButtons->addWidget(draw, 0, 0);
	regionButtons->addWidget(redrawButton, 0, 1);
	regionButtons->addWidget(deleteButton, 0, 2);
	regionButtons->addWidget(mergeButton, 1, 0);
	regionButtons->addWidget(chat, 1, 1);
	regionButtons->addWidget(hud, 1, 2);
	regionButtons->addWidget(import, 2, 0, 1, 3);

	QVBoxLayout *middle = new QVBoxLayout();
	middle->setContentsMargins(0, 0, 0, 0);
	middle->addLayout(form);
	middle->addWidget(regionTable, 1);
	middle->addLayout(regionButtons);
	editor = new QWidget();
	editor->setLayout(middle);

	/* the screenshot */
	image = new spectra::censor::ImageView();
	QPushButton *older = button("Lucida.Regions.Older", [this] { LoadShot(shot + 1); });
	QPushButton *newer = button("Lucida.Regions.Newer", [this] { LoadShot(shot - 1); });
	QPushButton *open = button("Lucida.Regions.OpenImage", [this] { OpenImage(); });
	testButton = button("Lucida.Regions.Test", [this] { Test(); });
	shotLabel = new QLabel();
	QHBoxLayout *shotRow = new QHBoxLayout();
	shotRow->addWidget(older);
	shotRow->addWidget(newer);
	shotRow->addWidget(open);
	shotRow->addWidget(shotLabel, 1);
	shotRow->addWidget(testButton);
	QLabel *legend = new QLabel(T("Lucida.Regions.Legend"));
	legend->setWordWrap(true);
	QVBoxLayout *right = new QVBoxLayout();
	right->setContentsMargins(0, 0, 0, 0);
	right->addWidget(image, 1);
	right->addLayout(shotRow);
	right->addWidget(legend);
	QWidget *rightPanel = new QWidget();
	rightPanel->setLayout(right);

	QSplitter *split = new QSplitter();
	split->addWidget(leftPanel);
	split->addWidget(editor);
	split->addWidget(rightPanel);
	split->setStretchFactor(0, 0);
	split->setStretchFactor(1, 0);
	split->setStretchFactor(2, 1);
	split->setSizes({220, 360, 700});

	status = new QLabel();
	status->setWordWrap(true);
	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	QHBoxLayout *bottom = new QHBoxLayout();
	bottom->addWidget(status, 1);
	bottom->addWidget(buttons);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(split, 1);
	layout->addLayout(bottom);

	connect(add, &QPushButton::clicked, this, &RegionEditor::AddProfile);
	connect(duplicate, &QPushButton::clicked, this, &RegionEditor::DuplicateProfile);
	connect(remove, &QPushButton::clicked, this, &RegionEditor::RemoveProfile);
	connect(profileList, &QListWidget::currentRowChanged, this, &RegionEditor::SelectProfile);
	connect(name, &QLineEdit::textEdited, this, [this](const QString &text) {
		if (QListWidgetItem *item = profileList->currentItem()) {
			item->setText(text);
		}
	});
	connect(executable, &QLineEdit::textChanged, this, &RegionEditor::CheckExecutable);
	connect(regionTable, &QTableWidget::itemSelectionChanged, this, [this] {
		const size_t n = SelectedRows().size();
		redrawButton->setEnabled(n == 1);
		deleteButton->setEnabled(n >= 1);
		mergeButton->setEnabled(n >= 2);
		Redraw();
	});
	connect(regionTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
		if (filling || item->column() != kColName || item->row() >= (int)lucida.regions.size()) {
			return;
		}
		RegionRule &rule = lucida.regions[item->row()];
		const QString text = item->text().trimmed();
		if (text.isEmpty() || text == rule.name) {
			const bool was = std::exchange(filling, true);
			item->setText(rule.name);
			filling = was;
			return;
		}
		std::vector<RegionRule> others = lucida.regions;
		others.erase(others.begin() + item->row());
		rule.name = UniqueName(text, others);
		if (rule.name != text) {
			const bool was = std::exchange(filling, true);
			item->setText(rule.name);
			filling = was;
		}
		Redraw();
	});
	connect(image, &spectra::censor::ImageView::rectDrawn, this, &RegionEditor::RectDrawn);

	QString error;
	if (!profiles.Load(ProfilesPath(), &error)) {
		status->setText(T("Lucida.Regions.LoadFailed").arg(QDir::toNativeSeparators(ProfilesPath()), error));
	}
	if (Store *store = controller ? controller->Reader() : nullptr) {
		for (const FrameInfo &f : store->Frames(kShots)) {
			shots.push_back(f.frame);
		}
	}
	ShowProfiles();
	LoadShot(0);
}

/* --- profiles ------------------------------------------------------------------- */

void RegionEditor::ShowProfiles()
{
	const QSignalBlocker block(profileList);
	profileList->clear();
	for (const GameProfile &p : profiles.profiles) {
		profileList->addItem(p.name);
	}
	current = -1;
	if (profiles.profiles.empty()) {
		editor->setEnabled(false);
		lucida = LucidaProfile();
		ShowProfile();
		status->setText(T("Lucida.Regions.NoProfiles"));
		return;
	}
	profileList->setCurrentRow(0);
	current = 0;
	ShowProfile();
}

void RegionEditor::SelectProfile(int row)
{
	KeepEdits();
	current = row;
	ShowProfile();
}

void RegionEditor::KeepEdits()
{
	if (current < 0 || current >= (int)profiles.profiles.size()) {
		return;
	}
	GameProfile &p = profiles.profiles[current];
	p.name = name->text().trimmed();
	p.executable = executable->text().trimmed();
	lucida.carnivore = carnivore->isChecked();
	lucida.readHud = readHud->isChecked();
	lucida.onlyDrawn = onlyDrawn->isChecked();
	p.SetLucida(lucida);
}

void RegionEditor::ShowProfile()
{
	const bool valid = current >= 0 && current < (int)profiles.profiles.size();
	editor->setEnabled(valid);
	if (valid) {
		const GameProfile &p = profiles.profiles[current];
		lucida =
			p.Lucida().value_or(controller ? FromSettings(controller->CurrentSettings()) : LucidaProfile());
		name->setText(p.name);
		executable->setText(p.executable);
	} else {
		name->clear();
		executable->clear();
	}
	carnivore->setChecked(lucida.carnivore);
	readHud->setChecked(lucida.readHud);
	onlyDrawn->setChecked(lucida.onlyDrawn);
	tested.clear();
	FillRegions();
	CheckExecutable();
}

void RegionEditor::FillRegions()
{
	filling = true;
	regionTable->setRowCount((int)lucida.regions.size());
	for (int row = 0; row < (int)lucida.regions.size(); row++) {
		const RegionRule &rule = lucida.regions[row];
		regionTable->setItem(row, kColName, new QTableWidgetItem(rule.name));
		QComboBox *mode = new QComboBox();
		for (const auto &[m, key] : kModes) {
			mode->addItem(T(key), (int)m);
		}
		mode->setCurrentIndex(mode->findData((int)rule.mode));
		connect(mode, &QComboBox::currentIndexChanged, this, [this, mode] {
			for (int r = 0; r < regionTable->rowCount(); r++) {
				if (regionTable->cellWidget(r, kColMode) == mode && r < (int)lucida.regions.size()) {
					lucida.regions[r].mode = (RegionMode)mode->currentData().toInt();
				}
			}
			Redraw();
		});
		regionTable->setCellWidget(row, kColMode, mode);
	}
	filling = false;
	redrawButton->setEnabled(false);
	deleteButton->setEnabled(false);
	mergeButton->setEnabled(false);
	Redraw();
}

std::vector<int> RegionEditor::SelectedRows() const
{
	std::vector<int> rows;
	for (const QModelIndex &i : regionTable->selectionModel()->selectedRows()) {
		rows.push_back(i.row());
	}
	std::sort(rows.begin(), rows.end());
	return rows;
}

void RegionEditor::AddProfile()
{
	KeepEdits();
	GameProfile p;
	const Settings s = controller ? controller->CurrentSettings() : Settings();
	p.name = profiles.profiles.empty() ? QStringLiteral("FiveM") : T("Lucida.Regions.NewProfile");
	p.executable = profiles.profiles.empty() ? s.targetProcess : QString();
	LucidaProfile l = FromSettings(s);
	l.carnivore = true; /* what regions are for */
	p.SetLucida(l);
	profiles.profiles.push_back(p);
	const QSignalBlocker block(profileList);
	profileList->addItem(p.name);
	profileList->setCurrentRow(profileList->count() - 1);
	current = profileList->count() - 1;
	ShowProfile();
	name->setFocus();
	name->selectAll();
	status->clear();
}

void RegionEditor::DuplicateProfile()
{
	if (current < 0) {
		return;
	}
	KeepEdits();
	GameProfile p = profiles.profiles[current];
	p.name = T("Lucida.Regions.Copy").arg(p.name);
	profiles.profiles.insert(profiles.profiles.begin() + current + 1, p);
	const QSignalBlocker block(profileList);
	profileList->insertItem(current + 1, p.name);
	profileList->setCurrentRow(current + 1);
	current++;
	ShowProfile();
}

void RegionEditor::RemoveProfile()
{
	if (current < 0) {
		return;
	}
	profiles.profiles.erase(profiles.profiles.begin() + current);
	ShowProfiles();
}

void RegionEditor::CheckExecutable()
{
	const QString text = executable->text().trimmed();
	if (!editor->isEnabled()) {
		executableCheck->clear();
	} else if (text.isEmpty()) {
		executableCheck->setText(T("Lucida.Regions.NoExecutable"));
	} else if (!QRegularExpression(text).isValid()) {
		executableCheck->setText(T("Lucida.Regions.BadExecutable"));
	} else {
		executableCheck->clear();
	}
}

/* --- regions -------------------------------------------------------------------- */

void RegionEditor::StartDrawing(Drawing what)
{
	if (shown.isNull()) {
		status->setText(T("Lucida.Regions.NoShots"));
		return;
	}
	if (what == Drawing::Redraw && SelectedRows().size() != 1) {
		return;
	}
	drawing = what;
	image->drawing = true;
	status->setText(T("Lucida.Regions.DrawHint"));
}

void RegionEditor::RectDrawn(const spectra::Rect &rect)
{
	const Drawing what = std::exchange(drawing, Drawing::None);
	image->drawing = false;
	status->clear();
	if (shown.isNull() || what == Drawing::None) {
		return;
	}
	const spectra::Region area = spectra::Region::FromPixels(rect, shown.width(), shown.height());
	switch (what) {
	case Drawing::NewRegion: {
		lucida.regions.push_back({UniqueName(RegionTracker::PlaceName(area), lucida.regions), area});
		FillRegions();
		const int row = (int)lucida.regions.size() - 1;
		regionTable->selectRow(row);
		regionTable->editItem(regionTable->item(row, kColName));
		break;
	}
	case Drawing::Redraw: {
		const std::vector<int> rows = SelectedRows();
		if (rows.size() == 1) {
			lucida.regions[rows[0]].area = area;
		}
		break;
	}
	case Drawing::Chat:
		lucida.chatRegion = area;
		break;
	case Drawing::Hud:
		lucida.hudRegion = area;
		break;
	default:
		break;
	}
	Redraw();
}

void RegionEditor::DeleteRegions()
{
	std::vector<int> rows = SelectedRows();
	for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
		lucida.regions.erase(lucida.regions.begin() + *it);
	}
	FillRegions();
}

void RegionEditor::MergeRegions()
{
	const std::vector<int> rows = SelectedRows();
	if (rows.size() < 2) {
		return;
	}
	RegionRule &into = lucida.regions[rows[0]];
	for (size_t i = 1; i < rows.size(); i++) {
		const spectra::Region &a = lucida.regions[rows[i]].area;
		into.area = {std::min(into.area.left, a.left), std::min(into.area.top, a.top),
			     std::max(into.area.right, a.right), std::max(into.area.bottom, a.bottom)};
	}
	for (auto it = rows.rbegin(); it + 1 != rows.rend(); ++it) {
		lucida.regions.erase(lucida.regions.begin() + *it);
	}
	FillRegions();
	regionTable->selectRow(rows[0]);
}

void RegionEditor::ImportLearned()
{
	Store *store = controller ? controller->Reader() : nullptr;
	if (!store) {
		return;
	}
	int added = 0;
	for (const ScreenRegion &r : store->ScreenRegions()) {
		const bool known = r.name == QLatin1String("chat") ||
				   std::any_of(lucida.regions.begin(), lucida.regions.end(),
					       [&](const RegionRule &d) { return d.name == r.name; });
		if (!known) {
			lucida.regions.push_back({r.name, r.area, RegionMode::Read});
			added++;
		}
	}
	FillRegions();
	status->setText(T("Lucida.Regions.Imported").arg(added));
}

/* --- the screenshot ------------------------------------------------------------- */

void RegionEditor::LoadShot(int index)
{
	if (shots.empty()) {
		image->ClearImage(T("Lucida.Regions.NoShots"));
		overlay.clear();
		shown = QImage();
		shotLabel->clear();
		return;
	}
	index = std::clamp(index, 0, (int)shots.size() - 1);
	const Frame &f = shots[index];
	QImage q(f.path);
	shot = index;
	ShowImage(q, T("Lucida.Regions.Shot")
			     .arg(index + 1)
			     .arg(shots.size())
			     .arg(QDateTime::fromSecsSinceEpoch(f.sortTs).toString(
				     QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
}

void RegionEditor::OpenImage()
{
	const QString path = QFileDialog::getOpenFileName(
		this, T("Lucida.Regions.OpenImage"),
		shot >= 0 && shot < (int)shots.size() ? QFileInfo(shots[shot].path).absolutePath() : QString(),
		QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp)"));
	if (!path.isEmpty()) {
		ShowImage(QImage(path), QFileInfo(path).fileName());
	}
}

void RegionEditor::ShowImage(const QImage &img, const QString &label)
{
	tested.clear();
	shotLabel->setText(label);
	if (img.isNull()) {
		image->ClearImage(T("Lucida.Regions.Unreadable").arg(label));
		overlay.clear();
		shown = QImage();
		return;
	}
	shown = img;
	image->SetImage(img);
	overlay.clear(); /* SetImage emptied the scene */
	Redraw();
}

void RegionEditor::Redraw()
{
	for (QGraphicsItem *item : overlay) {
		image->scene()->removeItem(item);
		delete item;
	}
	overlay.clear();
	if (shown.isNull()) {
		return;
	}
	const int w = shown.width(), h = shown.height();
	auto box = [&](const spectra::Region &area, const QColor &colour, const QString &label, bool selected,
		       bool hatched) {
		const spectra::Rect r = area.ToPixels(w, h);
		QPen pen(colour, selected ? 3 : 2, selected ? Qt::SolidLine : Qt::DashLine);
		pen.setCosmetic(true);
		auto *item = new QGraphicsRectItem(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
		item->setPen(pen);
		if (hatched) {
			item->setBrush(QBrush(colour, Qt::BDiagPattern));
		}
		item->setAcceptedMouseButtons(Qt::NoButton);
		image->scene()->addItem(item);
		overlay.push_back(item);
		auto *text = new QGraphicsSimpleTextItem(label);
		text->setBrush(colour);
		text->setFlag(QGraphicsItem::ItemIgnoresTransformations);
		text->setAcceptedMouseButtons(Qt::NoButton);
		text->setPos(r.x0, r.y0);
		text->setTransform(QTransform::fromTranslate(0, -text->boundingRect().height()));
		image->scene()->addItem(text);
		overlay.push_back(text);
	};

	box(lucida.chatRegion, kChatColour, QStringLiteral("chat"), false, false);
	if (lucida.readHud) {
		box(lucida.hudRegion, kHudColour, T("Lucida.Regions.HudLabel"), false, false);
	}
	const std::vector<int> selected = SelectedRows();
	for (int row = 0; row < (int)lucida.regions.size(); row++) {
		const RegionRule &rule = lucida.regions[row];
		box(rule.area, ModeColour(rule.mode), rule.name,
		    std::find(selected.begin(), selected.end(), row) != selected.end(),
		    rule.mode == RegionMode::Ignore);
	}

	for (const Tested &t : tested) {
		QPen pen(kTestColour, 1);
		pen.setCosmetic(true);
		auto *item = new QGraphicsRectItem(t.rect.x0, t.rect.y0, t.rect.x1 - t.rect.x0, t.rect.y1 - t.rect.y0);
		item->setPen(pen);
		item->setToolTip(QStringLiteral("%1: %2").arg(t.region, t.body));
		image->scene()->addItem(item);
		overlay.push_back(item);
	}
}

void RegionEditor::Test()
{
	if (shown.isNull()) {
		status->setText(T("Lucida.Regions.NoShots"));
		return;
	}
	KeepEdits();
	const spectra::Image img = spectra::censor::FromQImage(shown);
	const LucidaProfile p = lucida;
	std::vector<ScreenRegion> learned;
	if (Store *store = controller ? controller->Reader() : nullptr; store && !p.onlyDrawn) {
		learned = store->ScreenRegions();
	}
	testButton->setEnabled(false);
	status->setText(T("Lucida.Regions.Testing"));

	QPointer<RegionEditor> self(this);
	std::thread([self, img, p, learned]() mutable {
		std::string error;
		std::unique_ptr<spectra::OcrEngine> ocr = spectra::OcrEngine::Create({}, error);
		std::vector<Tested> found;
		if (ocr) {
			RegionTracker tracker(std::move(learned), WithChatBox(p.regions, p.chatRegion), p.onlyDrawn);
			std::vector<spectra::Rect> exclude;
			if (p.readHud) {
				exclude.push_back(p.hudRegion.ToPixels(img.width, img.height));
			}
			for (const spectra::ChatEntry &e :
			     ReadScreen(img, *ocr, tracker, QDateTime::currentSecsSinceEpoch(), exclude)) {
				for (const spectra::Rect &r : e.rects) {
					found.push_back({r, e.region, e.body});
				}
			}
		}
		QMetaObject::invokeMethod(
			qApp,
			[self, found, error]() {
				if (!self) {
					return;
				}
				self->testButton->setEnabled(true);
				if (!error.empty()) {
					self->status->setText(
						T("Lucida.Regions.TestFailed").arg(QString::fromStdString(error)));
					return;
				}
				self->tested = found;
				std::map<QString, int> perRegion;
				QStringList bodies;
				for (const Tested &t : found) {
					if (!bodies.contains(t.body)) {
						bodies << t.body;
						perRegion[t.region]++;
					}
				}
				QStringList parts;
				for (const auto &[region, n] : perRegion) {
					parts << QStringLiteral("%1: %2").arg(region).arg(n);
				}
				self->status->setText(T("Lucida.Regions.Tested")
							      .arg(bodies.size())
							      .arg(parts.join(QStringLiteral(", "))));
				self->Redraw();
			},
			Qt::QueuedConnection);
	}).detach();
}

/* --- saving --------------------------------------------------------------------- */

void RegionEditor::accept()
{
	KeepEdits();
	for (size_t i = 0; i < profiles.profiles.size(); i++) {
		const GameProfile &p = profiles.profiles[i];
		if (p.name.isEmpty() || p.executable.isEmpty() || !QRegularExpression(p.executable).isValid()) {
			profileList->setCurrentRow((int)i);
			QMessageBox::warning(this, windowTitle(), T("Lucida.Regions.Incomplete"));
			return;
		}
	}
	QString error;
	if (!profiles.Save(ProfilesPath(), &error)) {
		QMessageBox::warning(
			this, windowTitle(),
			T("Lucida.Regions.SaveFailed").arg(QDir::toNativeSeparators(ProfilesPath()), error));
		return;
	}
	if (controller) {
		controller->ReloadProfiles();
	}
	QDialog::accept();
}

} // namespace lucida
