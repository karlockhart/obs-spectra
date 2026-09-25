#pragma once

#include "profiles.hpp"
#include "store.hpp"

#include <spectra-censor/censor.hpp>

#include <QDialog>
#include <QImage>
#include <QPointer>

class QCheckBox;
class QGraphicsItem;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;

namespace lucida {

class Controller;

/* Game profiles and carnivore mode's regions. A profile is picked by the
 * captured game's executable and says how to read that game: carnivore
 * mode, the chat box, the HUD clock and the regions drawn here (logged
 * under their name, as "other", or ignored). Regions are drawn on a kept
 * screenshot; Test reads the screenshot with the profile and shows where
 * each line would go. */
class RegionEditor : public QDialog {
	Q_OBJECT

public:
	explicit RegionEditor(Controller *controller, QWidget *parent = nullptr);

	void accept() override;

private:
	enum class Drawing { None, NewRegion, Redraw, Chat, Hud };
	struct Tested {
		spectra::Rect rect;
		QString region;
		QString body;
	};

	QPointer<Controller> controller;
	GameProfiles profiles;
	int current = -1;
	LucidaProfile lucida; /* the profile on show, as edited */

	QListWidget *profileList;
	QLineEdit *name, *executable;
	QLabel *executableCheck;
	QCheckBox *carnivore, *readHud, *onlyDrawn;
	QTableWidget *regionTable;
	QPushButton *redrawButton, *deleteButton, *mergeButton, *testButton;
	spectra::censor::ImageView *image;
	QLabel *shotLabel, *status;
	QWidget *editor; /* everything that needs a profile */

	std::vector<Frame> shots;
	int shot = -1;
	QImage shown;
	Drawing drawing = Drawing::None;
	std::vector<QGraphicsItem *> overlay;
	std::vector<Tested> tested;
	bool filling = false;

	void ShowProfiles();
	void SelectProfile(int row);
	void KeepEdits();
	void ShowProfile();
	void FillRegions();
	std::vector<int> SelectedRows() const;

	void AddProfile();
	void DuplicateProfile();
	void RemoveProfile();

	void StartDrawing(Drawing what);
	void RectDrawn(const spectra::Rect &rect);
	void DeleteRegions();
	void MergeRegions();
	void ImportLearned();

	void LoadShot(int index);
	void OpenImage();
	void ShowImage(const QImage &img, const QString &label);
	void Redraw();
	void Test();
	void CheckExecutable();
};

} // namespace lucida
