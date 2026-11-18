#pragma once

/*
 * Spectra's feature switches. The Full edition can turn its heavier parts
 * off, down to what Ultralight has, without installing another edition.
 *
 * The switches live in the user config, section SpectraFeatures; a feature
 * is on unless its key is false. They are read once, when OBS-Spectra
 * starts, so a change made in the Features dialog takes effect on the next
 * start and the running session never ends up half switched. The Spectra
 * plugins read the same keys (plugins/spectra-vision/obs/spectra-grab/
 * spectra-features.hpp); keep the names in step.
 *
 * A feature this edition was built without is never available, so Lite and
 * Ultralight get the same answers as before, from the same code.
 */

#include <QString>

namespace SpectraFeatures {

enum class Feature {
	Lucida,    /* the chat logger: OCR of the game, the log, the viewer */
	Obscura,   /* chat censoring of screenshots */
	Speech,    /* Whisper: Lucida's transcripts and the Clip Maker's captions */
	Browser,   /* browser docks and new browser sources (CEF) */
	Streaming, /* going live; recording, loop recording and clips stay */
};

constexpr Feature All[] = {Feature::Lucida, Feature::Obscura, Feature::Speech, Feature::Browser, Feature::Streaming};

/* The edition presets: Full has everything this build has */
enum class Preset { Full, Lite, Ultralight, Custom };

/* The config key, e.g. "Lucida" */
const char *Key(Feature feature);

/* Built into this edition */
bool Available(Feature feature);

/* On in this session: available and switched on when OBS-Spectra started */
bool Enabled(Feature feature);

/* What the user has chosen, which may differ from Enabled() until a restart */
bool Configured(Feature feature);
void SetConfigured(Feature feature, bool on);

/* The preset a set of choices matches, and what a preset switches on */
Preset PresetFor(bool (*isOn)(Feature));
bool PresetIncludes(Preset preset, Feature feature);
/* Presets this edition can offer: never more than it was built with */
bool PresetAvailable(Preset preset);

/* Anything that can be switched in this edition at all */
bool AnyAvailable();

QString Name(Feature feature);
QString Description(Feature feature);
QString PresetName(Preset preset);

} // namespace SpectraFeatures
